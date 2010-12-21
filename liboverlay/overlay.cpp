/*
 * Copyright (C) 2008 The Android Open Source Project
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
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "TIOverlay"

#include <hardware/hardware.h>
#include <hardware/overlay.h>

extern "C" {
#include "v4l2_utils.h"
}

#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev.h>

#include <cutils/log.h>
#include <cutils/ashmem.h>
#include <cutils/atomic.h>

/*****************************************************************************/

#define LOG_FUNCTION_NAME LOGV(" %s %s",  __FILE__, __FUNCTION__)

#define SHARED_DATA_MARKER             (0x68759746) // OVRLYSHM on phone keypad

#define CACHEABLE_BUFFERS 0x1

#define ALL_BUFFERS_FLUSHED -66 //shared with Camera/Video Playback HAL

typedef struct
{
  uint32_t posX;
  uint32_t posY;
  uint32_t posW;
  uint32_t posH;
  uint32_t rotation;
} overlay_ctrl_t;

typedef struct
{
  uint32_t cropX;
  uint32_t cropY;
  uint32_t cropW;
  uint32_t cropH;
} overlay_data_t;

typedef struct
{
  uint32_t marker;
  uint32_t size;

  volatile int32_t refCnt;

  uint32_t controlReady; // Only updated by the control side
  uint32_t dataReady;    // Only updated by the data side

  pthread_mutex_t lock;

  uint32_t streamEn;
  uint32_t streamingReset;
} overlay_shared_t;

// Only one instance is created per platform
struct overlay_control_context_t {
    struct overlay_control_device_t device;
    /* our private state goes below here */
    struct overlay_t* overlay_video1;
    struct overlay_t* overlay_video2;
};

// A separate instance is created per overlay data side user
struct overlay_data_context_t {
    struct overlay_data_device_t device;
    /* our private state goes below here */
    int ctl_fd;
    int shared_fd;
    int resizer_fd;
    int shared_size;
    int width;
    int height;
    int format;
    int num_buffers;
#ifdef OVERLAY_USERPTR_BUFFER
    size_t buffers_len;
#else
    size_t *buffers_len;
    void **buffers;
    mapping_data_t    *mapping_data;
#endif

    overlay_data_t    data;
    overlay_shared_t  *shared;
    // Need to count Qd buffers to be sure we don't block DQ'ing when exiting
    int qd_buf_count;
    int cacheable_buffers;
};

static int  create_shared_data(overlay_shared_t **shared);
static void destroy_shared_data(int shared_fd, overlay_shared_t *shared, bool closefd);
static int  open_shared_data(overlay_data_context_t *ctx);
static void close_shared_data(overlay_data_context_t *ctx);
enum { LOCK_REQUIRED = 1, NO_LOCK_NEEDED = 0 };
static int  enable_streaming( overlay_shared_t *shared, int ovly_fd, int lock_required );

static int overlay_device_open(const struct hw_module_t* module,
                               const char* name, struct hw_device_t** device);

static struct hw_module_methods_t overlay_module_methods = {
    open: overlay_device_open
};

struct overlay_module_t HAL_MODULE_INFO_SYM = {
    common: {
        tag: HARDWARE_MODULE_TAG,
        version_major: 1,
        version_minor: 0,
        id: OVERLAY_HARDWARE_MODULE_ID,
        name: "Sample Overlay module",
        author: "The Android Open Source Project",
        methods: &overlay_module_methods,
    }
};

/*****************************************************************************/

/*
 * This is the overlay_t object, it is returned to the user and represents
 * an overlay. here we use a subclass, where we can store our own state.
 * This handles will be passed across processes and possibly given to other
 * HAL modules (for instance video decode modules).
 */
struct handle_t : public native_handle {
    /* add the data fields we need here, for instance: */
    int ctl_fd;
    int shared_fd;
    int resizer_fd;
    int width;
    int height;
    int format;
    int num_buffers;
    int shared_size;
};

static int handle_format(const overlay_handle_t overlay) {
    return static_cast<const struct handle_t *>(overlay)->format;
}

static int handle_ctl_fd(const overlay_handle_t overlay) {
    return static_cast<const struct handle_t *>(overlay)->ctl_fd;
}

static int handle_shared_fd(const overlay_handle_t overlay) {
    return static_cast<const struct handle_t *>(overlay)->shared_fd;
}

static int handle_resizer_fd(const overlay_handle_t overlay) {
    return static_cast<const struct handle_t *>(overlay)->resizer_fd;
}

static int handle_num_buffers(const overlay_handle_t overlay) {
    return static_cast<const struct handle_t *>(overlay)->num_buffers;
}

static int handle_width(const overlay_handle_t overlay) {
    return static_cast<const struct handle_t *>(overlay)->width;
}

