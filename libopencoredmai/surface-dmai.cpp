/*******************************************************************************
 * surface-dmai.cpp
 *
 * Definition of android surface output used in conjunction with DMAI overlays
 *
 * Based on TI OMAP3430 support code.
 *
 * Copyright (C) 2010 Konstantin Kozhevnikov <konstantin.kozhevnikov@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ******************************************************************************/

#define MODULE_TAG                      SURFACE

/*******************************************************************************
 * Includes
 ******************************************************************************/

#include "omx-dsp.h"
#include "surface-dmai.h"

TRACE_TAG (INIT, 1);
TRACE_TAG (INFO, 1);
TRACE_TAG (DATA, 1);
TRACE_TAG (ERROR, 1);

/*******************************************************************************
 * Local constants definitions
 ******************************************************************************/

#define CACHEABLE_BUFFERS               0x1

/*******************************************************************************
 * Interface definition
 ******************************************************************************/

using namespace android;

/*******************************************************************************
 * AndroidSurfaceDmai::AndroidSurfaceDmai
 *
 * Class constructor
 ******************************************************************************/

AndroidSurfaceDmai::AndroidSurfaceDmai() : AndroidSurfaceOutput()
{
    mOverlay = NULL;

    TRACE (INIT, _b("Component constructed: %p"), this);
}

/*******************************************************************************
 * AndroidSurfaceDmai::~AndroidSurfaceDmai
 *
 * Class destructor
 ******************************************************************************/

AndroidSurfaceDmai::~AndroidSurfaceDmai()
{
    TRACE (INIT, _b("Component deleted: %p (overlay: %d)"), this, (mOverlay == NULL ? 0 : 1));
}

/*******************************************************************************
 * Frame buffer interface definition
 ******************************************************************************/

/*******************************************************************************
 * AndroidSurfaceDmai::initCheck
 *
 * Check the videoformat and other stuff... (tbd)
 ******************************************************************************/

bool AndroidSurfaceDmai::initCheck (void)
{
    int     videoFormat = OVERLAY_FORMAT_CbYCrY_422_I;
    int     displayWidth = iVideoDisplayWidth;
    int     displayHeight = iVideoDisplayHeight;
    int     frameWidth = iVideoWidth;
    int     frameHeight = iVideoHeight;
    int     frameSize;

    /***************************************************************************
     * Reset flags in case display format changes in the middle of a stream
     **************************************************************************/

    resetVideoParameterFlags();

    /***************************************************************************
     * Verify the video format is 422 interleaved
     **************************************************************************/

    if (iVideoSubFormat != PVMF_MIME_YUV422_INTERLEAVED_UYVY) 
    {
        return TRACE (INIT, _b("Video format is not supported: %s"), iVideoSubFormat.getMIMEStrPtr()), false;
    }

    /***************************************************************************
     * Create overlay if not presents
     **************************************************************************/

    if (mOverlay == NULL)
    {
        sp<OverlayRef>  ref = NULL;
        int             retry;
       
        TRACE (INIT, _b("Create DMAI overlay"));

        /***********************************************************************
         * Surfaceflinger may hold onto the previous overlay reference for some
         * time after we try to destroy it. retry a few times. In the future, we
         * should make the destroy call block, or possibly specify that we can
         * wait in the createOverlay call if the previous overlay is in the
         * process of being destroyed.
         **********************************************************************/

        for (retry = 0; retry < 50; retry++)
        {
            /* ...try to create overlay */
            if ((ref = mSurface->createOverlay(displayWidth, displayHeight, videoFormat)) != NULL)  break;
            
            TRACE (INIT, _b("Overlay create failed - retrying"));

            /* ...wait for 100 ms before next attempt */
            usleep (100000);
        }

        /* ...give up if overlay cannot be created */
        if (ref.get() == NULL)  return TRACE (ERROR, _b("Overlay creation failed")), mInitialized;

        /* ...make a 'smart' reference to created overlay */
        mOverlay = new Overlay(ref);

        /* ...set its parameters */
        mOverlay->setParameter (CACHEABLE_BUFFERS, 0);
    }
    else
    {
        /***********************************************************************
         * Overlay already exists; resize it to new dimensions
         **********************************************************************/

        mOverlay->resizeInput (displayWidth, displayHeight);
    }

    /***************************************************************************
     * All that buffer-allocation stuff should be removed
     **************************************************************************/

    /* ...gone away */

    /***************************************************************************
     * Mark the initialization is completed
     **************************************************************************/

    mInitialized = true;

    /***************************************************************************
     * Post event to the player about video MIO dimensions
     **************************************************************************/

    TRACE (INIT, _b("sendEvent(MEDIA_SET_VIDEO_SIZE, %d, %d)"), iVideoDisplayWidth, iVideoDisplayHeight);

    mPvPlayer->sendEvent (MEDIA_SET_VIDEO_SIZE, iVideoDisplayWidth, iVideoDisplayHeight);

    /***************************************************************************
     * Return final result code
     **************************************************************************/

    return mInitialized;
}

/*******************************************************************************
 * AndroidSurfaceDmai::writeFrameBuf
 *
 * Write a data into frame buffer
 ******************************************************************************/

PVMFStatus AndroidSurfaceDmai::writeFrameBuf (uint8* aData, uint32 aDataLen, const PvmiMediaXferHeader& data_header_info)
{
    int     result;
    
    /***************************************************************************
     * Parameters verification
     **************************************************************************/

    /* ...make sure the surface exists */
    if (mSurface == 0)      return TRACE (ERROR, _x("Surface is null")), PVMFFailure;

    /***************************************************************************
     * Output all that stuff into framebuffer
     **************************************************************************/

    TRACE (DATA, _b("Output frame %p [%u]"), aData, (unsigned)aDataLen);

    /* ...we do believe the buffer holds valid pointer to DMAI buffer (can be checked) */

    if ((result = mOverlay->queueBuffer (aData)) == /*ALL_BUFFERS_FLUSHED*/0)
    {
        /* ...hmm... to-be-understood */
    }
    
    /* ...we do not need to dequeue any buffer, I guess */

    /***************************************************************************
     * Return final success result code
     **************************************************************************/
        
    return PVMFSuccess;
}

