#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <string.h>
#include <errno.h>
#include <rk_mpi.h>

#include "camerareader.h"

#define CAMERA_WIDTH 1280  //320
#define CAMERA_HEIGHT 720 //240
#define FORCE_FORMAT 1
#define DEVICE_FILE "/dev/video2"
#define MPP_ALIGN(x, a)         (((x)+(a)-1)&~((a)-1))

static struct buffer *buffers;
static unsigned int n_buffers;
static IO_METHOD io_method;

static MPP_ENC_DATA mpp_enc_data;

CameraReader::CameraReader()
{

}

static void cleanup(void *p)
{
    free(p);
}

static int xioctl(int fh, int request, void *arg)
{
    int r;
    do
    {
        r = ioctl(fh, request, arg);
    } while (-1 == r && EINTR == errno);

    return r;
}

static uint8_t check_pix(int a)
{
    if(a < 0)
        a = 0;
    else if(a > 255)
        a = 255;

    return a;
}


bool CameraReader::process_image(uint8_t *p, int size)
{
	MPP_RET ret = MPP_OK;
    if(size != CAMERA_WIDTH * CAMERA_HEIGHT * 2)
    {
        fprintf(stderr, "Invalid image data buffer!\n");
        return true;
    }
    int cycleStart;
    int cycleEnd;
    struct timeval tv;
    MppFrame frame = NULL;
    MppPacket packet = NULL;
    void *buf = mpp_buffer_get_ptr(mpp_enc_data.frm_buf);

    //TODO: improve performance here?
    gettimeofday(&tv,NULL);
    cycleStart = tv.tv_sec*1000000+tv.tv_usec;
    memcpy(buf, p, size);
    gettimeofday(&tv,NULL);
    cycleEnd = tv.tv_sec*1000000+tv.tv_usec;
    printf("memcpy cycle = %d\t size = %d \n",cycleEnd-cycleStart,size);
    ret = mpp_frame_init(&frame);
    if (ret)
    {
    	printf("mpp_frame_init failed\n");
    	return true;
    }

    mpp_frame_set_width(frame, mpp_enc_data.width);
    mpp_frame_set_height(frame, mpp_enc_data.height);
    mpp_frame_set_hor_stride(frame, mpp_enc_data.hor_stride);
    mpp_frame_set_ver_stride(frame, mpp_enc_data.ver_stride);
    mpp_frame_set_fmt(frame, mpp_enc_data.fmt);
    mpp_frame_set_buffer(frame, mpp_enc_data.frm_buf);
    mpp_frame_set_eos(frame, mpp_enc_data.frm_eos);

    ret = mpp_enc_data.mpi->encode_put_frame(mpp_enc_data.ctx, frame);
    if (ret)
    {
    	printf("mpp encode put frame failed\n");
    	return true;
    }

    ret = mpp_enc_data.mpi->encode_get_packet(mpp_enc_data.ctx, &packet);
    if (ret)
    {
    	printf("mpp encode get packet failed\n");
    	return true;
    }

    if (packet)
    {
    	// write packet to file here
    	void *ptr   = mpp_packet_get_pos(packet);
    	size_t len  = mpp_packet_get_length(packet);

    	mpp_enc_data.pkt_eos = mpp_packet_get_eos(packet);

    	if (mpp_enc_data.fp_output)
    		fwrite(ptr, 1, len, mpp_enc_data.fp_output);
    	mpp_packet_deinit(&packet);

    	printf("encoded frame %d size %d\n", mpp_enc_data.frame_count, len);
    	mpp_enc_data.stream_size += len;
    	mpp_enc_data.frame_count++;

    	if (mpp_enc_data.pkt_eos)
    	{
    		printf("found last packet\n");
    	}
    }

    if (mpp_enc_data.num_frames && mpp_enc_data.frame_count >= mpp_enc_data.num_frames)
    {
    	printf("encode max %d frames", mpp_enc_data.frame_count);
    	return false;
    }

    if (mpp_enc_data.frm_eos && mpp_enc_data.pkt_eos)
    	return false;

    return true;
}

