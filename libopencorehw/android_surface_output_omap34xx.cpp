/*
 * OMAP3430 support
 *
 * Author: Srini Gosangi <srini.gosangi@windriver.com>
 * Author: Michael Barabanov <michael.barabanov@windriver.com>

 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 */

/* ------------------------------------------------------------------
 * Copyright (C) 2008 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */


//#define LOG_NDEBUG 0
#ifdef LOG_NDEBUG
#warning LOG_NDEBUG ##LOG_NDEBUG##
#endif

#define LOG_TAG "VideoMio34xx"
#include <utils/Log.h>

#include <cutils/properties.h>
#define UNLIKELY( exp ) (__builtin_expect( (exp) != 0, false ))
static int mDebugFps = 0;

#include "android_surface_output_omap34xx.h"
#include "pv_mime_string_utils.h"
#include "pvmf_video.h"
#include <media/PVPlayer.h>

extern "C" {
#include "v4l2_utils.h"
}

#define CACHEABLE_BUFFERS 0x1
/* FIXME - duplicate in liboverlay/v4l2_utils.h */
#define NUM_OVERLAY_BUFFERS_REQUESTED  (3)

using namespace android;

enum wrd_state_e {
    WRD_STATE_UNUSED,
    WRD_STATE_UNQUEUED,
    WRD_STATE_INDSSQUEUE
};

typedef struct WriteResponseData_t {
     PvmiCapabilityContext aContext;
     PVMFTimestamp aTimestamp;
     PVMFCommandId cmdid;
     void *ptr;
     enum wrd_state_e state;
} WriteResponseData;

static WriteResponseData sWriteRespData[NUM_OVERLAY_BUFFERS_REQUESTED];

static void convertYuv420ToYuv422(int width, int height, void* src, void* dst);

OSCL_EXPORT_REF AndroidSurfaceOutputOmap34xx::AndroidSurfaceOutputOmap34xx() :
    AndroidSurfaceOutput()
{
    mUseOverlay = true;
    mOverlay = NULL;
    mbufferAlloc.buffer_address = NULL;
    mConvert = false;
    mBuffersQueuedToDSS = 0;
    /* the v4l2 overlay holds 2 decoder output buffers */
    mNumberOfFramesToHold = 2;
}

OSCL_EXPORT_REF AndroidSurfaceOutputOmap34xx::~AndroidSurfaceOutputOmap34xx()
{
    mUseOverlay = false;
}

