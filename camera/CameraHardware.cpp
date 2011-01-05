/*
**
** Copyright (C) 2009 0xlab.org - http://0xlab.org/
** Copyright 2008, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "CameraHardware"
#include <utils/Log.h>

#include "CameraHardware.h"
#include "converter.h"
#include <fcntl.h>
#include <sys/mman.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define VIDEO_DEVICE        "/dev/video0"
#define PREVIEW_WIDTH        320
#define PREVIEW_HEIGHT       240
#define PIXEL_FORMAT        V4L2_PIX_FMT_YUYV

#include <cutils/properties.h>
#ifndef UNLIKELY
#define UNLIKELY(exp) (__builtin_expect( (exp) != 0, false ))
#endif
static int mDebugFps = 0;

namespace android {

wp<CameraHardwareInterface> CameraHardware::singleton;

/* 29/12/10 : preview/picture size validation logic */
const char CameraHardware::supportedPictureSizes [] = "1600x1200,1024x768,640x480,352x288,320x240";
const char CameraHardware::supportedPreviewSizes [] = "1600x1200,1024x768,640x480,352x288,320x240";

const supported_resolution CameraHardware::supportedPictureRes[] = {{1600, 1200} ,
																	{1024, 768} ,
																	{640, 480} ,
																	{352, 288} ,
																	{320, 240} };
const supported_resolution CameraHardware::supportedPreviewRes[] = {{1600, 1200} ,
																																		{1024, 768} ,
																																		{640, 480} ,
																																		{352, 288} ,
																																		{320, 240} };


CameraHardware::CameraHardware()
                  : mParameters(),
                    mHeap(0),
                    mPreviewHeap(0),
					 mRawHeap(0),
                    mCamera(0),
                    mPreviewFrameSize(0),
                    mNotifyCb(0),
                    mDataCb(0),
                    mDataCbTimestamp(0),
                    mCallbackCookie(0),
                    mMsgEnabled(0),
                    previewStopped(true)
{
    initDefaultParameters();

    /* whether prop "debug.camera.showfps" is enabled or not */
    char value[PROPERTY_VALUE_MAX];
    property_get("debug.camera.showfps", value, "0");
    mDebugFps = atoi(value);
    LOGD_IF(mDebugFps, "showfps enabled");
}

void CameraHardware::initDefaultParameters()
{
    CameraParameters p;

    p.setPreviewSize(PREVIEW_WIDTH, PREVIEW_HEIGHT);
    p.setPreviewFrameRate(DEFAULT_FRAME_RATE);
    p.setPreviewFormat("yuv422sp");

    p.setPictureSize(PREVIEW_WIDTH, PREVIEW_HEIGHT);
    p.setPictureFormat("jpeg");

    p.set("jpeg-quality", "100"); // maximum quality
    p.set("picture-size-values", CameraHardware::supportedPictureSizes);

    if (setParameters(p) != NO_ERROR) {
        LOGE("Failed to set default parameters?!");
    }
}

CameraHardware::~CameraHardware()
{
    delete mCamera;
    mCamera = 0; // paranoia
    singleton.clear();
}

sp<IMemoryHeap> CameraHardware::getPreviewHeap() const
{
    LOGE("return Preview Heap");
    return mPreviewHeap;
}

sp<IMemoryHeap> CameraHardware::getRawHeap() const
{
    LOGE("return Raw Heap");
    return mRawHeap;
}

void CameraHardware::setCallbacks(notify_callback notify_cb,
                                  data_callback data_cb,
                                  data_callback_timestamp data_cb_timestamp,
                                  void* user)
{
    Mutex::Autolock lock(mLock);
    mNotifyCb = notify_cb;
    mDataCb = data_cb;
    mDataCbTimestamp = data_cb_timestamp;
    mCallbackCookie = user;
}

void CameraHardware::enableMsgType(int32_t msgType)
{
    Mutex::Autolock lock(mLock);
    mMsgEnabled |= msgType;
}