int CameraReader::read_frame(int fd)
{
    struct v4l2_buffer buf;
    switch (io_method)
    {

    case IO_METHOD_READ:
        if (read(fd, buffers[0].start, buffers[0].length) == -1)
        {
            if(errno == EAGAIN || errno == EINTR)
            {
                return 0;
            }
            else
            {
                fprintf(stderr, "read failed: %d, %s\n", errno, strerror(errno));
                return -1;
            }
        }

        if(!process_image((uint8_t*)buffers[0].start, buffers[0].length))
        {
        	return -2;
        }
        break;

    case IO_METHOD_MMAP:
        memset(&buf, 0, sizeof(v4l2_buffer));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(fd, VIDIOC_DQBUF, &buf) == -1)
        {
            if(errno == EAGAIN || errno == EINTR)
            {
                return 0;
            }
            else
            {
                fprintf(stderr, "set VIDIOC_DQBUF failed: %d, %s\n", errno, strerror(errno));
                return -1;
            }
        }

        if(buf.index < n_buffers)
        {
            if(!process_image((uint8_t*)buffers[buf.index].start, buf.bytesused))
            {
            	return -2;
            }
            if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
            {
                fprintf(stderr, "set VIDIOC_QBUF failed: %d, %s\n", errno, strerror(errno));
                return -1;
            }
        }
        break;
    }

    return 0;
}

void CameraReader::main_loop(int fd)
{
    int r;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);

    for (;;)
    {
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        fd_set rdset = fds;

        r = select(fd + 1, &rdset, NULL, NULL, &tv);

        if(r > 0)
        {
            if(read_frame(fd) == -2)
            	break;
        }
        else if(r == 0)
        {
            fprintf(stderr, "select timeout\\n");
        }
        else
        {
            if (EINTR == errno || EAGAIN == errno)
                continue;
            fprintf(stderr, "select failed: %d, %s\n", errno, strerror(errno));
            break;
        }
        /* EAGAIN - continue select loop. */
    }
}

void CameraReader::stop_capturing(int fd)
{
    enum v4l2_buf_type type;

    switch (io_method)
    {
    case IO_METHOD_READ:
        /* Nothing to do. */
        break;
    case IO_METHOD_MMAP:
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd, VIDIOC_STREAMOFF, &type);
        break;
    }
}

int CameraReader::start_capturing(int fd)
{
    unsigned int i;
    enum v4l2_buf_type type;

    switch (io_method)
    {
    case IO_METHOD_READ:
        /* Nothing to do. */
        break;
    case IO_METHOD_MMAP:
        for (i = 0; i < n_buffers; ++i)
        {
            struct v4l2_buffer buf;
            memset(&buf, 0, sizeof(buf));

            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            if (xioctl(fd, VIDIOC_QBUF, &buf) == -1)
            {
                fprintf(stderr, "set VIDIOC_QBUF failed: %d, %s\n", errno, strerror(errno));
                return -1;
            }
        }
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd, VIDIOC_STREAMON, &type) == -1)
        {
            fprintf(stderr, "set VIDIOC_STREAMON failed: %d, %s\n", errno, strerror(errno));
            return -1;
        }
        break;
    }

    return 0;
}

void CameraReader::uninit_device(void)
{
    unsigned int i;

    switch (io_method)
    {
    case IO_METHOD_READ:
        free(buffers[0].start);
        break;
    case IO_METHOD_MMAP:
        for (i = 0; i < n_buffers; ++i)
            munmap(buffers[i].start, buffers[i].length);
        break;
    }

    free(buffers);
}

int CameraReader::init_read(unsigned int buffer_size)
{
    buffers = (buffer*)calloc(1, sizeof(*buffers));

    if (!buffers)
    {
        fprintf(stderr, "Out of memory\n");
        return -1;
    }

    buffers[0].length = buffer_size;
    buffers[0].start = malloc(buffer_size);

    if (!buffers[0].start)
    {
        fprintf(stderr, "Out of memory\n");
        return -1;
    }

    return 0;
}

