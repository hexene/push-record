#include <binder/ProcessState.h>
#include <media/stagefright/FileSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaDebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MPEG4Writer.h>
#include <media/stagefright/OMXClient.h>
#include <media/stagefright/OMXCodec.h>
#include <include/MPEG4Extractor.h>


#define DECODE_ONLY 1

using namespace android;

class Frame {
private:
    int size;
    int64_t time;
    void *buffer;
public:
    Frame(int s, int64_t t = 0)
        : size(s),
          time(t) {
        buffer = new char[size];
    }

    int get_size() {
        return size;
    }

    int64_t get_time() {
        return time;
    }

    void* data() {
        return buffer;
    }

    ~Frame() {
        delete[] (char*)buffer;
    }
};

List<Frame*> in_queue;
List<Frame*> out_queue;

pthread_mutex_t in_mutex  = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t out_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t condition  = PTHREAD_COND_INITIALIZER;

class CustomSource : public MediaSource {

public:
    CustomSource(sp<MetaData> meta, int width, int height)
        : frame_size((width * height * 3) / 2) {
        source_meta = meta;
        buf_group.add_buffer(new MediaBuffer(frame_size));
    }

    virtual sp<MetaData> getFormat() {
        return source_meta;
    }

    virtual status_t start(MetaData *params) {
        return OK;
    }

    virtual status_t stop() {
        return OK;
    }

    virtual status_t read(MediaBuffer **buffer,
                          const MediaSource::ReadOptions *options) {
        Frame *frame;
        status_t ret = OK;


        pthread_mutex_lock(&in_mutex);
        while (in_queue.empty())
            pthread_cond_wait(&condition, &in_mutex);

        frame = *in_queue.begin();
        if (!frame->get_size()) {
            ret = ERROR_END_OF_STREAM;
            goto end;
        }
        ret = buf_group.acquire_buffer(buffer);
        if (ret != OK) {
            fprintf(stderr, "Failed to acquire buffer\n");
            goto end;
        } 

        memcpy((*buffer)->data(), frame->data(), frame->get_size());
        (*buffer)->set_range(0, frame->get_size());
        (*buffer)->meta_data()->clear();
        (*buffer)->meta_data()->setInt64(kKeyTime, frame->get_time());

    end:
        delete frame;
        in_queue.erase(in_queue.begin());
        pthread_mutex_unlock(&in_mutex);

        return ret;
    }
    
private:
    MediaBufferGroup buf_group;
    sp<MetaData> source_meta;
    int frame_size;
};

sp<MediaSource> custom_source;

sp<MediaSource> createSource(const char *filename) {
    sp<MediaSource> source;

    sp<MediaExtractor> extractor = 
        MediaExtractor::Create(new FileSource(filename));
    if(extractor == NULL)
        return NULL;

    size_t num_tracks = extractor->countTracks();

    sp<MetaData> meta;
    for (size_t i = 0; i < num_tracks; i++) {
        meta = extractor->getTrackMetaData(i);
        CHECK(meta.get() != NULL);

        const char *mime;
        if (!meta->findCString(kKeyMIMEType, &mime))
            continue;

        if (strncasecmp(mime, "video/", 6))
            continue;

        source = extractor->getTrack(i);
        break;
    }

    return source;
}