static int handle_height(const overlay_handle_t overlay) {
    return static_cast<const struct handle_t *>(overlay)->height;
}

static int handle_shared_size(const overlay_handle_t overlay) {
    return static_cast<const struct handle_t *>(overlay)->shared_size;
}

// A separate instance of this class is created per overlay
class overlay_object : public overlay_t
{
    handle_t mHandle;

    overlay_ctrl_t    mCtl;
    overlay_ctrl_t    mCtlStage;
    overlay_shared_t *mShared;

    static overlay_handle_t getHandleRef(struct overlay_t* overlay) {
        /* returns a reference to the handle, caller doesn't take ownership */
        return &(static_cast<overlay_object *>(overlay)->mHandle);
    }

public:
    overlay_object(int ctl_fd, int shared_fd, int resizer_fd, int shared_size, int w, int h,
                   int format, int num_buffers) {
        this->overlay_t::getHandleRef = getHandleRef;
        mHandle.version     = sizeof(native_handle);
        mHandle.numFds      = 3;
        mHandle.numInts     = 5; // extra ints we have in our handle
        mHandle.ctl_fd      = ctl_fd;
        mHandle.shared_fd   = shared_fd;
        mHandle.resizer_fd  = resizer_fd;
        mHandle.width       = w;
        mHandle.height      = h;
        mHandle.format      = format;
        mHandle.num_buffers = num_buffers;
        mHandle.shared_size = shared_size;
        this->w = w;
        this->h = h;
        this->format = format;

        memset( &mCtl, 0, sizeof( mCtl ) );
        memset( &mCtlStage, 0, sizeof( mCtlStage ) );
    }

    int               ctl_fd()    { return mHandle.ctl_fd; }
    int               shared_fd() { return mHandle.shared_fd; }
    int               resizer_fd() { return mHandle.resizer_fd; }
    overlay_ctrl_t*   data()      { return &mCtl; }
    overlay_ctrl_t*   staging()   { return &mCtlStage; }
    overlay_shared_t* getShared() { return mShared; }
    void              setShared( overlay_shared_t *p ) { mShared = p; }
};

// ****************************************************************************
// Local Functions
// ****************************************************************************

static int create_shared_data(overlay_shared_t **shared)
{
    int fd;
    // assuming sizeof(overlay_shared_t) < a single page
    int size = getpagesize();
    overlay_shared_t *p;

    if ((fd = ashmem_create_region("overlay_data", size)) < 0) {
        LOGE("Failed to Create Overlay Shared Data!\n");
        return fd;
    }

    p = (overlay_shared_t*)mmap(NULL, size, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        LOGE("Failed to Map Overlay Shared Data!\n");
        close(fd);
        return -1;
    }

    memset(p, 0, size);
    p->marker = SHARED_DATA_MARKER;
    p->size   = size;
    p->refCnt = 1;

    if (pthread_mutex_init(&p->lock, NULL) != 0) {
        LOGE("Failed to Open Overlay Lock!\n");
        munmap(p, size);
        close(fd);
        return -1;
    }

    *shared = p;
    return fd;
}

static void destroy_shared_data( int shared_fd, overlay_shared_t *shared, bool closefd )
{
    if (shared == NULL)
        return;

    // Last side deallocated releases the mutex, otherwise the remaining
    // side will deadlock trying to use an already released mutex
    if (android_atomic_dec(&shared->refCnt) == 1) {
        if (pthread_mutex_destroy(&shared->lock)) {
            LOGE("Failed to Close Overlay Semaphore!\n");
        }

        shared->marker = 0;
    }

    if (munmap(shared, shared->size)) {
        LOGE("Failed to Unmap Overlay Shared Data!\n");
    }

    if (closefd && close(shared_fd)) {
        LOGE("Failed to Close Overlay Shared Data!\n");
    }
}

static int open_shared_data( overlay_data_context_t *ctx )
{
    int rc   = -1;
    int mode = PROT_READ | PROT_WRITE;
    int fd   = ctx->shared_fd;
    int size = ctx->shared_size;

    if (ctx->shared != NULL) {
        // Already open, return success
        LOGI("Overlay Shared Data Already Open\n");
        return 0;
    }
    ctx->shared = (overlay_shared_t*)mmap(0, size, mode, MAP_SHARED, fd, 0);

    if (ctx->shared == MAP_FAILED) {
        LOGE("Failed to Map Overlay Shared Data!\n");
    } else if ( ctx->shared->marker != SHARED_DATA_MARKER ) {
        LOGE("Invalid Overlay Shared Marker!\n");
        munmap( ctx->shared, size);
    } else if ( (int)ctx->shared->size != size ) {
        LOGE("Invalid Overlay Shared Size!\n");
        munmap(ctx->shared, size);
    } else {
        android_atomic_inc(&ctx->shared->refCnt);
        rc = 0;
    }

    return rc;
}