OSCL_EXPORT_REF bool AndroidSurfaceOutputOmap34xx::initCheck()
{
    LOGV("Calling Vendor(34xx) Specific initCheck");
    mInitialized = false;
    // reset flags in case display format changes in the middle of a stream
    resetVideoParameterFlags();
    bufEnc = 0;
    mBuffersQueuedToDSS = 0;

    // copy parameters in case we need to adjust them
    int displayWidth = iVideoDisplayWidth;
    int displayHeight = iVideoDisplayHeight;
    int frameWidth = iVideoWidth;
    int frameHeight = iVideoHeight;
    int frameSize;
    int videoFormat = OVERLAY_FORMAT_CbYCrY_422_I;
    mapping_data_t *data;
    LOGV("Use Overlays");

    if (mUseOverlay) {
        if(mOverlay == NULL){
            LOGV("using Vendor Specific(34xx) codec");
            sp<OverlayRef> ref = NULL;
            // FIXME:
            // Surfaceflinger may hold onto the previous overlay reference for some
            // time after we try to destroy it. retry a few times. In the future, we
            // should make the destroy call block, or possibly specify that we can
            // wait in the createOverlay call if the previous overlay is in the
            // process of being destroyed.
            for (int retry = 0; retry < 50; ++retry) {
                //FIXME: frameWidth vs displayWidth ?
                // ref = mSurface->createOverlay(frameWidth, frameHeight, videoFormat, 0);
                ref = mSurface->createOverlay(displayWidth, displayHeight, videoFormat, 0);
                if (ref != NULL) break;
                LOGD("Overlay create failed - retrying");
                usleep(100000);
            }
            if ( ref.get() == NULL )
            {
                LOGE("Overlay Creation Failed!");
                return mInitialized;
            }
            mOverlay = new Overlay(ref);
            mOverlay->setParameter(CACHEABLE_BUFFERS, 0);

            for (int i=0; i<NUM_OVERLAY_BUFFERS_REQUESTED; i++)
                sWriteRespData[i].state = WRD_STATE_UNUSED;
        }
        else
        {
            //FIXME: frameWidth vs displayWidth ?
            // mOverlay->resizeInput(frameWidth, frameHeight);
            mOverlay->resizeInput(displayWidth, displayHeight);
        }
        LOGI("Actual resolution: %dx%d", frameWidth, frameHeight);
        LOGI("Video resolution: %dx%d", iVideoWidth, iVideoHeight);
#if 0
        mbufferAlloc.maxBuffers = mOverlay->getBufferCount();
        mbufferAlloc.bufferSize = iBufferSize;
        mbufferAlloc.buffer_address = new uint8*[mbufferAlloc.maxBuffers];
        if (mbufferAlloc.buffer_address == NULL) {
            LOGE("unable to allocate mem for overlay addresses");
            return mInitialized;
        }
        LOGV("number of buffers = %d\n", mbufferAlloc.maxBuffers);
        for (int i = 0; i < mbufferAlloc.maxBuffers; i++) {
            data = (mapping_data_t *)mOverlay->getBufferAddress((void*)i);
            mbufferAlloc.buffer_address[i] = (uint8*)data->ptr;
            strcpy((char *)mbufferAlloc.buffer_address[i], "hello");
            if (strcmp((char *)mbufferAlloc.buffer_address[i], "hello")) {
                LOGI("problem with buffer\n");
                return mInitialized;
            }else{
                LOGV("buffer = %d allocated addr=%#lx\n", i, (unsigned long) mbufferAlloc.buffer_address[i]);
            }
        }        
#endif
    }
    mInitialized = true;
    LOGV("sendEvent(MEDIA_SET_VIDEO_SIZE, %d, %d)", iVideoDisplayWidth, iVideoDisplayHeight);
    mPvPlayer->sendEvent(MEDIA_SET_VIDEO_SIZE, iVideoDisplayWidth, iVideoDisplayHeight);

    char value[PROPERTY_VALUE_MAX];
    property_get("debug.video.showfps", value, "0");
    mDebugFps = atoi(value);
    LOGV_IF(mDebugFps, "showfps enabled");

    return mInitialized;
}

static void debugShowFPS()
{
    static int mFrameCount = 0;
    static int mLastFrameCount = 0;
    static nsecs_t mLastFpsTime = 0;
    static float mFps = 0;
    mFrameCount++;
    if (!(mFrameCount & 0x1F)) {
        nsecs_t now = systemTime();
        nsecs_t diff = now - mLastFpsTime;
        mFps = ((mFrameCount - mLastFrameCount) * float(s2ns(1))) / diff;
        mLastFpsTime = now;
        mLastFrameCount = mFrameCount;
        LOGD("%d Frames, %f FPS", mFrameCount, mFps);
    }
    // XXX: mFPS has the value we want
}