int CameraReader::init_mmap(int fd)
{
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));

    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if(xioctl(fd, VIDIOC_REQBUFS, &req) == -1)
    {
        fprintf(stderr, "set VIDIOC_REQBUFS failed: %d, %s\n", errno, strerror(errno));
        return -1;
    }

    if (req.count < 2)
    {
        fprintf(stderr, "Insufficient buffer memory on %s\n",
                DEVICE_FILE);
        return -1;
    }

    buffers = (buffer*)calloc(req.count, sizeof(*buffers));

    if (!buffers)
    {
        fprintf(stderr, "Out of memory\n");
        return -1;
    }

    for (n_buffers = 0; n_buffers < req.count; ++n_buffers)
    {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));

        buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory      = V4L2_MEMORY_MMAP;
        buf.index       = n_buffers;

        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) == -1)
        {
            fprintf(stderr, "set VIDIOC_QUERYBUF %u failed: %d, %s\n", n_buffers, errno, strerror(errno));
            return -1;
        }

        buffers[n_buffers].length = buf.length;
        buffers[n_buffers].start =
                mmap(NULL /* start anywhere */,
                     buf.length,
                     PROT_READ | PROT_WRITE /* required */,
                     MAP_SHARED /* recommended */,
                     fd, buf.m.offset);

        if (MAP_FAILED == buffers[n_buffers].start)
        {
            fprintf(stderr, "mmap %u failed: %d, %s\n", n_buffers, errno, strerror(errno));
            return -1;
        }
    }

    return 0;
}

