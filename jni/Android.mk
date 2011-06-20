LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:=       \
     push_record.cpp

LOCAL_LDFLAGS := -L../lib -Wl,-rpath,../lib -Wl,--allow-shlib-undefined
LOCAL_LDLIBS := -lstagefright -lmedia -lutils -lbinder

LOCAL_C_INCLUDES:= \
    ../android/bionic/libc/include \
    ../android/system/core/include \
    ../android/frameworks/base/include \
    ../android/frameworks/base/media/libstagefright \
    ../android/frameworks/base/include/media/stagefright/openmax 

LOCAL_CFLAGS += -Wno-multichar

LOCAL_MODULE_TAGS := debug

LOCAL_MODULE:= push_record

include $(BUILD_EXECUTABLE)