PVMFCommandId  AndroidSurfaceOutputOmap34xx::writeAsync(uint8 aFormatType, int32 aFormatIndex, uint8* aData, uint32 aDataLen,
                                         const PvmiMediaXferHeader& data_header_info, OsclAny* aContext)
{
    bool bDequeueFail = false;
    bool bQueueFail = false;

    // Do a leave if MIO is not configured except when it is an EOS
    if (!iIsMIOConfigured &&
        !((PVMI_MEDIAXFER_FMT_TYPE_NOTIFICATION == aFormatType)
          && (PVMI_MEDIAXFER_FMT_INDEX_END_OF_STREAM == aFormatIndex)))
    {
        LOGE("data is pumped in before MIO is configured");
        OSCL_LEAVE(OsclErrInvalidState);
        return -1;
    }

    uint32 aSeqNum = data_header_info.seq_num;
    PVMFTimestamp aTimestamp = data_header_info.timestamp;
    uint32 flags = data_header_info.flags;
    PVMFCommandId cmdid = iCommandCounter++;

    if (aSeqNum < 6) {
        LOGV("AndroidSurfaceOutputOmap34xx::writeAsync() seqnum %d ts %d context %d",aSeqNum,aTimestamp, (int)aContext);
        LOGV("AndroidSurfaceOutputOmap34xx::writeAsync() Format Type %d Format Index %d length %d",aFormatType,aFormatIndex,aDataLen);
    }

    PVMFStatus status=PVMFFailure;

    switch (aFormatType) {
    case PVMI_MEDIAXFER_FMT_TYPE_COMMAND :
        LOGD("AndroidSurfaceOutputOmap34xx::writeAsync() called with Command info.");
        //ignore
        status= PVMFSuccess;
        break;

    case PVMI_MEDIAXFER_FMT_TYPE_NOTIFICATION :
        LOGD("AndroidSurfaceOutputOmap34xx::writeAsync() called with Notification info.");
        switch (aFormatIndex) {
        case PVMI_MEDIAXFER_FMT_INDEX_END_OF_STREAM:
            iEosReceived = true;
            break;
        default:
            break;
        }
        //ignore
        status= PVMFSuccess;
        break;

    case PVMI_MEDIAXFER_FMT_TYPE_DATA :
        switch (aFormatIndex) {
        case PVMI_MEDIAXFER_FMT_INDEX_FMT_SPECIFIC_INFO:
            //format-specific info contains codec headers.
            LOGD("AndroidSurfaceOutputOmap34xx::writeAsync() called with format-specific info.");

            if (iState<STATE_INITIALIZED) {
                LOGE("AndroidSurfaceOutputOmap34xx::writeAsync: Error - Invalid state");
                status = PVMFErrInvalidState;
            } else {
                status = PVMFSuccess;
            }
            break;

        case PVMI_MEDIAXFER_FMT_INDEX_DATA:
            //data contains the media bitstream.

            //Verify the state
            if (iState != STATE_STARTED) {
                LOGE("AndroidSurfaceOutputOmap34xx::writeAsync: Error - Invalid state");
                status = PVMFErrInvalidState;
            } else {
                int idx;

                // Call playback to send data to IVA for Color Convert
                if (mUseOverlay) {
                    // Convert from YUV420 to YUV422 for software codec
                    if (mConvert) {
                        LOGE("YUV420 to YUV422 conversion not supported");
                        //convertYuv420ToYuv422(iVideoWidth, iVideoHeight, aData, mbufferAlloc.buffer_address[bufEnc]);
                        status = PVMFFailure;
                        break;
                    }
#if 0
                    else if (data_header_info.nOffset != 0) {
                        LOGE("received buffer with non-zero offset");
                        status = PVMFFailure;
                        break;
                    }
#endif
                }

                for (idx = 0; idx < NUM_OVERLAY_BUFFERS_REQUESTED; idx++) {
                    if (sWriteRespData[idx].state == WRD_STATE_UNUSED)
                        break;
                }

                if (idx == NUM_OVERLAY_BUFFERS_REQUESTED) {
                    LOGE("writeAsync(): DSS Queue is full");
                    status = PVMFFailure;
                    WriteResponse resp(status, cmdid, aContext, aTimestamp);
                    iWriteResponseQueue.push_back(resp);
                    RunIfNotReady();
                    return cmdid;
                }
                LOGV("AndroidSurfaceOutputOmap34xx::writeAsync: Saving context, index=%d", idx);

                sWriteRespData[idx].aContext = aContext;
                sWriteRespData[idx].aTimestamp  = aTimestamp;
                sWriteRespData[idx].cmdid = cmdid;
                sWriteRespData[idx].ptr = aData;
                sWriteRespData[idx].state = WRD_STATE_UNQUEUED;

                bDequeueFail = false;
                bQueueFail = false;

                status = writeFrameBuf(aData, aDataLen, data_header_info, idx);
                switch (status) {
                    case PVMFSuccess:
                        LOGV("writeFrameBuf Success");
                        break;
                    case PVMFErrArgument:
                        bQueueFail = true;
                        LOGW("Queue FAIL from writeFrameBuf");
                        break;
                    case PVMFErrInvalidState:
                        bDequeueFail = true;
                        LOGI("Dequeue FAIL from writeFrameBuf");
                        break;
                    case PVMFFailure:
                        bDequeueFail = true;
                        bQueueFail = true;
                        LOGW("Queue & Dequeue FAIL");
                        break;
                    default: //Compiler requirement
                        LOGE("No such case!!!!!!!!!");
                    break;
                }
                LOGV("AndroidSurfaceOutputOmap34xx::writeAsync: Playback Progress - frame %lu",iFrameNumber++);
            }
            break;

        default:
            LOGE("AndroidSurfaceOutputOmap34xx::writeAsync: Error - unrecognized format index");
            status = PVMFFailure;
            break;
        }
        break;

    default:
        LOGE("AndroidSurfaceOutputOmap34xx::writeAsync: Error - unrecognized format type");
        status = PVMFFailure;
        break;
    }

    //Schedule asynchronous response
    if (iEosReceived) {
        for (int i = 0; i < NUM_OVERLAY_BUFFERS_REQUESTED; i++) {
            if (sWriteRespData[i].state == WRD_STATE_INDSSQUEUE) {
                mBuffersQueuedToDSS--;
                sWriteRespData[i].state = WRD_STATE_UNUSED;

                WriteResponse resp(status,
                                sWriteRespData[i].cmdid,
                                sWriteRespData[i].aContext,
                                sWriteRespData[i].aTimestamp);
                iWriteResponseQueue.push_back(resp);
                RunIfNotReady();
                //Don't return cmdid here
            }
        }
    }
    else if (bQueueFail) {
        //Send default response
    }
    else if (bDequeueFail) {
        status = PVMFFailure; //Set proper error for the caller.
    }
    else if (bDequeueFail == false) {
        status = PVMFSuccess; //Clear posible error while queueing
        WriteResponse resp(status,
                           sWriteRespData[iDequeueIndex].cmdid,
                           sWriteRespData[iDequeueIndex].aContext,
                           sWriteRespData[iDequeueIndex].aTimestamp);
        iWriteResponseQueue.push_back(resp);
        RunIfNotReady();
        return cmdid;
    }

    WriteResponse resp(status, cmdid, aContext, aTimestamp);
    iWriteResponseQueue.push_back(resp);
    RunIfNotReady();

    return cmdid;
}


