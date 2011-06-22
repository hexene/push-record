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


using namespace android;

class Frame {
private:
    int size;
    status_t ret;
    int64_t time;
    void *buffer;

public:
    Frame(status_t r, int s = 0, int64_t t = 0)
        : ret(r),
          size(s),
          time(t) {
        buffer = new char[size];
    }

    int get_size() {
        return size;
    }

    status_t get_ret() {
        return ret;
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

volatile sig_atomic_t thread_exited = 0;

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
        status_t source_ret, tmp;

        pthread_mutex_lock(&in_mutex);
        while (in_queue.empty())
            pthread_cond_wait(&condition, &in_mutex);

        frame      = *in_queue.begin();
        source_ret = frame->get_ret();

        if (source_ret != OK && source_ret != INFO_FORMAT_CHANGED)
            goto read_end;

        tmp = buf_group.acquire_buffer(buffer);
        if (tmp != OK) {
            fprintf(stderr, "Failed to acquire buffer\n");
            source_ret = tmp;
            goto read_end;
        }

        memcpy((*buffer)->data(), frame->data(), frame->get_size());
        (*buffer)->set_range(0, frame->get_size());
        (*buffer)->meta_data()->clear();
        (*buffer)->meta_data()->setInt64(kKeyTime, frame->get_time());

read_end:
        in_queue.erase(in_queue.begin());
        pthread_mutex_unlock(&in_mutex);
        delete frame;
        return source_ret;
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
    if (extractor == NULL)
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

    Frame* frame;
    CHECK_EQ(decoder->start(), OK);

    MediaBuffer *buffer;
    int64_t time;
    int decode_done = 0;
    do {
        ret = decoder->read(&buffer);
        if (ret != OK && ret != INFO_FORMAT_CHANGED) {
            decode_done = 1;
            frame = new Frame(ret);
            goto decode_end;
        }

        buffer->meta_data()->findInt64(kKeyTime, &time);
        frame = new Frame(ret, buffer->range_length(), time);
        memcpy(frame->data(), buffer->data(), buffer->range_length());

        buffer->release();
        buffer = NULL;
 
decode_end:
        pthread_mutex_lock(&out_mutex);
        out_queue.push_back(frame);
        pthread_mutex_unlock(&out_mutex);
    } while (!decode_done);
    
    ret = decoder->stop();

    if (ret != OK && ret != ERROR_END_OF_STREAM)
        fprintf(stderr, "Decode failed: %d\n", ret);
    
    client.disconnect();
    printf("decode_thread: done\n");
    thread_exited = 1;
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
    int64_t i = 0, j = 0;
    status_t source_ret, decode_ret;
    int source_done = 0, decode_done = 0;

    CHECK_EQ(source->start(), OK);
    do {
        if (source_done) goto decode_listen;

        source_ret = source->read(&buffer);
        if (source_ret != OK && source_ret != INFO_FORMAT_CHANGED) {
            source_done = 1;
            frame = new Frame(source_ret);
            goto source_end;
        }

        buffer->meta_data()->findInt64(kKeyTime, &time);
        frame = new Frame(source_ret, buffer->range_length(), time);
        printf("%lld: Source frame size: %d\n", ++i, buffer->range_length());
        memcpy(frame->data(), buffer->data(), buffer->range_length());

        buffer->release();
        buffer = NULL;

source_end:
        while(true) {
            if (thread_exited) {
                source_done = 1;
                goto decode_listen;
            }
            pthread_mutex_lock(&in_mutex);
            if (in_queue.size() >= 10) {
                pthread_mutex_unlock(&in_mutex);
                usleep(10000);
                continue;
            }
            in_queue.push_back(frame);
            pthread_cond_signal(&condition);
            pthread_mutex_unlock(&in_mutex);
            break;
        }

decode_listen:
        pthread_mutex_lock(&out_mutex);
        if (out_queue.empty()) {
            if (source_done) usleep(10000);
            pthread_mutex_unlock(&out_mutex);
            continue;
        }

        frame = *out_queue.begin();

        printf("%lld: Output frame size: %d\n", ++j, frame->get_size());
        out_queue.erase(out_queue.begin());
        pthread_mutex_unlock(&out_mutex);

        decode_ret = frame->get_ret();
        if (decode_ret != OK && decode_ret != INFO_FORMAT_CHANGED) {
            decode_done = 1;
        }
        delete frame;
    } while (!decode_done);

    source_ret = source->stop();
    if (source_ret != OK && source_ret != ERROR_END_OF_STREAM) {
        fprintf(stderr, "Source read failed: %d\n", source_ret);
        return 2;
    }

    while (in_queue.size()) {
        frame = *in_queue.begin();
        in_queue.erase(in_queue.begin());
        delete frame;
    }

    pthread_join(thread_id, NULL);
}