/*******************************************************************************
 * AndroidSurfaceDmai::setParametersSync
 *
 * Set component parameters
 ******************************************************************************/

void AndroidSurfaceDmai::setParametersSync (PvmiMIOSession aSession, PvmiKvp* aParameters, int num_elements, PvmiKvp *&aRet_kvp)
{
    int     i;

    TRACE (INFO, _b("setParametersSync called"));
    
    /* ...pass control to base class component */
    AndroidSurfaceOutput::setParametersSync(aSession, aParameters, num_elements, aRet_kvp);
    
    return;

#if 0
    for (i = 0; i < num_elements; i++)
    {
        /***********************************************************************
         * Process parameter key
         **********************************************************************/

        if (pv_mime_strcmp (aParameters[i].key, MOUT_VIDEO_FORMAT_KEY) == 0)
        {
            /*******************************************************************
             * Video format
             ******************************************************************/

            iVideoFormat = (iVideoFormatString = aParameters[i].value.pChar_value).get_str();

            TRACE (INFO, _b("Video format: %s"), iVideoFormat);
        }
        else if (pv_mime_strcmp (aParameters[i].key, PVMF_FORMAT_SPECIFIC_INFO_KEY_YUV) == 0)
        {
            /*******************************************************************
             * YUV specification
             ******************************************************************/

            PVMFYuvFormatSpecificInfo0  *yuvInfo = (PVMFYuvFormatSpecificInfo0*) aParameters->value.key_specific_value;

            /* ...extract video parameters */
            iVideoWidth = (int32) yuvInfo->buffer_width, TRACE (INFO, _b("Width: %d"), (int)iVideoWidth);
            iVideoHeight = (int32)yuvInfo->buffer_height, TRACE (INFO, _b("Height: %d"), (int)iVideoHeight);
            iVideoDisplayWidth = (int32)yuvInfo->viewable_width, TRACE (INFO, _b("Display width: %d"), (int)iVideoDisplayHeight);
            iVideoDisplayHeight = (int32)yuvInfo->viewable_height, TRACE (INFO, _b("Display height: %d"), (int)iVideoDisplayHeight);
            iNumberOfBuffers = (int32)yuvInfo->num_buffers, TRACE (INFO, _b("Number of buffers: %d"), (int)iNumberOfBuffers);
            iBufferSize = (int32)yuvInfo->buffer_size, TRACE (INFO, _b("Buffer size: %d"), (int)iBufferSize);
            iVideoSubFormat = yuvInfo->video_format.getMIMEStrPtr(), TRACE (INFO, _b("Video format: %s"), iVideoSubFormat);

            /* ...put parameters flag */
            iVideoParameterFlags |= VIDEO_WIDTH_VALID | VIDEO_HEIGHT_VALID | DISPLAY_WIDTH_VALID | DISPLAY_HEIGHT_VALID | VIDEO_SUBFORMAT_VALID;
        }
        else
        {
            /*******************************************************************
             * Unrecognized key
             ******************************************************************/

            TRACE (INFO, _b("Unrecognized key: %s"), aParameters[i].key);

            /* ...pass the key as a return value */
            aRet_kvp = &aParameters[i];

            return;
        }
    }

    /***************************************************************************
     * Copy Code from base class. Ideally we'd just call base class's 
     * setParametersSync, but we can't as it will not get to initCheck if it 
     * encounters an unrecognized parameter such as the one we're handling here 
     **************************************************************************/

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
#endif
}

/*******************************************************************************
 * AndroidSurfaceDmai::getParametersSync
 *
 * Hmm... Okay...
 ******************************************************************************/

PVMFStatus AndroidSurfaceDmai::getParametersSync (PvmiMIOSession aSession, PvmiKeyType aIdentifier,  PvmiKvp *&aParameters, int &num_parameter_elements, PvmiCapabilityContext aContext)
{
    TRACE (INFO, _b("getParametersSync: %s"), aIdentifier);
    
#if 0
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

    /* ...pass control to base class method */
    return AndroidSurfaceOutput::getParametersSync (aSession, aIdentifier, aParameters, num_parameter_elements, aContext);
}

/*******************************************************************************
 * AndroidSurfaceDmai::postLastFrame
 *
 * Stub to override base class' implementation
 ******************************************************************************/

void AndroidSurfaceDmai::postLastFrame (void)
{
    TRACE (INFO, _b("Post last frame - ignored"));
}

/*******************************************************************************
 * AndroidSurfaceDmai::closeFrameBuf
 *
 * Close frame buffer
 ******************************************************************************/

void AndroidSurfaceDmai::closeFrameBuf (void)
{
    /* ...destroy overlay as appropriate */
    (mOverlay != NULL ? mOverlay->destroy(), mOverlay = NULL : 0);

    /* ...mark the component is not initialized */
    mInitialized = false;

    TRACE (INIT, _b("Frame buffer closed"));
}

/*******************************************************************************
 * Entry points
 ******************************************************************************/

/*******************************************************************************
 * createVideoMio
 *
 * Create MIO component (will never be closed or what?)
 ******************************************************************************/

extern "C" AndroidSurfaceDmai* createVideoMio (void)
{
    TRACE (INIT, _b("Create Android Dmai MIO component"));

    return new AndroidSurfaceDmai();
}