PVMFStatus AndroidSurfaceOutputOmap34xx::writeFrameBuf(uint8* aData, uint32 aDataLen, const PvmiMediaXferHeader& data_header_info, int aIndex)
{
    LOGV(" calling Vendor Specific(34xx) writeFrameBuf call");
    PVMFStatus eStatus = PVMFSuccess;
    int idx = 0;
    int nError = 0;
    int nActualBuffersInDSS = 0;

    if (mSurface == 0) return PVMFFailure;

    if (UNLIKELY(mDebugFps)) {
        debugShowFPS();
    }

    if (mUseOverlay == 0) return PVMFSuccess;
    
    nActualBuffersInDSS = mOverlay->queueBuffer(sWriteRespData[aIndex].ptr);
    if (nActualBuffersInDSS < 0) {
        LOGE("overlay queue buffer returns %d ", nActualBuffersInDSS);
        eStatus = PVMFErrArgument; // Queue failed
    }
    else { // Queue succeeded
        mBuffersQueuedToDSS++;
        sWriteRespData[aIndex].state = WRD_STATE_INDSSQUEUE;

        // This code will make sure that whenever a Stream OFF occurs in overlay...
        // a response is sent for each of the buffer.. so that buffers are not lost 
        if (mBuffersQueuedToDSS != nActualBuffersInDSS) {
            for (idx = 0; idx < NUM_OVERLAY_BUFFERS_REQUESTED; idx++) {
                if (idx == aIndex) {
                    continue;
                }
                if (sWriteRespData[idx].state == WRD_STATE_INDSSQUEUE) {
                    LOGD("Sending dequeue response for buffer %d", idx);
                    mBuffersQueuedToDSS--;
                    sWriteRespData[idx].state = WRD_STATE_UNUSED;
                    WriteResponse resp(PVMFFailure,
                                    sWriteRespData[idx].cmdid,
                                    sWriteRespData[idx].aContext,
                                    sWriteRespData[idx].aTimestamp);
                    iWriteResponseQueue.push_back(resp);
                    RunIfNotReady();
                }
                else {
                    LOGD("Skip buffer %d, not in DSS", idx);
                }
            }
        }
    }

    overlay_buffer_t overlay_buffer;
    nError = mOverlay->dequeueBuffer(&overlay_buffer);
    if (nError == NO_ERROR) {
        int idx;

        for (idx = 0; idx < NUM_OVERLAY_BUFFERS_REQUESTED; idx++) {
            if (sWriteRespData[idx].ptr == (uint8 *)overlay_buffer)
                break;
        }
        if (idx == NUM_OVERLAY_BUFFERS_REQUESTED) {
            LOGE("Overlay dequeued buffer is not in the record");
            goto exit;
        }

        iDequeueIndex = idx;
        mBuffersQueuedToDSS--;
        sWriteRespData[iDequeueIndex].state = WRD_STATE_UNUSED;

        if (eStatus == PVMFSuccess)
            return PVMFSuccess; // both Queue and Dequeue succeeded
        else
            return PVMFErrArgument; // Only Queue failed
    }

exit:
    if (eStatus == PVMFSuccess)
        return PVMFErrInvalidState; // Only Dequeue failed
    else
        return PVMFFailure; // both Queue and Dequeue failed
}