static void close_shared_data(overlay_data_context_t *ctx)
{
    destroy_shared_data(ctx->shared_fd, ctx->shared, false);
    ctx->shared = NULL;
}

static int enable_streaming_locked(overlay_shared_t *shared, int ovly_fd)
{
    int rc = 0;

    if (!shared->controlReady || !shared->dataReady) {
        LOGI("Postponing Stream Enable/%d/%d\n", shared->controlReady,
             shared->dataReady);
    } else {
        shared->streamEn = 1;
        rc = v4l2_overlay_stream_on(ovly_fd);
        if (rc) {
            LOGE("Stream Enable Failed!/%d\n", rc);
            shared->streamEn = 0;
        }
    }

    return rc;
}

static int enable_streaming(overlay_shared_t *shared, int ovly_fd)
{
    int ret;

    pthread_mutex_lock(&shared->lock);
    ret = enable_streaming_locked(shared, ovly_fd);
    pthread_mutex_unlock(&shared->lock);
    return ret;
}

static int disable_streaming_locked(overlay_shared_t *shared, int ovly_fd)
{
    int ret = 0;

    if (shared->streamEn) {
        ret = v4l2_overlay_stream_off( ovly_fd );
        if (ret) {
            LOGE("Stream Off Failed!/%d\n", ret);
        } else {
            shared->streamingReset = 1;
            shared->streamEn = 0;
            /* indicate that buffers are flushed during stream off */
            shared->dataReady = 0;
        }
    }

    return ret;
}

// ****************************************************************************
// Control module
// ****************************************************************************

static int overlay_get(struct overlay_control_device_t *dev, int name)
{
    int result = -1;

    switch (name) {
        case OVERLAY_MINIFICATION_LIMIT:   result = 0;  break; // 0 = no limit
        case OVERLAY_MAGNIFICATION_LIMIT:  result = 0;  break; // 0 = no limit
        case OVERLAY_SCALING_FRAC_BITS:    result = 0;  break; // 0 = infinite
        case OVERLAY_ROTATION_STEP_DEG:    result = 90; break; // 90 rotation steps (for instance)
        case OVERLAY_HORIZONTAL_ALIGNMENT: result = 1;  break; // 1-pixel alignment
        case OVERLAY_VERTICAL_ALIGNMENT:   result = 1;  break; // 1-pixel alignment
        case OVERLAY_WIDTH_ALIGNMENT:      result = 1;  break; // 1-pixel alignment
        case OVERLAY_HEIGHT_ALIGNMENT:     break;
    }

    return result;
}

static overlay_t* overlay_createOverlay(struct overlay_control_device_t *dev,
                                        uint32_t w, uint32_t h, int32_t  format)
{
    LOGD("overlay_createOverlay:IN w=%d h=%d format=%d\n", w, h, format);
    LOG_FUNCTION_NAME;

    overlay_object            *overlay;
    overlay_control_context_t *ctx = (overlay_control_context_t *)dev;
    overlay_shared_t          *shared;

    int ret;
    uint32_t num = NUM_OVERLAY_BUFFERS_REQUESTED;
    int fd;
    int shared_fd;
    int resizer_fd;

    if (format == OVERLAY_FORMAT_DEFAULT)
    {
        format = OVERLAY_FORMAT_CbYCrY_422_I;
    }

    if (ctx->overlay_video1) {
        LOGE("Error - overlays already in use\n");
        return NULL;
    }

    shared_fd = create_shared_data(&shared);
    if (shared_fd < 0) {
        LOGE("Failed to create shared data");
        return NULL;
    }

    fd = v4l2_overlay_open(V4L2_OVERLAY_PLANE_VIDEO1);
    if (fd < 0) {
        LOGE("Failed to open overlay device\n");
        goto error;
    }

#ifdef HAS_ISP
    resizer_fd = v4l2_resizer_open();
    if (resizer_fd < 0) {
        LOGE("Failed to open resizer device ret=%d\n",resizer_fd);
        goto error;
    }

    if (v4l2_resizer_config(resizer_fd, w, h)) {
        LOGE("Failed to configure resizer device\n");
        goto error;
    }
#endif

    if (v4l2_overlay_init(fd, w, h, format)) {
        LOGE("Failed initializing overlays\n");
        goto error1;
    }

    /* Rotation MUST come prior all overlay operations.
     * Set rotation to 90 now. Then we'll be able to
     * change the angle runtime, otherwise DSS crashes.
     */
    if (v4l2_overlay_set_rotation(fd, 90, 0)) {
        LOGE("Failed defaulting rotation\n");
        goto error1;
    }

    if (v4l2_overlay_set_crop(fd, 0, 0, w, h)) {
        LOGE("Failed defaulting crop window\n");
        goto error1;
    }

    if (v4l2_overlay_req_buf(fd, &num, 0)) {
        LOGE("Failed requesting buffers\n");
        goto error1;
    }

   overlay = new overlay_object(fd, shared_fd, resizer_fd, shared->size, w, h, format, num);
   if (overlay == NULL) {
        LOGE("Failed to create overlay object\n");
        goto error1;
   }
   ctx->overlay_video1 = overlay;

   overlay->data()->rotation = 90;

   overlay->setShared(shared);

   shared->controlReady = 0;
   shared->streamEn = 0;
   shared->streamingReset = 0;
    LOGI("Opened video1/fd=%d/obj=%08lx/shm=%d/size=%d", fd,
        (unsigned long)overlay, shared_fd, shared->size);


    LOGD("overlay_createOverlay: OUT");
    return overlay;

error1:
    close(fd);
error:
    destroy_shared_data(shared_fd, shared, true);
    return NULL;
}