void CameraHardware::disableMsgType(int32_t msgType)
{
    Mutex::Autolock lock(mLock);
    mMsgEnabled &= ~msgType;
}

bool CameraHardware::msgTypeEnabled(int32_t msgType)
{
    Mutex::Autolock lock(mLock);
    return (mMsgEnabled & msgType);
}

bool CameraHardware::validateSize(size_t width, size_t height, const supported_resolution *supRes, size_t count)
{
    bool ret = false;
    status_t stat = NO_ERROR;
    unsigned int size;

    if ( NULL == supRes ) {
        LOGE("Invalid resolutions array passed");
        stat = -EINVAL;
    }

    if ( NO_ERROR == stat ) {
        for ( unsigned int i = 0 ; i < count; i++ ) {
            LOGD( "Validating %d, %d and %d, %d", supRes[i].width, width, supRes[i].height, height);
            if ( ( supRes[i].width == width ) && ( supRes[i].height == height ) ) {
                ret = true;
                break;
            }
        }
    }
    return ret;
}

// ---------------------------------------------------------------------------
static void showFPS(const char *tag)
{
    static int mFrameCount = 0;
    static int mLastFrameCount = 0;
    static nsecs_t mLastFpsTime = 0;
    static float mFps = 0;
    mFrameCount++;
    if (!(mFrameCount & 0x1F)) {
        nsecs_t now = systemTime();
        nsecs_t diff = now - mLastFpsTime;
        mFps =  ((mFrameCount - mLastFrameCount) * float(s2ns(1))) / diff;
        mLastFpsTime = now;
        mLastFrameCount = mFrameCount;
        LOGD("[%s] %d Frames, %f FPS", tag, mFrameCount, mFps);
    }
}

int CameraHardware::previewThread()
{
    Mutex::Autolock lock(mPreviewLock);
    if (!previewStopped) {

      mCamera->GrabRawFrame(mRawHeap->getBase());

        mRecordingLock.lock();
        if (mMsgEnabled & CAMERA_MSG_VIDEO_FRAME) {
            yuyv422_to_yuv420sp((unsigned char *)mRawHeap->getBase(),
                                (unsigned char *)mRecordingHeap->getBase(),
                                mPreviewWidth, mPreviewHeight);

            mDataCb(CAMERA_MSG_VIDEO_FRAME, mRecordingBuffer, mCallbackCookie);
            mDataCbTimestamp(systemTime(), CAMERA_MSG_VIDEO_FRAME, mRecordingBuffer, mCallbackCookie);
        }
        mRecordingLock.unlock();

        if (mMsgEnabled & CAMERA_MSG_PREVIEW_FRAME) {
            mCamera->convert((unsigned char *) mRawHeap->getBase(),
                             (unsigned char *) mPreviewHeap->getBase(),
                             mPreviewWidth, mPreviewHeight);

            yuyv422_to_yuv420sp((unsigned char *)mRawHeap->getBase(),
                              (unsigned char *)mHeap->getBase(),
                              mPreviewWidth, mPreviewHeight);
            mDataCb(CAMERA_MSG_PREVIEW_FRAME, mBuffer, mCallbackCookie);
        }

        if (UNLIKELY(mDebugFps)) {
            showFPS("Preview");
        }
    }
    return NO_ERROR;
}

