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

//#define OVERLAY_DEBUG 1
#define LOG_TAG "Overlay"

#include <fcntl.h>
#include <errno.h>
#include <cutils/log.h>
#include <hardware/overlay.h>
#include <linux/videodev.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "v4l2_utils.h"
#include <linux/omap_resizer.h>

#define LOG_FUNCTION_NAME    LOGV("%s: %s",  __FILE__, __FUNCTION__);

#ifndef LOGE
#define LOGE(fmt,args...) \
        do { printf(fmt, ##args); } \
        while (0)
#endif

#ifndef LOGI
#define LOGI(fmt,args...) \
        do { LOGE(fmt, ##args); } \
        while (0)
#endif
#define V4L2_CID_PRIV_OFFSET			0x00530000
#define V4L2_CID_PRIV_ROTATION		(V4L2_CID_PRIVATE_BASE \
						+ V4L2_CID_PRIV_OFFSET + 0)
#define V4L2_CID_PRIV_COLORKEY		(V4L2_CID_PRIVATE_BASE \
						+ V4L2_CID_PRIV_OFFSET + 1)
#define V4L2_CID_PRIV_COLORKEY_EN	(V4L2_CID_PRIVATE_BASE \
						+ V4L2_CID_PRIV_OFFSET + 2)



int v4l2_overlay_get(int name) {
    int result = -1;
    switch (name) {
        case OVERLAY_MINIFICATION_LIMIT:
            result = 4; // 0 = no limit
            break;
        case OVERLAY_MAGNIFICATION_LIMIT:
            result = 2; // 0 = no limit
            break;
        case OVERLAY_SCALING_FRAC_BITS:
            result = 0; // 0 = infinite
            break;
        case OVERLAY_ROTATION_STEP_DEG:
            result = 90; // 90 rotation steps (for instance)
            break;
        case OVERLAY_HORIZONTAL_ALIGNMENT:
            result = 1; // 1-pixel alignment
            break;
        case OVERLAY_VERTICAL_ALIGNMENT:
            result = 1; // 1-pixel alignment
            break;
        case OVERLAY_WIDTH_ALIGNMENT:
            result = 1; // 1-pixel alignment
            break;
        case OVERLAY_HEIGHT_ALIGNMENT:
            result = 1; // 1-pixel alignment
            break;
    }
    return result;
}

int v4l2_overlay_open(int id)
{
    LOG_FUNCTION_NAME

    if (id == V4L2_OVERLAY_PLANE_VIDEO1)
        return open("/dev/video1", O_RDWR);
    else if (id == V4L2_OVERLAY_PLANE_VIDEO2)
        return open("/dev/video2", O_RDWR);
    return -EINVAL;
}

int v4l2_resizer_open(void)
{
    LOG_FUNCTION_NAME

    return open("/dev/omap-resizer", O_RDWR);
}

int v4l2_resizer_config(int resizer_fd, uint32_t w, uint32_t h)
{
    int rszRate, ret;
    struct v4l2_requestbuffers reqbuf;
    struct rsz_params  params = {
        0,                              /* in_hsize (set at run time) */
        0,                              /* in_vsize (set at run time) */
        0,                              /* in_pitch (set at run time) */
        RSZ_INTYPE_YCBCR422_16BIT,      /* inptyp */
        0,                              /* vert_starting_pixel */
        0,                              /* horz_starting_pixel */
        0,                              /* cbilin */
        RSZ_PIX_FMT_UYVY,               /* pix_fmt */
        0,                              /* out_hsize (set at run time) */
        0,                              /* out_vsize (set at run time) */
        0,                              /* out_pitch (set at run time) */
        0,                              /* hstph */
        0,                              /* vstph */
        {                               /* hfilt_coeffs */
             0, 256,   0,   0,  -6, 246,  16,   0,
            -7, 219,  44,   0,  -5, 179,  83,  -1,
            -3, 130, 132,  -3,  -1,  83, 179,  -5,
             0,  44, 219,  -7,   0,  16, 246,  -6
        },
        {                               /* vfilt_coeffs */
            -1,  19, 108, 112,  19,  -1,   0,   0,
             0,   6,  88, 126,  37,  -1,   0,   0,
             0,   0,  61, 134,  61,   0,   0,   0,
             0,  -1,  37, 126,  88,   6,   0,   0
        },
        {                               /* yenh_params */
            0,                              /* type */
            0,                              /* gain */
            0,                              /* slop */
            0                               /* core */
        }
    };

    LOG_FUNCTION_NAME

    /* Set up the copy job */
    params.in_hsize  = w;
    params.in_vsize  = h;
    params.in_pitch  = params.in_hsize << 1;
    params.out_hsize = params.in_hsize;
    params.out_vsize = params.in_vsize;
    params.out_pitch = params.in_pitch;

    ret = ioctl(resizer_fd, RSZ_S_PARAM, &params);
    if (ret) {
        LOGE("Framecopy setting parameters failed ret=%d\n",ret);
        return ret;
    }

    rszRate = 0x0;

    ret = ioctl(resizer_fd, RSZ_S_EXP, &rszRate);
    if (ret) {
        LOGE("Framecopy setting read cycle failed\n");
        return ret;
    }
    reqbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_USERPTR;
    reqbuf.count  = 2;

    ret = ioctl(resizer_fd, RSZ_REQBUF, &reqbuf);
    if (ret != reqbuf.count) {
        LOGE("Resizer request buffer failed ret=%d\n",ret);
        return ret;
    }

    return 0;
}

int v4l2_resizer_execute(int resizer_fd, void *src_buf, void *dst_buf)
{
    int i, ret;
    struct v4l2_buffer qbuf[2];

    /* Queue the resizer buffers */
    for (i=0; i < 2; i++) {
        qbuf[i].type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        qbuf[i].memory = V4L2_MEMORY_USERPTR;
        qbuf[i].index  = i;

        ret = ioctl (resizer_fd, RSZ_QUERYBUF, &qbuf[i]);
        if (ret < 0) {
            LOGE("Failed to query buffer index %d\n", i);
            return ret;
        }

        qbuf[i].m.userptr = (i == 0) ? (unsigned long)src_buf :
                                       (unsigned long)dst_buf;

        ret = ioctl (resizer_fd, RSZ_QUEUEBUF, &qbuf[i]);
        if (ret < 0) {
            LOGE("Failed to queue buffer index %d ret=%d\n",i, ret);
            return ret;
        }
    }

    ret = ioctl(resizer_fd, RSZ_RESIZE, NULL);
    if (ret < 0) {
        LOGE("Failed to execute resize job ret=%d\n",ret);
        return ret;
    }

    return 0;
}

void dump_pixfmt(struct v4l2_pix_format *pix)
{
    char *fmt;

    switch (pix->pixelformat) {
        case V4L2_PIX_FMT_YUYV: fmt = "YUYV"; break;
        case V4L2_PIX_FMT_UYVY: fmt = "UYVY"; break;
        case V4L2_PIX_FMT_RGB565: fmt = "RGB565"; break;
        case V4L2_PIX_FMT_RGB565X: fmt = "RGB565X"; break;
        default: fmt = "unsupported"; break;
    }
    LOGI("output pixfmt: w %d, h %d, colorsapce %x, pixfmt %s",
            pix->width, pix->height, pix->colorspace, fmt);
}

void v4l2_overlay_dump_state(int fd)
{
    struct v4l2_format format;
    struct v4l2_crop crop;
    int ret;

    LOGI("dumping driver state:");
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ret = ioctl(fd, VIDIOC_G_FMT, &format);
    if (ret < 0)
        return;
    dump_pixfmt(&format.fmt.pix);

    format.type = V4L2_BUF_TYPE_VIDEO_OVERLAY;
    ret = ioctl(fd, VIDIOC_G_FMT, &format);
    if (ret < 0)
        return;
    LOGI("v4l2_overlay window: l %d, t %d, w %d, h %d",
         format.fmt.win.w.left, format.fmt.win.w.top,
         format.fmt.win.w.width, format.fmt.win.w.height);

    crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ret = ioctl(fd, VIDIOC_G_CROP, &crop);
    if (ret < 0)
        return;
    LOGI("output crop: l %d, t %d, w %d, h %d",
         crop.c.left, crop.c.top, crop.c.width, crop.c.height);
}

static void error(int fd, const char *msg)
{
  LOGE("Error = %s from %s", strerror(errno), msg);
#ifdef OVERLAY_DEBUG
  v4l2_overlay_dump_state(fd);
#endif
}

static int v4l2_overlay_ioctl(int fd, int req, void *arg, const char* msg)
{
    int ret;
    ret = ioctl(fd, req, arg);
    if (ret < 0) {
        error(fd, msg);
        return -1;
    }
    return 0;
}

int configure_pixfmt(struct v4l2_pix_format *pix, int32_t fmt,
                     uint32_t w, uint32_t h)
{
    LOG_FUNCTION_NAME

    int fd;

    switch (fmt) {
        case OVERLAY_FORMAT_RGBA_8888:
            return -1;
        case OVERLAY_FORMAT_RGB_565:
            pix->pixelformat = V4L2_PIX_FMT_RGB565;
            break;
        case OVERLAY_FORMAT_BGRA_8888:
            return -1;
        case OVERLAY_FORMAT_YCbCr_422_SP:
            break;
        case OVERLAY_FORMAT_YCbCr_420_SP:
            return -1;
        case OVERLAY_FORMAT_YCbYCr_422_I:
            pix->pixelformat = V4L2_PIX_FMT_YUYV;
            break;
        case OVERLAY_FORMAT_CbYCrY_422_I:
            pix->pixelformat = V4L2_PIX_FMT_UYVY;
            break;
        case OVERLAY_FORMAT_YCbYCr_420_I:
            return -1;
        case OVERLAY_FORMAT_CbYCrY_420_I:
            return -1;
        default:
            return -1;
    }
    pix->width = w;
    pix->height = h;
    return 0;
}

static void configure_window(struct v4l2_window *win, int32_t w,
                             int32_t h, int32_t x, int32_t y)
{
    LOG_FUNCTION_NAME

    win->w.left = x;
    win->w.top = y;
    win->w.width = w;
    win->w.height = h;
}

void get_window(struct v4l2_format *format, int32_t *x,
                int32_t *y, int32_t *w, int32_t *h)
{
    LOG_FUNCTION_NAME

    *x = format->fmt.win.w.left;
    *y = format->fmt.win.w.top;
    *w = format->fmt.win.w.width;
    *h = format->fmt.win.w.height;
}

int v4l2_overlay_init(int fd, uint32_t w, uint32_t h, uint32_t fmt)
{
    LOG_FUNCTION_NAME

    struct v4l2_format format;
    int ret;

    /* configure the v4l2_overlay framebuffer */
    /*
    ret = v4l2_overlay_ioctl(fd, VIDIOC_G_FBUF, &fbuf, "get fbuf");
    if (ret)
        return ret;
    if (fbuf.fmt.pixelformat != dst_format) {
        fbuf.fmt.pixelformat = dst_format;
        ret = v4l2_overlay_ioctl(fd, VIDIOC_S_FBUF, &fbuf, "set fbuf");
        if (ret)
            return ret;
    }
    */

    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ret = v4l2_overlay_ioctl(fd, VIDIOC_G_FMT, &format, "get format");
    if (ret)
        return ret;
    LOGI("v4l2_overlay_init:: w=%d h=%d\n", format.fmt.pix.width, format.fmt.pix.height);

    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    configure_pixfmt(&format.fmt.pix, fmt, w, h);
    LOGI("v4l2_overlay_init:: w=%d h=%d\n", format.fmt.pix.width, format.fmt.pix.height);
    ret = v4l2_overlay_ioctl(fd, VIDIOC_S_FMT, &format, "set output format");

    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ret = v4l2_overlay_ioctl(fd, VIDIOC_G_FMT, &format, "get output format");
    LOGI("v4l2_overlay_init:: w=%d h=%d\n", format.fmt.pix.width, format.fmt.pix.height);
    return ret;
}

int v4l2_overlay_reinit(int fd)
{
    struct v4l2_format format;
    int ret;

    LOG_FUNCTION_NAME

    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ret = v4l2_overlay_ioctl(fd, VIDIOC_G_FMT, &format, "get format");
    if (ret)
        return ret;
    LOGI("v4l2_overlay_reinit:: w=%d h=%d\n", format.fmt.pix.width, format.fmt.pix.height);

    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    LOGI("v4l2_overlay_init:: w=%d h=%d\n", format.fmt.pix.width, format.fmt.pix.height);
    ret = v4l2_overlay_ioctl(fd, VIDIOC_S_FMT, &format, "set output format");

    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ret = v4l2_overlay_ioctl(fd, VIDIOC_G_FMT, &format, "get output format");
    LOGI("v4l2_overlay_init:: w=%d h=%d\n", format.fmt.pix.width, format.fmt.pix.height);
    return ret;
}

int v4l2_overlay_get_input_size_and_format(int fd, uint32_t *w, uint32_t *h, uint32_t *fmt)
{
    LOG_FUNCTION_NAME

    struct v4l2_format format;
    int ret;

    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ret = v4l2_overlay_ioctl(fd, VIDIOC_G_FMT, &format, "get format");
    *w = format.fmt.pix.width;
    *h = format.fmt.pix.height;
    //if (format.fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY)
    if (format.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV)
        *fmt = OVERLAY_FORMAT_CbYCrY_422_I;
    else return -EINVAL;
    return ret;
}

int v4l2_overlay_set_position(int fd, int32_t x, int32_t y, int32_t w, int32_t h)
{
    LOG_FUNCTION_NAME

    struct v4l2_format format;
    int ret;

#ifdef OVERLAY_USERPTR_BUFFER
    int video_w, video_h, video_fmt, tw, th;

    v4l2_overlay_get_input_size_and_format(fd, &video_w, &video_h, &video_fmt);
#endif

     /* configure the src format pix */
    /* configure the dst v4l2_overlay window */
    format.type = V4L2_BUF_TYPE_VIDEO_OVERLAY;
    ret = v4l2_overlay_ioctl(fd, VIDIOC_G_FMT, &format,
                             "get v4l2_overlay format");
    if (ret)
       return ret;
    LOGI("v4l2_overlay_set_position:: original w=%d h=%d", format.fmt.win.w.width, format.fmt.win.w.height);

#ifdef OVERLAY_USERPTR_BUFFER
    /* set the overlay to full display window if overlay is
     * smaller than video buffer */
    if (video_w > w || video_h > h) {
        LOGI("v4l2_overlay_set_position:: video %dx%d, disp %dx%d", video_w, video_h, w, h);
        configure_window(&format.fmt.win, video_w, video_h, 0, 0);
    } else {
#endif
        configure_window(&format.fmt.win, w, h, x, y);
#ifdef OVERLAY_USERPTR_BUFFER
    }
#endif

    LOGI("v4l2_overlay_set_position:: set to w=%d h=%d", format.fmt.win.w.width, format.fmt.win.w.height);
    format.type = V4L2_BUF_TYPE_VIDEO_OVERLAY;
    ret = v4l2_overlay_ioctl(fd, VIDIOC_S_FMT, &format,
                             "set v4l2_overlay format");
    LOGI("v4l2_overlay_set_position:: new w=%d h=%d", format.fmt.win.w.width, format.fmt.win.w.height);

#ifdef OVERLAY_USERPTR_BUFFER
    /* crop the video buffer to the full display window size in 
     * the center if overlay is smaller than video buffer,
     * so no DSS down scaling in the case */
    if (video_w > w || video_h > h) {
        tw = format.fmt.win.w.width;
        th = format.fmt.win.w.height;
        v4l2_overlay_set_crop(fd, (video_w-tw)/2, (video_h-th)/2, tw, th);
    }

    /* set cropping will change overlay fmt so re-set again to correct it */
    format.type = V4L2_BUF_TYPE_VIDEO_OVERLAY;
    ret = v4l2_overlay_ioctl(fd, VIDIOC_S_FMT, &format,
                             "set v4l2_overlay format");
    LOGI("v4l2_overlay_set_position:: again w=%d h=%d", format.fmt.win.w.width, format.fmt.win.w.height);
#endif

    if (ret)
       return ret;
    v4l2_overlay_dump_state(fd);

    return 0;
}

int v4l2_overlay_get_position(int fd, int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    struct v4l2_format format;
    int ret;

    format.type = V4L2_BUF_TYPE_VIDEO_OVERLAY;
    ret = v4l2_overlay_ioctl(fd, VIDIOC_G_FMT, &format, "get v4l2_overlay format");
    if (ret)
       return ret;
    get_window(&format, x, y, w, h);
    return 0;
}

int v4l2_overlay_set_crop(int fd, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    LOG_FUNCTION_NAME

    struct v4l2_crop crop;
    int ret;

    crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ret = v4l2_overlay_ioctl(fd, VIDIOC_G_CROP, &crop, "get crop");
    crop.c.left = x;
    crop.c.top = y;
    crop.c.width = w;
    crop.c.height = h;
    crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

    return v4l2_overlay_ioctl(fd, VIDIOC_S_CROP, &crop, "set crop");
}

int v4l2_overlay_get_crop(int fd, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h)
{
    LOG_FUNCTION_NAME

    struct v4l2_crop crop;
    int ret;

    crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ret = v4l2_overlay_ioctl(fd, VIDIOC_G_CROP, &crop, "get crop");
    *x = crop.c.left;
    *y = crop.c.top;
    *w = crop.c.width;
    *h = crop.c.height;
    return ret;
}

int v4l2_overlay_set_rotation(int fd, int degree, int step)
{
    LOG_FUNCTION_NAME

    int ret;
    struct v4l2_control ctrl;

    ctrl.id = V4L2_CID_ROTATE;
    ctrl.value = degree;
    ret = v4l2_overlay_ioctl(fd, VIDIOC_S_CTRL, &ctrl, "set rotation");

    return ret;
}

int v4l2_overlay_set_colorkey(int fd, int enable, int colorkey)
{
    LOG_FUNCTION_NAME

    int ret;
    struct v4l2_framebuffer fbuf;
    struct v4l2_format fmt;

    memset(&fbuf, 0, sizeof(fbuf));
    ret = v4l2_overlay_ioctl(fd, VIDIOC_G_FBUF, &fbuf, "get transparency enables");

    if (ret)
        return ret;

    if (enable)
        fbuf.flags |= V4L2_FBUF_FLAG_CHROMAKEY;
    else
        fbuf.flags &= ~V4L2_FBUF_FLAG_CHROMAKEY;

    ret = v4l2_overlay_ioctl(fd, VIDIOC_S_FBUF, &fbuf, "enable colorkey");

    if (ret)
        return ret;

    if (enable)

    {
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_OVERLAY;
        ret = v4l2_overlay_ioctl(fd, VIDIOC_G_FMT, &fmt, "get colorkey");

        if (ret)
            return ret;

        fmt.fmt.win.chromakey = colorkey & 0xFFFFFF;

        ret = v4l2_overlay_ioctl(fd, VIDIOC_S_FMT, &fmt, "set colorkey");
    }

    return ret;
}

int v4l2_overlay_set_global_alpha(int fd, int enable, int alpha)
{
    LOG_FUNCTION_NAME

    int ret;
    struct v4l2_framebuffer fbuf;
    struct v4l2_format fmt;

    memset(&fbuf, 0, sizeof(fbuf));
    ret = v4l2_overlay_ioctl(fd, VIDIOC_G_FBUF, &fbuf, "get transparency enables");

    if (ret)
        return ret;

    if (enable)
        fbuf.flags |= V4L2_FBUF_FLAG_GLOBAL_ALPHA;
    else
        fbuf.flags &= ~V4L2_FBUF_FLAG_GLOBAL_ALPHA;

    ret = v4l2_overlay_ioctl(fd, VIDIOC_S_FBUF, &fbuf, "enable global alpha");

    if (ret)
        return ret;

    if (enable)
    {
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_OVERLAY;
        ret = v4l2_overlay_ioctl(fd, VIDIOC_G_FMT, &fmt, "get global alpha");

        if (ret)
            return ret;

        fmt.fmt.win.global_alpha = alpha & 0xFF;

        ret = v4l2_overlay_ioctl(fd, VIDIOC_S_FMT, &fmt, "set global alpha");
    }

    return ret;
}

int v4l2_overlay_set_local_alpha(int fd, int enable)
{
    int ret;
    struct v4l2_framebuffer fbuf;

    ret = v4l2_overlay_ioctl(fd, VIDIOC_G_FBUF, &fbuf,
                             "get transparency enables");

    if (ret)
        return ret;

    if (enable)
        fbuf.flags |= V4L2_FBUF_FLAG_LOCAL_ALPHA;
    else
        fbuf.flags &= ~V4L2_FBUF_FLAG_LOCAL_ALPHA;

    ret = v4l2_overlay_ioctl(fd, VIDIOC_S_FBUF, &fbuf, "enable global alpha");

    return ret;
}

int v4l2_overlay_req_buf(int fd, uint32_t *num_bufs, int cacheable_buffers)
{
    LOG_FUNCTION_NAME

    struct v4l2_requestbuffers reqbuf;
    int ret, i;
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

#ifdef OVERLAY_USERPTR_BUFFER
    reqbuf.memory = V4L2_MEMORY_USERPTR;
#else
    reqbuf.memory = V4L2_MEMORY_MMAP;
#endif
    reqbuf.count = *num_bufs;
    //reqbuf.reserved[0] = cacheable_buffers;
    ret = ioctl(fd, VIDIOC_REQBUFS, &reqbuf);
    if (ret < 0) {
        error(fd, "reqbuf ioctl");
        return ret;
    }
    LOGI("%d buffers allocated, %d requested\n", reqbuf.count, *num_bufs);
    if (reqbuf.count != *num_bufs) {
        error(fd, "VIDIOC_REQBUFS failed");
        return -ENOMEM;
    }
#ifndef OVERLAY_USERPTR_BUFFER
    *num_bufs = reqbuf.count;
#endif
    LOGI("buffer cookie is %d\n", reqbuf.type);
    return 0;
}

#ifndef OVERLAY_USERPTR_BUFFER
static int is_mmaped(struct v4l2_buffer *buf)
{
    return buf->flags == V4L2_BUF_FLAG_MAPPED;
}
#endif

int v4l2_overlay_query_buffer(int fd, int index, struct v4l2_buffer *buf)
{
    LOG_FUNCTION_NAME

#ifdef OVERLAY_USERPTR_BUFFER
    LOGE("v4l2_overlay_query_buffer() is called in V4L2_MEMORY_USERPTR mode");
    return -1;
#else
    memset(buf, 0, sizeof(struct v4l2_buffer));

    buf->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf->memory = V4L2_MEMORY_MMAP;
    buf->index = index;
    LOGI("query buffer, mem=%u type=%u index=%u\n", buf->memory, buf->type,
         buf->index);
    return v4l2_overlay_ioctl(fd, VIDIOC_QUERYBUF, buf, "querybuf ioctl");
#endif
}

#ifndef OVERLAY_USERPTR_BUFFER
int v4l2_overlay_map_buf(int fd, int index, void **start, size_t *len)
{
    LOG_FUNCTION_NAME

    struct v4l2_buffer buf;
    int ret;

    ret = v4l2_overlay_query_buffer(fd, index, &buf);
    if (ret)
        return ret;

    if (is_mmaped(&buf)) {
        LOGE("Trying to mmap buffers that are already mapped!\n");
        return -EINVAL;
    }

    *len = buf.length;
    *start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                  fd, buf.m.offset);
    if (*start == MAP_FAILED) {
        LOGE("map failed, length=%u offset=%u\n", buf.length, buf.m.offset);
        return -EINVAL;
    }
    return 0;
}