static void overlay_destroyOverlay(struct overlay_control_device_t *dev,
                                   overlay_t* overlay)
{
    LOGD("overlay_destroyOverlay:IN dev (%p) and overlay (%p)", dev, overlay);
    LOG_FUNCTION_NAME;

    overlay_control_context_t *ctx = (overlay_control_context_t *)dev;
    overlay_object *obj = static_cast<overlay_object *>(overlay);

    int rc;
    int fd = obj->ctl_fd();
    int resizer_fd = obj->resizer_fd();
    overlay_shared_t *shared = obj->getShared();

    if (shared == NULL) {
        LOGE("Overlay was already destroyed - nothing needs to be done\n");
        return;
    }

    pthread_mutex_lock(&shared->lock);

    disable_streaming_locked(shared, fd);

    pthread_mutex_unlock(&shared->lock);

    destroy_shared_data(obj->shared_fd(), shared, true);
    obj->setShared(NULL);

    LOGI("Destroying overlay/fd=%d/obj=%08lx", fd, (unsigned long)overlay);

    if (close(fd)) {
        LOGE( "Error closing overly fd/%d\n", errno);
    }

#ifdef HAS_ISP
    if (close(resizer_fd)) {
        LOGE( "Error closing overly resizer fd/%d\n", errno);
    }
#endif

    if (overlay) {
        if (ctx->overlay_video1 == overlay)
            ctx->overlay_video1 = NULL;
        delete overlay;
        overlay = NULL;
    }
    LOGD("overlay_destroyOverlay:OUT");
}

static int overlay_setPosition(struct overlay_control_device_t *dev,
                               overlay_t* overlay, int x, int y, uint32_t w,
                               uint32_t h)
{
    overlay_object *obj = static_cast<overlay_object *>(overlay);
    overlay_ctrl_t   *stage  = obj->staging();

    LOG_FUNCTION_NAME;

    stage->posX = x;
    stage->posY = y;
    stage->posW = w;
    stage->posH = h;

    return 0;
}

static int overlay_getPosition(struct overlay_control_device_t *dev,
                               overlay_t* overlay, int* x, int* y, uint32_t* w,
                               uint32_t* h)
{
    LOG_FUNCTION_NAME;

    int fd = static_cast<overlay_object *>(overlay)->ctl_fd();

    if (v4l2_overlay_get_position(fd, x, y, (int32_t*)w, (int32_t*)h)) {
        return -EINVAL;
    }
    return 0;
}

static int overlay_setParameter(struct overlay_control_device_t *dev,
                                overlay_t* overlay, int param, int value)
{
    LOG_FUNCTION_NAME;

    overlay_ctrl_t *stage = static_cast<overlay_object *>(overlay)->staging();
    int rc = 0;

    switch (param) {
    case OVERLAY_DITHER:
        break;

    case OVERLAY_TRANSFORM:
        switch ( value )
        {
        case 0:
            stage->rotation = 0;
            break;
        case OVERLAY_TRANSFORM_ROT_90:
            stage->rotation = 90;
            break;
        case OVERLAY_TRANSFORM_ROT_180:
            stage->rotation = 180;
            break;
        case OVERLAY_TRANSFORM_ROT_270:
            stage->rotation = 270;
            break;
        default:
            rc = -EINVAL;
            break;
        }
        break;
    }

    return rc;
}

static int overlay_stage(struct overlay_control_device_t *dev,
                          overlay_t* overlay) {
    return 0;
}

