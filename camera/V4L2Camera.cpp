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

#define LOG_TAG "V4L2Camera"
#include <utils/Log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>

#include <linux/videodev.h>

extern "C" {
    #include <jpeglib.h>
}

#include "V4L2Camera.h"

namespace android {

V4L2Camera::V4L2Camera ()
    : nQueued(0), nDequeued(0)
{
    videoIn = (struct vdIn *) calloc (1, sizeof (struct vdIn));
    camHandle = -1;
#ifdef _OMAP_RESIZER_
	videoIn->resizeHandle = -1;
#endif //_OMAP_RESIZER_
}

V4L2Camera::~V4L2Camera()
{
    free(videoIn);
}

int V4L2Camera::Open(const char *device)
{
	int ret = 0;
	LOG_FUNCTION_START

	do
	{
		if ((camHandle = open(device, O_RDWR)) == -1) {
			LOGE("ERROR opening V4L interface: %s", strerror(errno));
			ret = -1;
			break;
		}

		ret = ioctl (camHandle, VIDIOC_QUERYCAP, &videoIn->cap);
		if (ret < 0) {
			LOGE("Error opening device: unable to query device.");
			break;
		}

		if ((videoIn->cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
			LOGE("Error opening device: video capture not supported.");
			ret = -1;
			break;
		}

		if (!(videoIn->cap.capabilities & V4L2_CAP_STREAMING)) {
			LOGE("Capture device does not support streaming i/o");
			ret = -1;
			break;
		}
#ifdef _OMAP_RESIZER_
		videoIn->resizeHandle = OMAPResizerOpen();
#endif //_OMAP_RESIZER_
	}
    while(0);

	LOG_FUNCTION_EXIT
    return ret;
}
int V4L2Camera::Configure(int width,int height,int pixelformat,int fps)
{
	int ret = 0;
	LOG_FUNCTION_START

	struct v4l2_streamparm parm;

    videoIn->width = width;
    videoIn->height = height;
    videoIn->framesizeIn = (width * height << 1);
    videoIn->formatIn = pixelformat;

    videoIn->format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    videoIn->format.fmt.pix.width = width;
    videoIn->format.fmt.pix.height = height;
    videoIn->format.fmt.pix.pixelformat = pixelformat;

	do
	{
		ret = ioctl(camHandle, VIDIOC_S_FMT, &videoIn->format);
		if (ret < 0) {
			LOGE("Open: VIDIOC_S_FMT Failed: %s", strerror(errno));
			break;
		}
		LOGD("CameraConfigure PreviewFormat: w=%d h=%d", videoIn->format.fmt.pix.width, videoIn->format.fmt.pix.height);

		parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		ret = ioctl(camHandle, VIDIOC_G_PARM, &parm);
		if(ret != 0) {
		   LOGD("VIDIOC_G_PARM ");
		   break;
		}

		LOGD("CameraConfigure: Old frame rate is %d/%d  fps",
			parm.parm.capture.timeperframe.denominator,
			parm.parm.capture.timeperframe.numerator);

		ret = ioctl(camHandle, VIDIOC_S_PARM, &parm);
		if(ret != 0) {
			LOGE("VIDIOC_S_PARM  Fail....");
			ret = -1;
			break;
		}
	}while(0);

    LOG_FUNCTION_EXIT
    return ret;
}
int V4L2Camera::BufferMap()
{
    int ret;
    LOG_FUNCTION_START
    /* Check if camera can handle NB_BUFFER buffers */
    videoIn->rb.type 	= V4L2_BUF_TYPE_VIDEO_CAPTURE;
    videoIn->rb.memory 	= V4L2_MEMORY_MMAP;
    videoIn->rb.count 	= NB_BUFFER;

    ret = ioctl(camHandle, VIDIOC_REQBUFS, &videoIn->rb);
    if (ret < 0) {
        LOGE("Init: VIDIOC_REQBUFS failed: %s", strerror(errno));
        return ret;
    }

    for (int i = 0; i < NB_BUFFER; i++) {

        memset (&videoIn->buf, 0, sizeof (struct v4l2_buffer));

        videoIn->buf.index = i;
        videoIn->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        videoIn->buf.memory = V4L2_MEMORY_MMAP;

        ret = ioctl (camHandle, VIDIOC_QUERYBUF, &videoIn->buf);
        if (ret < 0) {
            LOGE("Init: Unable to query buffer (%s)", strerror(errno));
            return ret;
        }

        videoIn->mem[i] = mmap (0,
               videoIn->buf.length,
               PROT_READ | PROT_WRITE,
               MAP_SHARED,
               camHandle,
               videoIn->buf.m.offset);

        if (videoIn->mem[i] == MAP_FAILED) {
            LOGE("Init: Unable to map buffer (%s)", strerror(errno));
            return -1;
        }

        ret = ioctl(camHandle, VIDIOC_QBUF, &videoIn->buf);
        if (ret < 0) {
            LOGE("Init: VIDIOC_QBUF Failed");
            return -1;
        }

        nQueued++;
    }

    LOG_FUNCTION_EXIT
    return 0;
}

void V4L2Camera::Close ()
{
	LOG_FUNCTION_START
    close(camHandle);
    camHandle = -1;
#ifdef _OMAP_RESIZER_
    OMAPResizerClose(videoIn->resizeHandle);
    videoIn->resizeHandle = -1;
#endif //_OMAP_RESIZER_
    LOG_FUNCTION_EXIT
    return;
}

int V4L2Camera::init_parm()
{
    int ret;
    int framerate;
    struct v4l2_streamparm parm;

    LOG_FUNCTION_START
    framerate = DEFAULT_FRAME_RATE;

    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    ret = ioctl(camHandle, VIDIOC_G_PARM, &parm);
    if(ret != 0) {
        LOGE("VIDIOC_G_PARM fail....");
        return ret;
    }

    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = framerate;
    ret = ioctl(camHandle, VIDIOC_S_PARM, &parm);
    if(ret != 0) {
        LOGE("VIDIOC_S_PARM  Fail....");
        return -1;
    }
    LOG_FUNCTION_EXIT
    return 0;
}

void V4L2Camera::Uninit()
{
    int ret;

    LOG_FUNCTION_START

    videoIn->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    videoIn->buf.memory = V4L2_MEMORY_MMAP;

    /* Dequeue everything */
    int DQcount = nQueued - nDequeued;

    for (int i = 0; i < DQcount-1; i++) {
        ret = ioctl(camHandle, VIDIOC_DQBUF, &videoIn->buf);
        if (ret < 0)
            LOGE("Uninit: VIDIOC_DQBUF Failed");
    }
    nQueued = 0;
    nDequeued = 0;

    /* Unmap buffers */
    for (int i = 0; i < NB_BUFFER; i++)
        if (munmap(videoIn->mem[i], videoIn->buf.length) < 0)
            LOGE("Uninit: Unmap failed");

    LOG_FUNCTION_EXIT
    return;
}

int V4L2Camera::StartStreaming ()
{
    enum v4l2_buf_type type;
    int ret;

    LOG_FUNCTION_START
    if (!videoIn->isStreaming) {
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        ret = ioctl (camHandle, VIDIOC_STREAMON, &type);
        if (ret < 0) {
            LOGE("StartStreaming: Unable to start capture: %s", strerror(errno));
            return ret;
        }

        videoIn->isStreaming = true;
    }

    LOG_FUNCTION_EXIT
    return 0;
}

int V4L2Camera::StopStreaming ()
{
    enum v4l2_buf_type type;
    int ret;

    LOG_FUNCTION_START
    if (videoIn->isStreaming) {
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        ret = ioctl (camHandle, VIDIOC_STREAMOFF, &type);
        if (ret < 0) {
            LOGE("StopStreaming: Unable to stop capture: %s", strerror(errno));
            return ret;
        }

        videoIn->isStreaming = false;
    }

    LOG_FUNCTION_EXIT
    return 0;
}

void V4L2Camera::GrabPreviewFrame (void *previewBuffer)
{
    unsigned char *tmpBuffer;
    int ret;

    tmpBuffer = (unsigned char *) calloc (1, videoIn->width * videoIn->height * 2);

    videoIn->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    videoIn->buf.memory = V4L2_MEMORY_MMAP;

    /* DQ */
    ret = ioctl(camHandle, VIDIOC_DQBUF, &videoIn->buf);
    if (ret < 0) {
        LOGE("GrabPreviewFrame: VIDIOC_DQBUF Failed");
        return;
    }
    nDequeued++;

    memcpy(tmpBuffer, videoIn->mem[videoIn->buf.index], (size_t) videoIn->buf.bytesused);

    convert((unsigned char *) tmpBuffer, (unsigned char *) previewBuffer,
            videoIn->width, videoIn->height);

    ret = ioctl(camHandle, VIDIOC_QBUF, &videoIn->buf);
    if (ret < 0) {
        LOGE("GrabPreviewFrame: VIDIOC_QBUF Failed");
        return;
    }

    nQueued++;

    free(tmpBuffer);
}

void V4L2Camera::GrabRawFrame(void *previewBuffer,unsigned int width, unsigned int height)
{
    int ret = 0;
    int DQcount = 0;

    videoIn->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    videoIn->buf.memory = V4L2_MEMORY_MMAP;


    DQcount = nQueued - nDequeued;
    if(DQcount == 0)
    {
    	LOGE("postGrabRawFrame: Drop the frame");
		ret = ioctl(camHandle, VIDIOC_QBUF, &videoIn->buf);
		if (ret < 0) {
			LOGE("postGrabRawFrame: VIDIOC_QBUF Failed");
			return;
		}
    }

    /* DQ */
    ret = ioctl(camHandle, VIDIOC_DQBUF, &videoIn->buf);
    if (ret < 0) {
        LOGE("GrabRawFrame: VIDIOC_DQBUF Failed");
        return;
    }
    nDequeued++;

    if(videoIn->format.fmt.pix.width != width || \
    		videoIn->format.fmt.pix.height != height)
    {
    	//do resize
    	//LOGE("Resizing required");
#ifdef _OMAP_RESIZER_

    ret = OMAPResizerConvert(videoIn->resizeHandle, videoIn->mem[videoIn->buf.index],\
									videoIn->format.fmt.pix.height,\
									videoIn->format.fmt.pix.width,\
									previewBuffer,\
									height,\
									width);
    if(ret < 0)
    	LOGE("Resize operation:%d",ret);
#endif //_OMAP_RESIZER_
    }
    else
    {
    	memcpy(previewBuffer, videoIn->mem[videoIn->buf.index], (size_t) videoIn->buf.bytesused);
    }

    ret = ioctl(camHandle, VIDIOC_QBUF, &videoIn->buf);
    if (ret < 0) {
        LOGE("postGrabRawFrame: VIDIOC_QBUF Failed");
        return;
    }

    nQueued++;
}

int 
V4L2Camera::savePicture(unsigned char *inputBuffer, const char * filename)
{
    FILE *output;
    int fileSize;
    int ret;
    output = fopen(filename, "wb");

    if (output == NULL) {
        LOGE("GrabJpegFrame: Ouput file == NULL");
        return 0;
    }

    fileSize = saveYUYVtoJPEG(inputBuffer, videoIn->width, videoIn->height, output, 100);

    fclose(output);
    return fileSize;
}

void V4L2Camera::GrabJpegFrame (void *captureBuffer)
{
    FILE *output;
    FILE *input;
    int fileSize;
    int ret;

    LOG_FUNCTION_START
    videoIn->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    videoIn->buf.memory = V4L2_MEMORY_MMAP;

    do{
       	LOGE("Dequeue buffer");
		/* Dequeue buffer */
		ret = ioctl(camHandle, VIDIOC_DQBUF, &videoIn->buf);
		if (ret < 0) {
			LOGE("GrabJpegFrame: VIDIOC_DQBUF Failed");
			break;
		}
		nDequeued++;

		LOGE("savePicture");
		fileSize = savePicture((unsigned char *)videoIn->mem[videoIn->buf.index], "/sdcard/tmp.jpg");

		LOGE("VIDIOC_QBUF");

		/* Enqueue buffer */
		ret = ioctl(camHandle, VIDIOC_QBUF, &videoIn->buf);
		if (ret < 0) {
			LOGE("GrabJpegFrame: VIDIOC_QBUF Failed");
			break;
		}
		nQueued++;

		LOGE("fopen temp file");
		input = fopen("/sdcard/tmp.jpg", "rb");

		if (input == NULL)
			LOGE("GrabJpegFrame: Input file == NULL");
		else {
			fread((uint8_t *)captureBuffer, 1, fileSize, input);
			fclose(input);
		}
		break;
    }while(0);

    LOG_FUNCTION_EXIT
    return;
}
void V4L2Camera::CreateJpegFromBuffer(void *rawBuffer,void *captureBuffer)
{
    FILE *output;
    FILE *input;
    int fileSize;
    int ret;

    LOG_FUNCTION_START

    do{
     	LOGE("savePicture");
		fileSize = savePicture((unsigned char *)rawBuffer, "/sdcard/tmp.jpg");

		LOGE("fopen temp file");
		input = fopen("/sdcard/tmp.jpg", "rb");

		if (input == NULL)
			LOGE("GrabJpegFrame: Input file == NULL");
		else {
			fread((uint8_t *)captureBuffer, 1, fileSize, input);
			fclose(input);
		}
		break;
    }while(0);

    LOG_FUNCTION_EXIT
    return;
}
int V4L2Camera::saveYUYVtoJPEG (unsigned char *inputBuffer, int width, int height, FILE *file, int quality)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW row_pointer[1];
    unsigned char *line_buffer, *yuyv;
    int z;
    int fileSize;

    line_buffer = (unsigned char *) calloc (width * 3, 1);
    yuyv = inputBuffer;

    cinfo.err = jpeg_std_error (&jerr);
    jpeg_create_compress (&cinfo);
    jpeg_stdio_dest (&cinfo, file);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults (&cinfo);
    jpeg_set_quality (&cinfo, quality, TRUE);

    jpeg_start_compress (&cinfo, TRUE);

    z = 0;
    while (cinfo.next_scanline < cinfo.image_height) {
        int x;
        unsigned char *ptr = line_buffer;

        for (x = 0; x < width; x++) {
            int r, g, b;
            int y, u, v;

            if (!z)
                y = yuyv[0] << 8;
            else
                y = yuyv[2] << 8;

            u = yuyv[1] - 128;
            v = yuyv[3] - 128;

            r = (y + (359 * v)) >> 8;
            g = (y - (88 * u) - (183 * v)) >> 8;
            b = (y + (454 * u)) >> 8;

            *(ptr++) = (r > 255) ? 255 : ((r < 0) ? 0 : r);
            *(ptr++) = (g > 255) ? 255 : ((g < 0) ? 0 : g);
            *(ptr++) = (b > 255) ? 255 : ((b < 0) ? 0 : b);

            if (z++) {
                z = 0;
                yuyv += 4;
            }
        }

        row_pointer[0] = line_buffer;
        jpeg_write_scanlines (&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress (&cinfo);
    fileSize = ftell(file);
    jpeg_destroy_compress (&cinfo);

    free (line_buffer);

    return fileSize;
}

static inline void yuv_to_rgb16(unsigned char y,
                                unsigned char u,
                                unsigned char v,
                                unsigned char *rgb)
{
    register int r,g,b;
    int rgb16;

    r = (1192 * (y - 16) + 1634 * (v - 128) ) >> 10;
    g = (1192 * (y - 16) - 833 * (v - 128) - 400 * (u -128) ) >> 10;
    b = (1192 * (y - 16) + 2066 * (u - 128) ) >> 10;

    r = r > 255 ? 255 : r < 0 ? 0 : r;
    g = g > 255 ? 255 : g < 0 ? 0 : g;
    b = b > 255 ? 255 : b < 0 ? 0 : b;

    rgb16 = (int)(((r >> 3)<<11) | ((g >> 2) << 5)| ((b >> 3) << 0));

    *rgb = (unsigned char)(rgb16 & 0xFF);
    rgb++;
    *rgb = (unsigned char)((rgb16 & 0xFF00) >> 8);

}

void V4L2Camera::convert(unsigned char *buf, unsigned char *rgb, int width, int height)
{
    int x,y,z=0;
    int blocks;

    blocks = (width * height) * 2;

    for (y = 0; y < blocks; y+=4) {
        unsigned char Y1, Y2, U, V;

        Y1 = buf[y + 0];
        U = buf[y + 1];
        Y2 = buf[y + 2];
        V = buf[y + 3];

        yuv_to_rgb16(Y1, U, V, &rgb[y]);
        yuv_to_rgb16(Y2, U, V, &rgb[y + 2]);
    }
}



}; // namespace android
