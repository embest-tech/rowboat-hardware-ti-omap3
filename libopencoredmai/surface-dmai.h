/*******************************************************************************
 * surface-dmai.h
 *
 * Definition of android surface output used in conjunction with DMAI overlays
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

#ifndef __SURFACE_DMAI_H
#define __SURFACE_DMAI_H

/*******************************************************************************
 * Includes
 ******************************************************************************/
 
/* ...OpenCore video MIO component for Android */
#include "android_surface_output.h"

/* ...support for shared contiguous physical memory */
#include <ui/Overlay.h>

#include <cutils/properties.h>
#include "pv_mime_string_utils.h"
#include "pvmf_video.h"
#include <media/PVPlayer.h>

/*******************************************************************************
 * Video MIO component definition
 ******************************************************************************/

class AndroidSurfaceDmai : public AndroidSurfaceOutput
{
public:

    /***************************************************************************
     * Constructor/destructor
     **************************************************************************/

    AndroidSurfaceDmai();
    ~AndroidSurfaceDmai();

    /***************************************************************************
     * Frame buffer interface
     **************************************************************************/

    virtual bool initCheck();
    virtual void setParametersSync(PvmiMIOSession aSession, PvmiKvp* aParameters, int num_elements, PvmiKvp * & aRet_kvp);
    virtual PVMFStatus getParametersSync(PvmiMIOSession aSession, PvmiKeyType aIdentifier, PvmiKvp*& aParameters, int& num_parameter_elements, PvmiCapabilityContext aContext);
    virtual PVMFStatus writeFrameBuf(uint8* aData, uint32 aDataLen, const PvmiMediaXferHeader& data_header_info);
    virtual void closeFrameBuf();
    virtual void postLastFrame();

private:

    /***************************************************************************
     * Local data
     **************************************************************************/

    bool                    mUseOverlay;
    sp<Overlay>             mOverlay;
    int                     bufEnc;
    int32                   iNumberOfBuffers;
    int32                   iBufferSize;
    bool                    mIsFirstFrame;
    bool                    mConvert;

public:

    /***************************************************************************
     * Buffer allocator stuff... to-be-removed
     **************************************************************************/
    
    //BufferAllocOmap34xx     mbufferAlloc;
};

#endif  /* __SURFACE_DMAI_H */