//#define USE_BUFFER_ALLOC 1

/* based on test code in pvmi/media_io/pvmiofileoutput/src/pvmi_media_io_fileoutput.cpp */
void AndroidSurfaceOutputOmap34xx::setParametersSync(PvmiMIOSession aSession,
        PvmiKvp* aParameters,
        int num_elements,
        PvmiKvp * & aRet_kvp)
{
    OSCL_UNUSED_ARG(aSession);
    aRet_kvp = NULL;

#ifndef USE_BUFFER_ALLOC
    AndroidSurfaceOutput::setParametersSync(aSession, aParameters, num_elements, aRet_kvp);
    return;
#endif

    for (int32 i = 0;i < num_elements;i++)
    {
        if (pv_mime_strcmp(aParameters[i].key, MOUT_VIDEO_FORMAT_KEY) == 0)
        {
            iVideoFormatString=aParameters[i].value.pChar_value;
            iVideoFormat=iVideoFormatString.get_str();
            LOGV("AndroidSurfaceOutputOmap34xx::setParametersSync() Video Format Key, Value %s",iVideoFormatString.get_str());
        }
        else if (pv_mime_strcmp(aParameters[i].key, PVMF_FORMAT_SPECIFIC_INFO_KEY_YUV) == 0)
        {
            uint8* data = (uint8*)aParameters->value.key_specific_value;
            PVMFYuvFormatSpecificInfo0* yuvInfo = (PVMFYuvFormatSpecificInfo0*)data;

            iVideoWidth = (int32)yuvInfo->width;
            iVideoParameterFlags |= VIDEO_WIDTH_VALID;
            LOGV("AndroidSurfaceOutputOmap34xx::setParametersSync() Video Width, Value %d", iVideoWidth);

            iVideoHeight = (int32)yuvInfo->height;
            iVideoParameterFlags |= VIDEO_HEIGHT_VALID;
            LOGV("AndroidSurfaceOutputOmap34xx::setParametersSync() Video Height, Value %d", iVideoHeight);

            iVideoDisplayHeight = (int32)yuvInfo->display_height;
            iVideoParameterFlags |= DISPLAY_HEIGHT_VALID;
            LOGV("AndroidSurfaceOutputOmap34xx::setParametersSync() Video Display Height, Value %d", iVideoDisplayHeight);


            iVideoDisplayWidth = (int32)yuvInfo->display_width;
            iVideoParameterFlags |= DISPLAY_WIDTH_VALID;
            LOGV("AndroidSurfaceOutputOmap34xx::setParametersSync() Video Display Width, Value %d", iVideoDisplayWidth);

            iNumberOfBuffers = (int32)yuvInfo->num_buffers;
            LOGV("AndroidSurfaceOutputOmap34xx::setParametersSync() Number of Buffer, Value %d", iNumberOfBuffers);

            iBufferSize = (int32)yuvInfo->buffer_size;
            LOGV("AndroidSurfaceOutputOmap34xx::setParametersSync() Buffer Size, Value %d", iBufferSize);

            LOGV("Ln %d video_format %s", __LINE__, yuvInfo->video_format.getMIMEStrPtr() );
            iVideoSubFormat = yuvInfo->video_format.getMIMEStrPtr();
            iVideoParameterFlags |= VIDEO_SUBFORMAT_VALID;
        }
        else
        {
            //if we get here the key is unrecognized.
            LOGV("AndroidSurfaceOutputOmap34xx::setParametersSync() Error, unrecognized key %s ", aParameters[i].key);

            //set the return value to indicate the unrecognized key
            //and return.
            aRet_kvp = &aParameters[i];
            return;
        }
    }
    /* Copy Code from base class. Ideally we'd just call base class's setParametersSync, but we can't as it will not get to initCheck if it encounters an unrecognized parameter such as the one we're handling here */
    uint32 mycache = iVideoParameterFlags ;
    if( checkVideoParameterFlags() ) {
        initCheck();
    }
    iVideoParameterFlags = mycache;
    if(!iIsMIOConfigured && checkVideoParameterFlags() )
    {
        iIsMIOConfigured = true;
        if(iObserver)
        {
            iObserver->ReportInfoEvent(PVMFMIOConfigurationComplete);
        }
    }
}