status_t CameraHardware::startPreview()
{
    int width, height;
    int ret;
    if(!mCamera) {
        delete mCamera;
        mCamera = new V4L2Camera();
    }

    Mutex::Autolock lock(mPreviewLock);
    if (mPreviewThread != 0) {
        return INVALID_OPERATION;
    }

	LOGD("startPreview :opening device!!!!,width:%d,height:%d",mPreviewWidth,mPreviewHeight);

	if(mPreviewWidth <=0 || mPreviewHeight <=0)
	{
		LOGE("Preview size is not valid,aborting..Device can not open!!!");
		return INVALID_OPERATION;
	}

    if (mCamera->Open(VIDEO_DEVICE, mPreviewWidth, mPreviewHeight, PIXEL_FORMAT) < 0) {
        LOGE("startPreview failed: cannot open device.");
        return UNKNOWN_ERROR;
    }

    mPreviewFrameSize = mPreviewWidth * mPreviewHeight * 2;

    mHeap = new MemoryHeapBase(mPreviewFrameSize);
    mBuffer = new MemoryBase(mHeap, 0, mPreviewFrameSize);

    mPreviewHeap = new MemoryHeapBase(mPreviewFrameSize);
    mPreviewBuffer = new MemoryBase(mPreviewHeap, 0, mPreviewFrameSize);

    mRecordingHeap = new MemoryHeapBase(mPreviewFrameSize);
    mRecordingBuffer = new MemoryBase(mRecordingHeap, 0, mPreviewFrameSize);

   mRawHeap = new MemoryHeapBase(mPreviewFrameSize);
    mRawBuffer = new MemoryBase(mRawHeap, 0, mPreviewFrameSize);
    ret = mCamera->Init();
    if (ret) {
        LOGE("Camera Init fail: %s", strerror(errno));
        return UNKNOWN_ERROR;
    }

    ret = mCamera->StartStreaming();
    if (ret) {
        LOGE("Camera StartStreaming fail: %s", strerror(errno));
        return UNKNOWN_ERROR;
    }

    previewStopped = false;
    mPreviewThread = new PreviewThread(this);

    return NO_ERROR;
}

void CameraHardware::stopPreview()
{
    sp<PreviewThread> previewThread;
    { // scope for the lock
        Mutex::Autolock lock(mPreviewLock);
        previewStopped = true;
    }

    if (mPreviewThread != 0) {
        mCamera->Uninit();
        mCamera->StopStreaming();
        mCamera->Close();
    }

    {
        Mutex::Autolock lock(mPreviewLock);
        previewThread = mPreviewThread;
    }

    // don't hold the lock while waiting for the thread to quit
    if (previewThread != 0) {
        previewThread->requestExitAndWait();
    }

    Mutex::Autolock lock(mPreviewLock);
    mPreviewThread.clear();
}

bool CameraHardware::previewEnabled()
{
    return mPreviewThread != 0;
}

status_t CameraHardware::startRecording()
{
    LOGE("startRecording");
    mRecordingLock.lock();
    mRecordingEnabled = true;
    mRecordingLock.unlock();
    return NO_ERROR;

}

void CameraHardware::stopRecording()
{
    LOGE("stopRecording");
    mRecordingLock.lock();
    mRecordingEnabled = false;
    mRecordingLock.unlock();
}

bool CameraHardware::recordingEnabled()
{
    return mRecordingEnabled == true;
}

void CameraHardware::releaseRecordingFrame(const sp<IMemory>& mem)
{
    if (UNLIKELY(mDebugFps)) {
        showFPS("Recording");
    }
    return;
}

// ---------------------------------------------------------------------------

int CameraHardware::beginAutoFocusThread(void *cookie)
{
    CameraHardware *c = (CameraHardware *)cookie;
    LOGE("beginAutoFocusThread");
    return c->autoFocusThread();
}

int CameraHardware::autoFocusThread()
{
    if (mMsgEnabled & CAMERA_MSG_FOCUS)
        mNotifyCb(CAMERA_MSG_FOCUS, true, 0, mCallbackCookie);
    return NO_ERROR;
}

status_t CameraHardware::autoFocus()
{
    Mutex::Autolock lock(mLock);
    if (createThread(beginAutoFocusThread, this) == false)
        return UNKNOWN_ERROR;
    return NO_ERROR;
}

status_t CameraHardware::cancelAutoFocus()
{
    return NO_ERROR;
}

int CameraHardware::beginPictureThread(void *cookie)
{
    CameraHardware *c = (CameraHardware *)cookie;
    LOGE("begin Picture Thread");
    return c->pictureThread();
}

