LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

# Set up the OpenCore variables.
include external/opencore/Config.mk
LOCAL_C_INCLUDES := \
    $(PV_INCLUDES) \
	external/ti-dsp/dvsdk_3_01_00_06/omx_ti/include

LOCAL_SRC_FILES := \
	surface-dmai.cpp

LOCAL_CFLAGS := -Wno-non-virtual-dtor -DENABLE_SHAREDFD_PLAYBACK -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -DUSE_CML2_CONFIG -DHAS_OSCL_LIB_SUPPORT

LOCAL_SHARED_LIBRARIES := \
    libutils \
    libcutils \
    libui \
    libhardware\
    libandroid_runtime \
    libmedia \
    libopencore_common \
    libicuuc \
    libopencore_player

# do not prelink
LOCAL_PRELINK_MODULE := false

LOCAL_MODULE := libopencoredmai

LOCAL_LDLIBS += 

include $(BUILD_SHARED_LIBRARY)