static int overlay_commit(struct overlay_control_device_t *dev,
                          overlay_t* overlay) {
    LOG_FUNCTION_NAME;

    overlay_object *obj = static_cast<overlay_object *>(overlay);

    overlay_ctrl_t   *data   = obj->data();
    overlay_ctrl_t   *stage  = obj->staging();
    overlay_shared_t *shared = obj->getShared();
    overlay_ctrl_t    tmp_stage;

    int ret = 0;
    int fd = obj->ctl_fd();

    if (shared == NULL) {
        LOGI("Shared Data Not Init'd!\n");
        return -1;
    }

    pthread_mutex_lock(&shared->lock);

    if (!shared->controlReady) {
        shared->controlReady = 1;
    }

    /* convert stage window into v4l2 window format */
    if (stage->rotation == 90 || stage->rotation == 270) {
        memcpy(&tmp_stage, stage, sizeof(tmp_stage));
        stage->posX = tmp_stage.posY;
        stage->posY = tmp_stage.posX;
        stage->posW = tmp_stage.posH;
        stage->posH = tmp_stage.posW;
    }

    if (data->posX == stage->posX && data->posY == stage->posY &&
        data->posW == stage->posW && data->posH == stage->posH &&
        data->rotation == stage->rotation) {
        LOGI("Nothing to do!\n");
        goto end;
    }

    LOGI("Position/X%d/Y%d/W%d/H%d\n", data->posX, data->posY, data->posW,
         data->posH);
    LOGI("Adjusted Position/X%d/Y%d/W%d/H%d\n", stage->posX, stage->posY,
         stage->posW, stage->posH);
    LOGI("Rotation/%d\n", stage->rotation );

    if ((ret = disable_streaming_locked(shared, fd)))
        goto end;

    if (stage->rotation != data->rotation) {
        ret = v4l2_overlay_set_rotation(fd, stage->rotation, 0);
        if (ret) {
            LOGE("Set Rotation Failed!/%d\n", ret);
            goto end;
        }
        data->rotation = stage->rotation;

        ret = v4l2_overlay_reinit(fd);
        if (ret) {
            LOGE("Failed reinitializing overlays %d\n", ret);
            goto end;
        }
    }

    if (!(stage->posX == data->posX && stage->posY == data->posY &&
        stage->posW == data->posW && stage->posH == data->posH)) {
        ret = v4l2_overlay_set_position(fd, stage->posX, stage->posY,
                                        stage->posW, stage->posH);
        if (ret) {
            LOGE("Set Position Failed!/%d\n", ret);
            goto end;
        }
        data->posX = stage->posX;
        data->posY = stage->posY;
        data->posW = stage->posW;
        data->posH = stage->posH;
    }

    ret = enable_streaming_locked(shared, fd);

end:
    pthread_mutex_unlock(&shared->lock);

    return ret;
}

static int overlay_control_close(struct hw_device_t *dev)
{
    LOG_FUNCTION_NAME;

    struct overlay_control_context_t* ctx = (struct overlay_control_context_t*)dev;
    overlay_object *overlay_v1;
    //overlay_object *overlay_v2;

    if (ctx) {
        overlay_v1 = static_cast<overlay_object *>(ctx->overlay_video1);
        //overlay_v2 = static_cast<overlay_object *>(ctx->overlay_video2);

        overlay_destroyOverlay((struct overlay_control_device_t *)ctx,
                               overlay_v1);
        //overlay_destroyOverlay((struct overlay_control_device_t *)ctx, overlay_v2);

        free(ctx);
    }
    return 0;
}

// ****************************************************************************
// Data module
// ****************************************************************************