int CameraHardware::pictureThread()
{
    LOGE("Picture Thread");
    if (mMsgEnabled & CAMERA_MSG_SHUTTER) {
		// TODO: Don't notify SHUTTER for now, avoid making camera service
		// register preview surface to picture size.
	}

    // Just as stopPreview function
    sp<PreviewThread> previewThread;
    {
        Mutex::Autolock lock(mLock);
        previewThread = mPreviewThread;
        previewStopped = true;
    }

    if(previewThread != 0) {
        previewThread->requestExitAndWait();
    }

    {
        Mutex::Autolock lock(mLock);
        mPreviewThread.clear();
    }

    if (mMsgEnabled & CAMERA_MSG_RAW_IMAGE) {
        LOGE("Take Picture RAW IMAGE");
		// TODO: post a RawBuffer may be needed.
    }

    if (mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE) {
        LOGE("Take Picture COMPRESSED IMAGE");
        mDataCb(CAMERA_MSG_COMPRESSED_IMAGE, mCamera->GrabJpegFrame(), mCallbackCookie);
    }

    previewStopped = false;
    LOGE("%s: preview started", __FUNCTION__);
    mPreviewThread = new PreviewThread(this);

    return NO_ERROR;
}

status_t CameraHardware::takePicture()
{
    pictureThread();
    return NO_ERROR;
}

status_t CameraHardware::cancelPicture()
{
    return NO_ERROR;
}

status_t CameraHardware::dump(int fd, const Vector<String16>& args) const
{
    return NO_ERROR;
}

status_t CameraHardware::setParameters(const CameraParameters& params)
{
    Mutex::Autolock lock(mLock);
	int width  = 0;
	int height = 0;
	params.getPreviewSize(&width,&height);

	LOGD("Set Parameter...!! ");

	/* validate preview size */
	params.getPreviewSize(&width, &height);
	LOGD("preview width:%d,height:%d",width,height);

	if ( validateSize(width, height, supportedPreviewRes, ARRAY_SIZE(supportedPreviewRes)) == false ) {
        LOGE("Preview size not supported");
        return -EINVAL;
    }
	/* set valid preview size */
	mPreviewWidth  = width;
	mPreviewHeight = height;

    /* validate picture size */
	params.getPictureSize(&width, &height);
	LOGD("picture width:%d,height:%d",width,height);

	if (validateSize(width, height, supportedPictureRes, ARRAY_SIZE(supportedPictureRes)) == false ) {
        LOGE("Picture size not supported");
        return -EINVAL;
    }


	/* validate preview & picture format */
	LOGD("Preview Format:%s,Picture Format:%s",params.getPreviewFormat(),params.getPictureFormat());
    if (strcmp(params.getPreviewFormat(), "yuv422sp") != 0) {
        LOGE("Only yuv422sp preview is supported");
        return -1;
    }

    if (strcmp(params.getPictureFormat(), "jpeg") != 0) {
        LOGE("Only jpeg still pictures are supported");
        return -1;
    }

    mParameters = params;

    return NO_ERROR;
}

CameraParameters CameraHardware::getParameters() const
{
    CameraParameters params;

    {
        Mutex::Autolock lock(mLock);
        params = mParameters;
    }

    return params;
}

status_t CameraHardware::sendCommand(int32_t cmd, int32_t arg1, int32_t arg2)
{
    return BAD_VALUE;
}

void CameraHardware::release()
{
}

sp<CameraHardwareInterface> CameraHardware::createInstance()
{
    if (singleton != 0) {
        sp<CameraHardwareInterface> hardware = singleton.promote();
        if (hardware != 0) {
            return hardware;
        }
    }
    sp<CameraHardwareInterface> hardware(new CameraHardware());
    singleton = hardware;
    return hardware;
}

extern "C" sp<CameraHardwareInterface> openCameraHardware()
{
    return CameraHardware::createInstance();
}

}; // namespace android