void CameraReader::init_mpp(const char *output_file)
{
	MPP_RET ret = MPP_OK;
	memset(&mpp_enc_data, 0, sizeof(mpp_enc_data));

	mpp_enc_data.width = CAMERA_WIDTH;
	mpp_enc_data.height = CAMERA_HEIGHT;
	mpp_enc_data.hor_stride = MPP_ALIGN(mpp_enc_data.width, 16);
	mpp_enc_data.ver_stride = MPP_ALIGN(mpp_enc_data.height, 16);
	mpp_enc_data.fmt = MPP_FMT_YUV422_YUYV;
	mpp_enc_data.frame_size = mpp_enc_data.hor_stride * mpp_enc_data.ver_stride * 2;
	mpp_enc_data.type = MPP_VIDEO_CodingAVC;
	mpp_enc_data.num_frames = 2000;

	ret = mpp_buffer_get(NULL, &mpp_enc_data.frm_buf, mpp_enc_data.frame_size);
	if (ret)
	{
		printf("failed to get buffer for input frame ret %d\n", ret);
		goto MPP_INIT_OUT;
	}

	ret = mpp_create(&mpp_enc_data.ctx, &mpp_enc_data.mpi);
	if (ret)
	{
		printf("mpp_create failed ret %d\n", ret);
		goto MPP_INIT_OUT;
	}

	ret = mpp_init(mpp_enc_data.ctx, MPP_CTX_ENC, mpp_enc_data.type);
	if (ret)
	{
		printf("mpp_init failed ret %d\n", ret);
		goto MPP_INIT_OUT;
	}

	mpp_enc_data.fps = 20;
	mpp_enc_data.gop = 60;
	mpp_enc_data.bps = mpp_enc_data.width * mpp_enc_data.height / 8 * mpp_enc_data.fps;

	mpp_enc_data.prep_cfg.change        = MPP_ENC_PREP_CFG_CHANGE_INPUT |
			MPP_ENC_PREP_CFG_CHANGE_ROTATION |
			MPP_ENC_PREP_CFG_CHANGE_FORMAT;
	mpp_enc_data.prep_cfg.width         = mpp_enc_data.width;
	mpp_enc_data.prep_cfg.height        = mpp_enc_data.height;
	mpp_enc_data.prep_cfg.hor_stride    = mpp_enc_data.hor_stride;
	mpp_enc_data.prep_cfg.ver_stride    = mpp_enc_data.ver_stride;
	mpp_enc_data.prep_cfg.format        = mpp_enc_data.fmt;
	mpp_enc_data.prep_cfg.rotation      = MPP_ENC_ROT_0;
	ret = mpp_enc_data.mpi->control(mpp_enc_data.ctx, MPP_ENC_SET_PREP_CFG, &mpp_enc_data.prep_cfg);
	if (ret)
	{
		printf("mpi control enc set prep cfg failed ret %d\n", ret);
		goto MPP_INIT_OUT;
	}

	mpp_enc_data.rc_cfg.change  = MPP_ENC_RC_CFG_CHANGE_ALL;
	mpp_enc_data.rc_cfg.rc_mode = MPP_ENC_RC_MODE_CBR;
	mpp_enc_data.rc_cfg.quality = MPP_ENC_RC_QUALITY_MEDIUM;

	if (mpp_enc_data.rc_cfg.rc_mode == MPP_ENC_RC_MODE_CBR)
	{
		/* constant bitrate has very small bps range of 1/16 bps */
		mpp_enc_data.rc_cfg.bps_target   = mpp_enc_data.bps;
		mpp_enc_data.rc_cfg.bps_max      = mpp_enc_data.bps * 17 / 16;
		mpp_enc_data.rc_cfg.bps_min      = mpp_enc_data.bps * 15 / 16;
	}
	else if (mpp_enc_data.rc_cfg.rc_mode ==  MPP_ENC_RC_MODE_VBR)
	{
		if (mpp_enc_data.rc_cfg.quality == MPP_ENC_RC_QUALITY_CQP)
		{
			/* constant QP does not have bps */
			mpp_enc_data.rc_cfg.bps_target   = -1;
			mpp_enc_data.rc_cfg.bps_max      = -1;
			mpp_enc_data.rc_cfg.bps_min      = -1;
		}
		else
		{
			/* variable bitrate has large bps range */
			mpp_enc_data.rc_cfg.bps_target   = mpp_enc_data.bps;
			mpp_enc_data.rc_cfg.bps_max      = mpp_enc_data.bps * 17 / 16;
			mpp_enc_data.rc_cfg.bps_min      = mpp_enc_data.bps * 1 / 16;
		}
	}

	/* fix input / output frame rate */
	mpp_enc_data.rc_cfg.fps_in_flex      = 0;
	mpp_enc_data.rc_cfg.fps_in_num       = mpp_enc_data.fps;
	mpp_enc_data.rc_cfg.fps_in_denorm    = 1;
	mpp_enc_data.rc_cfg.fps_out_flex     = 0;
	mpp_enc_data.rc_cfg.fps_out_num      = mpp_enc_data.fps;
	mpp_enc_data.rc_cfg.fps_out_denorm   = 1;

	mpp_enc_data.rc_cfg.gop              = mpp_enc_data.gop;
	mpp_enc_data.rc_cfg.skip_cnt         = 0;

	ret = mpp_enc_data.mpi->control(mpp_enc_data.ctx, MPP_ENC_SET_RC_CFG, &mpp_enc_data.rc_cfg);
	if (ret)
	{
		printf("mpi control enc set rc cfg failed ret %d\n", ret);
		goto MPP_INIT_OUT;
	}

	mpp_enc_data.codec_cfg.coding = mpp_enc_data.type;
	switch (mpp_enc_data.codec_cfg.coding)
	{
	case MPP_VIDEO_CodingAVC :
	{
		mpp_enc_data.codec_cfg.h264.change = MPP_ENC_H264_CFG_CHANGE_PROFILE |
				MPP_ENC_H264_CFG_CHANGE_ENTROPY |
				MPP_ENC_H264_CFG_CHANGE_TRANS_8x8;
		/*
		 * H.264 profile_idc parameter
		 * 66  - Baseline profile
		 * 77  - Main profile
		 * 100 - High profile
		 */
		mpp_enc_data.codec_cfg.h264.profile  = 100;
		/*
		 * H.264 level_idc parameter
		 * 10 / 11 / 12 / 13    - qcif@15fps / cif@7.5fps / cif@15fps / cif@30fps
		 * 20 / 21 / 22         - cif@30fps / half-D1@@25fps / D1@12.5fps
		 * 30 / 31 / 32         - D1@25fps / 720p@30fps / 720p@60fps
		 * 40 / 41 / 42         - 1080p@30fps / 1080p@30fps / 1080p@60fps
		 * 50 / 51 / 52         - 4K@30fps
		 */
		mpp_enc_data.codec_cfg.h264.level    = 31;
		mpp_enc_data.codec_cfg.h264.entropy_coding_mode  = 1;
		mpp_enc_data.codec_cfg.h264.cabac_init_idc  = 0;
		mpp_enc_data.codec_cfg.h264.transform8x8_mode = 1;
	}
	break;
	case MPP_VIDEO_CodingMJPEG :
	{
		mpp_enc_data.codec_cfg.jpeg.change  = MPP_ENC_JPEG_CFG_CHANGE_QP;
		mpp_enc_data.codec_cfg.jpeg.quant   = 10;
	}
	break;
	case MPP_VIDEO_CodingVP8 :
	case MPP_VIDEO_CodingHEVC :
	default :
	{
		printf("support encoder coding type %d\n", mpp_enc_data.codec_cfg.coding);
	}
	break;
	}
	ret = mpp_enc_data.mpi->control(mpp_enc_data.ctx, MPP_ENC_SET_CODEC_CFG, &mpp_enc_data.codec_cfg);
	if (ret)
	{
		printf("mpi control enc set codec cfg failed ret %d\n", ret);
		goto MPP_INIT_OUT;
	}

	/* optional */
	mpp_enc_data.sei_mode = MPP_ENC_SEI_MODE_ONE_FRAME;
	ret = mpp_enc_data.mpi->control(mpp_enc_data.ctx, MPP_ENC_SET_SEI_CFG, &mpp_enc_data.sei_mode);
	if (ret)
	{
		printf("mpi control enc set sei cfg failed ret %d\n", ret);
		goto MPP_INIT_OUT;
	}

	//mpp_enc_data.fp_output = fopen("/tmp/output_luoji.h264", "wb+");
	  mpp_enc_data.fp_output = fopen(output_file, "wb+");

	if (mpp_enc_data.type == MPP_VIDEO_CodingAVC)
	{
		MppPacket packet = NULL;
		ret = mpp_enc_data.mpi->control(mpp_enc_data.ctx, MPP_ENC_GET_EXTRA_INFO, &packet);
		if (ret)
		{
			printf("mpi control enc get extra info failed\n");
			goto MPP_INIT_OUT;
		}

		/* get and write sps/pps for H.264 */
		if (packet)
		{
			void *ptr   = mpp_packet_get_pos(packet);
			size_t len  = mpp_packet_get_length(packet);

			if (mpp_enc_data.fp_output)
				fwrite(ptr, 1, len, mpp_enc_data.fp_output);

			packet = NULL;
		}
	}

	return;

MPP_INIT_OUT:

    if (mpp_enc_data.ctx)
    {
        mpp_destroy(mpp_enc_data.ctx);
        mpp_enc_data.ctx = NULL;
    }

    if (mpp_enc_data.frm_buf)
    {
        mpp_buffer_put(mpp_enc_data.frm_buf);
        mpp_enc_data.frm_buf = NULL;
    }

    printf("init mpp failed!\n");
}