int overlay_initialize(struct overlay_data_device_t *dev,
                       overlay_handle_t handle)
{
    LOG_FUNCTION_NAME;

    struct overlay_data_context_t* ctx = (struct overlay_data_context_t*)dev;
    struct stat stat;

    int i;
    int rc = -1;

    ctx->num_buffers  = handle_num_buffers(handle);
    ctx->width        = handle_width(handle);
    ctx->height       = handle_height(handle);
    ctx->format       = handle_format(handle);
    ctx->ctl_fd       = handle_ctl_fd(handle);
    ctx->shared_fd    = handle_shared_fd(handle);
    ctx->resizer_fd   = handle_resizer_fd(handle);
    ctx->shared_size  = handle_shared_size(handle);
    ctx->shared       = NULL;
    ctx->qd_buf_count = 0;
    ctx->cacheable_buffers = 0;

    if (fstat(ctx->ctl_fd, &stat)) {
        LOGE("Error = %s from %s\n", strerror(errno), "overlay initialize");
        return -1;
    }

    if (open_shared_data(ctx)) {
        return -1;
    }

    ctx->shared->dataReady = 0;

#ifdef OVERLAY_USERPTR_BUFFER
    /* FIXME following is not ideal to get overlay buffer size,
     * which is needed to queue a bufer */
    if (ctx->format == OVERLAY_FORMAT_CbYCrY_422_I) {
        ctx->buffers_len = ctx->width * ctx->height * 2;
        rc = 0; /* NO_ERROR */
    } else {
        LOGE("overlay_initialize() - unsupported format");
        return -1;
    }
#else
    ctx->mapping_data = new mapping_data_t;
    ctx->buffers     = new void* [ctx->num_buffers];
    ctx->buffers_len = new size_t[ctx->num_buffers];
    if (!ctx->buffers || !ctx->buffers_len || !ctx->mapping_data) {
            LOGE("Failed alloc'ing buffer arrays\n");
            close_shared_data(ctx);
    } else {
        for (i = 0; i < ctx->num_buffers; i++) {
            rc = v4l2_overlay_map_buf(ctx->ctl_fd, i, &ctx->buffers[i],
                                       &ctx->buffers_len[i]);
            if (rc) {
                LOGE("Failed mapping buffers\n");
                close_shared_data( ctx );
                break;
            }
        }
    }
#endif

    return ( rc );
}

static int overlay_frameCopy(struct overlay_data_device_t *dev, overlay_buffer_t inbuf,
                             overlay_buffer_t outbuf)
{
    int rc = 0;

#ifndef OVERLAY_USERPTR_BUFFER
    struct overlay_data_context_t* ctx = (struct overlay_data_context_t*)dev;

    rc = v4l2_resizer_execute(ctx->resizer_fd, (void*) inbuf, (void*)outbuf);
    if (rc) {
        LOGE("Error resizer framecopy");
        return rc;
    }
#endif

    return rc;
}

static int overlay_resizeInput(struct overlay_data_device_t *dev, uint32_t w,
                               uint32_t h)
{
    int rc = -1;

    struct overlay_data_context_t* ctx = (struct overlay_data_context_t*)dev;

    if ((ctx->width == (int)w) && (ctx->width == (int)h)) {
        LOGV("same as current width and height. so do nothing");
        return 0;
    }

    if (!ctx->shared) {
        LOGI("Shared Data Not Init'd!\n");
        return -1;
    }

    if (ctx->shared->dataReady) {
        LOGV("Either setCrop() or queueBuffer() was called prior to this!"
             "Therefore failing this call.\n");
        return -1;
    }

    pthread_mutex_lock(&ctx->shared->lock);

    if ((rc = disable_streaming_locked(ctx->shared, ctx->ctl_fd)))
        goto end;

#ifndef OVERLAY_USERPTR_BUFFER
    for (int i = 0; i < ctx->num_buffers; i++) {
        v4l2_overlay_unmap_buf(ctx->buffers[i], ctx->buffers_len[i]);      
    }
#endif

    rc = v4l2_overlay_init(ctx->ctl_fd, w, h, ctx->format);
    if (rc) {
        LOGE("Error initializing overlay");
        goto end;
    }
    rc = v4l2_overlay_set_crop(ctx->ctl_fd, 0, 0, w, h);
    if (rc) {
        LOGE("Error setting crop window\n");
        goto end;
    }
    rc = v4l2_overlay_req_buf(ctx->ctl_fd, (uint32_t *)(&ctx->num_buffers),
                              ctx->cacheable_buffers);
    if (rc) {
        LOGE("Error creating buffers");
        goto end;
    }

#ifndef OVERLAY_USERPTR_BUFFER
    for (int i = 0; i < ctx->num_buffers; i++)
        v4l2_overlay_map_buf(ctx->ctl_fd, i, &ctx->buffers[i],
                             &ctx->buffers_len[i]);
#endif

    rc = enable_streaming_locked(ctx->shared, ctx->ctl_fd);

end:
    pthread_mutex_unlock(&ctx->shared->lock);

    return rc;
}


static int overlay_data_setParameter(struct overlay_data_device_t *dev,
                                     int param, int value)
{
    int ret = 0;
    struct overlay_data_context_t* ctx = (struct overlay_data_context_t*)dev;

    if (ctx->shared == NULL)
    {
        LOGI("Shared Data Not Init'd!\n");
        return -1;
    }

    if (ctx->shared->dataReady) {
        LOGI("Too late. Cant set it now!\n");
        return -1;
    }

    if (param == CACHEABLE_BUFFERS)
        ctx->cacheable_buffers = value;

    //ret = v4l2_overlay_set_attributes(ctx->ctl_fd, param, value);
    return ( ret );
}