/* based on test code in pvmi/media_io/pvmiofileoutput/src/pvmi_media_io_fileoutput.cpp */
PVMFStatus AndroidSurfaceOutputOmap34xx::getParametersSync(PvmiMIOSession aSession, PvmiKeyType aIdentifier,
        PvmiKvp*& aParameters, int& num_parameter_elements,
        PvmiCapabilityContext aContext)
{
#ifdef USE_BUFFER_ALLOC
    OSCL_UNUSED_ARG(aSession);
    OSCL_UNUSED_ARG(aContext);
    aParameters=NULL;

    if (strcmp(aIdentifier, PVMF_BUFFER_ALLOCATOR_KEY) == 0)
    {
        if( iVideoSubFormat != PVMF_MIME_YUV422_INTERLEAVED_UYVY ) {
            LOGV("Ln %d iVideoSubFormat %s. do NOT allocate decoder buffer from overlay", __LINE__, iVideoSubFormat.getMIMEStrPtr() );
            OSCL_LEAVE(OsclErrNotSupported);
            return PVMFErrNotSupported;
        }

        int32 err;
        aParameters = (PvmiKvp*)oscl_malloc(sizeof(PvmiKvp));
        if (!aParameters)
        {
            return PVMFErrNoMemory;
        }
        aParameters[0].value.key_specific_value = (PVInterface*)&mbufferAlloc;
        return PVMFSuccess;
    }

#endif
    return AndroidSurfaceOutput::getParametersSync(aSession, aIdentifier, aParameters, num_parameter_elements, aContext);
}