int v4l2_overlay_unmap_buf(void *start, size_t len)
{
    LOG_FUNCTION_NAME

  return munmap(start, len);
}
#endif

int v4l2_overlay_get_caps(int fd, struct v4l2_capability *caps)
{
    return v4l2_overlay_ioctl(fd, VIDIOC_QUERYCAP, caps, "query cap");
}

int v4l2_overlay_stream_on(int fd)
{
    LOG_FUNCTION_NAME

    int ret;
    uint32_t type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

    ret = v4l2_overlay_set_local_alpha(fd, 0);
    if (ret)
        return ret;

    ret = v4l2_overlay_set_global_alpha(fd,1,255);
    if (ret)
        return ret;

    ret =  v4l2_overlay_set_colorkey(fd, 1, 0);
    if (ret)
        return ret;

    ret = v4l2_overlay_ioctl(fd, VIDIOC_STREAMON, &type, "stream on");

    return ret;
}

int v4l2_overlay_stream_off(int fd)
{
    LOG_FUNCTION_NAME

    int ret;
    uint32_t type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

    ret = v4l2_overlay_set_local_alpha(fd, 0);
    if (ret)
        return ret;

    ret = v4l2_overlay_ioctl(fd, VIDIOC_STREAMOFF, &type, "stream off");

    return ret;
}