void CameraReader::destroy_mpp()
{
	MPP_RET ret = MPP_OK;
	ret = mpp_enc_data.mpi->reset(mpp_enc_data.ctx);
	if (ret)
	{
		printf("mpi->reset failed\n");
	}

	if (mpp_enc_data.ctx)
	{
		mpp_destroy(mpp_enc_data.ctx);
		mpp_enc_data.ctx = NULL;
	}

	if (mpp_enc_data.frm_buf)
	{
		mpp_buffer_put(mpp_enc_data.frm_buf);
		mpp_enc_data.frm_buf = NULL;
	}

	fclose(mpp_enc_data.fp_output);
}

int CameraReader::init_device(int fd)
{
    struct v4l2_capability cap;
    struct v4l2_cropcap cropcap;
    struct v4l2_crop crop;
    struct v4l2_format fmt;
    unsigned int min;

    if(xioctl(fd, VIDIOC_QUERYCAP, &cap) == -1)
    {
        fprintf(stderr, "get VIDIOC_QUERYCAP error: %d, %s\n", errno, strerror(errno));
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
    {
        fprintf(stderr, "%s is not video capture device\n",
                DEVICE_FILE);
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING))
    {
        fprintf(stderr, "%s does not support streaming i/o\n", DEVICE_FILE);

        if (!(cap.capabilities & V4L2_CAP_READWRITE))
        {
            fprintf(stderr, "%s does not support read i/o\n", DEVICE_FILE);
            return -1;
        }
        io_method = IO_METHOD_READ;
	printf("IO_METHOD_READ");
    }
    else
    {
        io_method = IO_METHOD_MMAP;
	printf("IO_METHOD_MMAP = %x\n",cap.capabilities);
    }


    /* Select video input, video standard and tune here. */


    memset(&cropcap, 0, sizeof(cropcap));
    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_CROPCAP, &cropcap) == 0)
    {
        memset(&crop, 0, sizeof(crop));
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        crop.c = cropcap.defrect; /* reset to default */

        if (xioctl(fd, VIDIOC_S_CROP, &crop) == -1)
        {
            fprintf(stderr, "set VIDIOC_S_CROP failed: %d, %s\n", errno, strerror(errno));
        }
    }
    else
    {
        fprintf(stderr, "get VIDIOC_CROPCAP failed: %d, %s\n", errno, strerror(errno));
    }

    /* Enum pixel format */
    for(int i=0; i<20; i++)
    {
        struct v4l2_fmtdesc fmtdesc;
        fmtdesc.index = i;
        fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if(xioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == -1)
            break;

        printf("%d: %s\n", i, fmtdesc.description);
    }


    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (FORCE_FORMAT)
    {
        fmt.fmt.pix.width       = CAMERA_WIDTH;
        fmt.fmt.pix.height      = CAMERA_HEIGHT;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field       = V4L2_FIELD_INTERLACED;

        if (xioctl(fd, VIDIOC_S_FMT, &fmt) == -1)
        {
            fprintf(stderr, "get VIDIOC_S_FMT failed: %d, %s\n", errno, strerror(errno));
            return -1;
        }

        /* Note VIDIOC_S_FMT may change width and height. */
    }
    else
    {
        /* Preserve original settings as set by v4l2-ctl for example */
        if (xioctl(fd, VIDIOC_G_FMT, &fmt) == -1)
        {
            fprintf(stderr, "get VIDIOC_G_FMT failed: %d, %s\n", errno, strerror(errno));
            return -1;
        }
    }

    /* Buggy driver paranoia. */
    min = fmt.fmt.pix.width * 2;
    if (fmt.fmt.pix.bytesperline < min)
        fmt.fmt.pix.bytesperline = min;
    min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
    if (fmt.fmt.pix.sizeimage < min)
        fmt.fmt.pix.sizeimage = min;

    if(io_method == IO_METHOD_MMAP)
        return init_mmap(fd);
    else
        return init_read(fmt.fmt.pix.sizeimage);
}

void CameraReader::close_device(int fd)
{
    close(fd);
}

int CameraReader::open_device(const char *pucDevice)
{
    struct stat st;
    if (stat(pucDevice, &st) == -1)
    {
        fprintf(stderr, "Cannot identify '%s': %d, %s\n",
                DEVICE_FILE, errno, strerror(errno));
        return -1;
    }

    if (!S_ISCHR(st.st_mode))
    {
        fprintf(stderr, "%s is not device", DEVICE_FILE);
        return -1;
    }

    int fd = open(pucDevice, O_RDWR /* required */ | O_NONBLOCK, 0);
    if (fd == -1)
    {
        fprintf(stderr, "Cannot open '%s': %d, %s\\n",
                DEVICE_FILE, errno, strerror(errno));
        return -1;
    }

    return fd;
}

void CameraReader::start(const char *pucDevice,const char *output_file)
{
    camera_fd = open_device(pucDevice);
    init_device(camera_fd);
    init_mpp(output_file);
    start_capturing(camera_fd);
    main_loop(camera_fd);
    stop_capturing(camera_fd);
    destroy_mpp();
    uninit_device();
    close_device(camera_fd);
}