static int overlay_setCrop(struct overlay_data_device_t *dev, uint32_t x,
                           uint32_t y, uint32_t w, uint32_t h) {
    int rc = 0;
    struct overlay_data_context_t* ctx = (struct overlay_data_context_t*)dev;

    LOG_FUNCTION_NAME;

    if (ctx->shared == NULL) {
        LOGI("Shared Data Not Init'd!\n");
        return -1;
    }

    pthread_mutex_lock(&ctx->shared->lock);

    ctx->shared->dataReady = 1;

    if (ctx->data.cropX == x && ctx->data.cropY == y && ctx->data.cropW == w
        && ctx->data.cropH == h) {
        LOGI("Nothing to do!\n");
        goto end;
    }

    ctx->data.cropX = x;
    ctx->data.cropY = y;
    ctx->data.cropW = w;
    ctx->data.cropH = h;

    LOGI("Crop Win/X%d/Y%d/W%d/H%d\n", x, y, w, h );

    if ((rc = disable_streaming_locked(ctx->shared, ctx->ctl_fd)))
        goto end;

    rc = v4l2_overlay_set_crop(ctx->ctl_fd, x, y, w, h);
    if (rc) {
        LOGE("Set Crop Window Failed!/%d\n", rc);
    }

    rc = enable_streaming_locked(ctx->shared, ctx->ctl_fd);

end:
    pthread_mutex_unlock(&ctx->shared->lock);
    return rc;
}

static int overlay_getCrop(struct overlay_data_device_t *dev , uint32_t* x,
                           uint32_t* y, uint32_t* w, uint32_t* h) {
    struct overlay_data_context_t* ctx = (struct overlay_data_context_t*)dev;

    LOG_FUNCTION_NAME;

    return v4l2_overlay_get_crop(ctx->ctl_fd, x, y, w, h);
}

int overlay_dequeueBuffer(struct overlay_data_device_t *dev,
                          overlay_buffer_t *buffer) {
    /* blocks until a buffer is available and return an opaque structure
     * representing this buffer.
     */

    struct overlay_data_context_t* ctx = (struct overlay_data_context_t*)dev;

    int rc;
#ifdef OVERLAY_USERPTR_BUFFER
    void *i = NULL;
#else
    int i = -1;
#endif

    pthread_mutex_lock(&ctx->shared->lock);
    if ( ctx->shared->streamingReset )
    {
        ctx->shared->streamingReset = 0;
        pthread_mutex_unlock(&ctx->shared->lock);
        return ALL_BUFFERS_FLUSHED;
    }
    pthread_mutex_unlock(&ctx->shared->lock);

    // If we are not streaming dequeue will fail, skip to prevent error printouts
    if (ctx->shared->streamEn) {
#ifdef OVERLAY_USERPTR_BUFFER
         if ( ctx->qd_buf_count < NUM_QUEUED_BUFFERS_OPTIMAL ) {
             LOGE("Queue more buffers before attempting to dequeue");
             return -1;
        }
#endif
        if ((rc = v4l2_overlay_dq_buf( ctx->ctl_fd, &i )) != 0) {
            LOGE("Failed to DQ/%d\n", rc);
        }
#ifdef OVERLAY_USERPTR_BUFFER
        *buffer = i;
        ctx->qd_buf_count --;
#else
        else if (i < 0 || i > ctx->num_buffers) {
            rc = -EINVAL;
        } else {
            *((int *)buffer) = i;
            ctx->qd_buf_count --;
        }
#endif
    } else {
        rc = -1;
    }

    return rc;
}

int overlay_queueBuffer(struct overlay_data_device_t *dev,
                        overlay_buffer_t buffer) {

    struct overlay_data_context_t* ctx = (struct overlay_data_context_t*)dev;

    pthread_mutex_lock(&ctx->shared->lock);
    if ( ctx->shared->streamingReset )
    {
        ctx->shared->streamingReset = 0;
        pthread_mutex_unlock(&ctx->shared->lock);
        return ALL_BUFFERS_FLUSHED;
    }
    pthread_mutex_unlock(&ctx->shared->lock);

#ifdef OVERLAY_USERPTR_BUFFER
    int rc = v4l2_overlay_q_buf( ctx->ctl_fd, buffer, ctx->buffers_len);
#else
    int rc = v4l2_overlay_q_buf( ctx->ctl_fd, (int)buffer );
#endif
    if (rc < 0) {
        LOGE("queueBuffer failed [%d]", rc);
    }
    else {
        if (ctx->qd_buf_count < ctx->num_buffers) {
            ctx->qd_buf_count ++;
        }
#ifdef OVERLAY_USERPTR_BUFFER
        rc = ctx->qd_buf_count;
#endif

        // Catch the case where the data side had no need to set the crop window
        if (!ctx->shared->dataReady) {
            ctx->shared->dataReady = 1;
            enable_streaming(ctx->shared, ctx->ctl_fd);
        }
    }

    return rc;
}

