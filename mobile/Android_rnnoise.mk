LOCAL_PATH := $(GZDOOM_TOP_PATH)/rnnoise

include $(CLEAR_VARS)

LOCAL_MODULE := rnnoise
LOCAL_SRC_FILES := denoise.c rnn.c rnn_data.c rnn_reader.c pitch.c kiss_fft.c celt_lpc.c
LOCAL_C_INCLUDES := $(LOCAL_PATH)
LOCAL_CFLAGS := -DRNNOISE_BUILD

include $(BUILD_STATIC_LIBRARY)