// post the last video frame to refresh screen after pause
void AndroidSurfaceOutputOmap34xx::postLastFrame()
{
    //do nothing here, this is only for override the Android_Surface_output::PostLastFrame()
    LOGV("AndroidSurfaceOutputOmap34xx::postLastFrame()");
    //mSurface->postBuffer(mOffset);
}

void AndroidSurfaceOutputOmap34xx::closeFrameBuf()
{
    LOGV("Vendor(34xx) Specific CloseFrameBuf");
    if (UNLIKELY(mDebugFps)) {
        debugShowFPS();
    }
    if (mOverlay != NULL){
        mOverlay->destroy();
        mOverlay = NULL;
    }
    if (mbufferAlloc.buffer_address) {
        delete [] mbufferAlloc.buffer_address;
        mbufferAlloc.buffer_address = NULL;
    }
    if (!mInitialized) return;
    mInitialized = false;
}

// return a byte offset from any pointer
static inline void* byteOffset(void* p, size_t offset) { return (void*)((uint8_t*)p + offset); }

static void convertYuv420ToYuv422(int width, int height, void* src, void* dst)
{

    // calculate total number of pixels, and offsets to U and V planes
    int pixelCount = height * width;
    int srcLineLength = width / 4;
    int destLineLength = width / 2;
    uint32_t* ySrc = (uint32_t*) src;
    uint16_t* uSrc = (uint16_t*) byteOffset(src, pixelCount);
    uint16_t* vSrc = (uint16_t*) byteOffset(uSrc, pixelCount >> 2);
    uint32_t *p = (uint32_t*) dst;

    // convert lines
    for (int i = 0; i < height; i += 2) {

        // upsample by repeating the UV values on adjacent lines
        // to save memory accesses, we handle 2 adjacent lines at a time
        // convert 4 pixels in 2 adjacent lines at a time
        for (int j = 0; j < srcLineLength; j++) {

            // fetch 4 Y values for each line
            uint32_t y0 = ySrc[0];
            uint32_t y1 = ySrc[srcLineLength];
            ySrc++;

            // fetch 2 U/V values
            uint32_t u = *uSrc++;
            uint32_t v = *vSrc++;

            // assemble first U/V pair, leave holes for Y's
            uint32_t uv = (u | (v << 16)) & 0x00ff00ff;

            // OR y values and write to memory
            p[0] = ((y0 & 0xff) << 8) | ((y0 & 0xff00) << 16) | uv;
            p[destLineLength] = ((y1 & 0xff) << 8) | ((y1 & 0xff00) << 16) | uv;
            p++;

            // assemble second U/V pair, leave holes for Y's
            uv = ((u >> 8) | (v << 8)) & 0x00ff00ff;

            // OR y values and write to memory
            p[0] = ((y0 >> 8) & 0xff00) | (y0 & 0xff000000) | uv;
            p[destLineLength] = ((y1 >> 8) & 0xff00) | (y1 & 0xff000000) | uv;
            p++;
        }

        // skip the next y line, we already converted it
        ySrc += srcLineLength;
        p += destLineLength;
    }
}


// factory function for playerdriver linkage
extern "C" AndroidSurfaceOutputOmap34xx* createVideoMio()
{
    LOGV("Creating Vendor(34xx) Specific MIO component");
    return new AndroidSurfaceOutputOmap34xx();
}