#ifdef OVERLAY_USERPTR_BUFFER
int v4l2_overlay_q_buf(int fd, void *ptr, size_t len)
{
    /* FIXME not idea to track qbuf index */
    static int index = 0;
#else
int v4l2_overlay_q_buf(int fd, int index)
{
#endif
    struct v4l2_buffer buf;
    int ret;

    memset(&buf, 0, sizeof(buf));

    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
#ifdef OVERLAY_USERPTR_BUFFER
    if (!ptr) {
        LOGE("v4l2_overlay_q_buf() ptr == NULL");
        return -1;
    }
    buf.m.userptr = (unsigned long)ptr;
    buf.length = len;
    buf.index = index++;
    buf.memory = V4L2_MEMORY_USERPTR;
/*    LOGD("v4l2_overlay_q_buf() ptr %p", ptr);*/
    index = index % NUM_OVERLAY_BUFFERS_REQUESTED;
#else
    buf.index = index;
    buf.memory = V4L2_MEMORY_MMAP;
#endif
    buf.field = V4L2_FIELD_NONE;
    buf.timestamp.tv_sec = 0;
    buf.timestamp.tv_usec = 0;
    buf.flags = 0;

    return v4l2_overlay_ioctl(fd, VIDIOC_QBUF, &buf, "qbuf");
}

#ifdef OVERLAY_USERPTR_BUFFER
int v4l2_overlay_dq_buf(int fd, void **ptr)
#else
int v4l2_overlay_dq_buf(int fd, int *index)
#endif
{
    struct v4l2_buffer buf;
    int ret;

    memset(&buf, 0, sizeof(buf));

    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
#ifdef OVERLAY_USERPTR_BUFFER
    buf.memory = V4L2_MEMORY_USERPTR;
#else
    buf.memory = V4L2_MEMORY_MMAP;
#endif

    ret = v4l2_overlay_ioctl(fd, VIDIOC_DQBUF, &buf, "dqbuf");
    if (ret)
      return ret;
#ifdef OVERLAY_USERPTR_BUFFER
    *ptr = (void *)buf.m.userptr;
#else
    *index = buf.index;
#endif
    return 0;
}