void *overlay_getBufferAddress(struct overlay_data_device_t *dev,
                               overlay_buffer_t buffer)
{
    LOG_FUNCTION_NAME;

    /* this may fail (NULL) if this feature is not supported. In that case,
     * presumably, there is some other HAL module that can fill the buffer,
     * using a DSP for instance
     */
#ifdef OVERLAY_USERPTR_BUFFER
    return NULL;
#else
    int ret;
    struct v4l2_buffer buf;
    struct overlay_data_context_t* ctx = (struct overlay_data_context_t*)dev;

    ret = v4l2_overlay_query_buffer(ctx->ctl_fd, (int)buffer, &buf);

    if (ret)
        return NULL;

    // Initialize ctx->mapping_data
    memset(ctx->mapping_data, 0, sizeof(mapping_data_t));

    ctx->mapping_data->fd = ctx->ctl_fd;
    ctx->mapping_data->length = buf.length;
    ctx->mapping_data->offset = buf.m.offset;
    ctx->mapping_data->ptr = NULL;

    if ((int)buffer >= 0 && (int)buffer < ctx->num_buffers) {
        ctx->mapping_data->ptr = ctx->buffers[(int)buffer];
        LOGI("Buffer/%d/addr=%08lx/len=%d", (int)buffer, (unsigned long)ctx->mapping_data->ptr,
             ctx->buffers_len[(int)buffer]);
    }

    return (void *)ctx->mapping_data;
#endif
}

int overlay_getBufferCount(struct overlay_data_device_t *dev)
{
    LOG_FUNCTION_NAME;

    struct overlay_data_context_t* ctx = (struct overlay_data_context_t*)dev;

    return (ctx->num_buffers);
}

static int overlay_data_close(struct hw_device_t *dev) {

    LOG_FUNCTION_NAME;

    struct overlay_data_context_t* ctx = (struct overlay_data_context_t*)dev;
    int rc;

    if (ctx) {
#ifndef OVERLAY_USERPTR_BUFFER
        overlay_data_device_t *overlay_dev = &ctx->device;
        int buf;
        int i;

        pthread_mutex_lock(&ctx->shared->lock);

        for (i = 0; i < ctx->num_buffers; i++) {
            LOGV("Unmap Buffer/%d/%08lx/%d", i, (unsigned long)ctx->buffers[i], ctx->buffers_len[i] );
            rc = v4l2_overlay_unmap_buf(ctx->buffers[i], ctx->buffers_len[i]);
            if (rc != 0) {
                LOGE("Error unmapping the buffer/%d/%d", i, rc);
            }
        }

        delete(ctx->mapping_data);
        delete(ctx->buffers);
        delete(ctx->buffers_len);

        pthread_mutex_unlock(&ctx->shared->lock);
#endif

        ctx->shared->dataReady = 0;
        close_shared_data( ctx );

        free(ctx);
    }

    return 0;
}

/*****************************************************************************/

static int overlay_device_open(const struct hw_module_t* module,
                               const char* name, struct hw_device_t** device)
{
    LOG_FUNCTION_NAME;
    int status = -EINVAL;

    if (!strcmp(name, OVERLAY_HARDWARE_CONTROL)) {
        struct overlay_control_context_t *dev;
        dev = (overlay_control_context_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = overlay_control_close;

        dev->device.get = overlay_get;
        dev->device.createOverlay = overlay_createOverlay;
        dev->device.destroyOverlay = overlay_destroyOverlay;
        dev->device.setPosition = overlay_setPosition;
        dev->device.getPosition = overlay_getPosition;
        dev->device.setParameter = overlay_setParameter;
        dev->device.stage = overlay_stage;
        dev->device.commit = overlay_commit;

        *device = &dev->device.common;
        status = 0;
    } else if (!strcmp(name, OVERLAY_HARDWARE_DATA)) {
        struct overlay_data_context_t *dev;
        dev = (overlay_data_context_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = overlay_data_close;

        dev->device.initialize = overlay_initialize;
        dev->device.resizeInput = overlay_resizeInput;
        dev->device.frameCopy = overlay_frameCopy;
        dev->device.setCrop = overlay_setCrop;
        dev->device.getCrop = overlay_getCrop;
        dev->device.setParameter = overlay_data_setParameter;
        dev->device.dequeueBuffer = overlay_dequeueBuffer;
        dev->device.queueBuffer = overlay_queueBuffer;
        dev->device.getBufferAddress = overlay_getBufferAddress;
        dev->device.getBufferCount = overlay_getBufferCount;

        *device = &dev->device.common;
        status = 0;
    }
    return status;
}