void* decode_thread(void* arg)
{
    OMXClient client;
    status_t ret;

    CHECK_EQ(client.connect(), OK);

    sp<MetaData> meta = custom_source->getFormat();
    sp<MediaSource> decoder = OMXCodec::Create(client.interface(), meta,
                                               false, custom_source, NULL,
                                               OMXCodec::kClientNeedsFramebuffer);
#if DECODE_ONLY
    Frame* frame;
    CHECK_EQ(decoder->start(), OK);
    
    MediaBuffer *buffer;
    int64_t time;
    while ((ret = decoder->read(&buffer)) == OK) {
//        printf("decode_thread: Got an output frame of size %d\n", buffer->range_length());

        buffer->meta_data()->findInt64(kKeyTime, &time);
        frame = new Frame(buffer->range_length(), time);
        memcpy(frame->data(), buffer->data(), buffer->range_length());
        
        pthread_mutex_lock(&out_mutex);
        out_queue.push_back(frame);
        pthread_mutex_unlock(&out_mutex);

        buffer->release();
        buffer = NULL;
    }

    client.disconnect();
    
    frame = new Frame(0);

    pthread_mutex_lock(&out_mutex);
    out_queue.push_back(frame);
    pthread_mutex_unlock(&out_mutex);

    if (ret != ERROR_END_OF_STREAM)
        fprintf(stderr, "Decode failed: %d\n", ret);
#else
    int width, height;
    bool success = meta->findInt32(kKeyWidth, &width);
    success = success && meta->findInt32(kKeyHeight, &height);
    CHECK(success);

    sp<MetaData> enc_meta = new MetaData;
    
//    enc_meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_H263);
//    enc_meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_MPEG4);
    enc_meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_AVC);
    enc_meta->setInt32(kKeyWidth, width);
    enc_meta->setInt32(kKeyHeight, height);
    enc_meta->setInt32(kKeySampleRate, 24);
    enc_meta->setInt32(kKeyBitRate, 512 * 1024);
    enc_meta->setInt32(kKeyStride, width);
    enc_meta->setInt32(kKeySliceHeight, height);
    enc_meta->setInt32(kKeyIFramesInterval, 1);
    enc_meta->setInt32(kKeyColorFormat, OMX_COLOR_FormatYUV420Planar);

    sp<MediaSource> encoder = OMXCodec::Create(client.interface(), enc_meta, true,
                              decoder, NULL,
                              OMXCodec::kClientNeedsFramebuffer);

    sp<MPEG4Writer> writer = new MPEG4Writer("/sdcard/output.mp4");
    writer->addSource(encoder);
    writer->setMaxFileDuration(10000000LL);
    CHECK_EQ(OK, writer->start());
    while (!writer->reachedEOS()) {
        fprintf(stderr, ".");
        usleep(100000);
    }
    ret = writer->stop();
    
    if (ret != OK && ret != ERROR_END_OF_STREAM)
        fprintf(stderr, "Encode failed: %d\n", ret);
#endif        
}

int main(int argc, char **argv)
{
    android::ProcessState::self()->startThreadPool();
    DataSource::RegisterDefaultSniffers();
   
    sp<MediaSource> source = createSource(argv[1]);
    if (source == NULL) {
        fprintf(stderr, "Unable to find a video track\n");
        return 1;
    }

    sp<MetaData> meta = source->getFormat();

    int width, height;
    CHECK(meta->findInt32(kKeyWidth, &width) &&
          meta->findInt32(kKeyHeight, &height));

    custom_source = new CustomSource(meta, width, height);

    pthread_t thread_id;
    pthread_create(&thread_id, NULL, &decode_thread, NULL);

    MediaBuffer *buffer;
    Frame *frame;
    int64_t time;

    CHECK_EQ(source->start(), OK);
    status_t ret;

    while ((ret = source->read(&buffer)) == OK) {
        buffer->meta_data()->findInt64(kKeyTime, &time);
        frame = new Frame(buffer->range_length(), time);
        
        memcpy(frame->data(), buffer->data(), buffer->range_length());

        pthread_mutex_lock(&in_mutex);
        in_queue.push_back(frame);
        pthread_cond_signal(&condition);
        pthread_mutex_unlock(&in_mutex);

        buffer->release();
        buffer = NULL;

//        sleep(1);
#if DECODE_ONLY
        if (out_queue.empty())
            continue;
            
        pthread_mutex_lock(&out_mutex);
        frame = *out_queue.begin();

        printf("main: Got an output frame of size: %d\n", frame->get_size());
        out_queue.erase(out_queue.begin());
        pthread_mutex_unlock(&out_mutex);
        
        if (!frame->get_size()) {
             delete frame;
             break;
        }
        delete frame;
#endif
    }

    if (ret == ERROR_END_OF_STREAM) {
        frame = new Frame(0);

        pthread_mutex_lock(&in_mutex);
        in_queue.push_back(frame);
        pthread_cond_signal(&condition);
        pthread_mutex_unlock(&in_mutex);
        printf("Last frame\n");
        
#if DECODE_ONLY
        while (!out_queue.empty()) {
            pthread_mutex_lock(&out_mutex);
            frame = *out_queue.begin();

            printf("main: Got an output frame of size: %d\n", frame->get_size());
            out_queue.erase(out_queue.begin());
            pthread_mutex_unlock(&out_mutex);
        
            delete frame;
        }
#endif
    } else {
        fprintf(stderr, "Source read failed\n");
        return 2;
    }

    pthread_join(thread_id, NULL);
}
