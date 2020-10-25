#ifndef CAMERAREADER_H
#define CAMERAREADER_H

#include <stdint.h>
#include <rk_mpi.h>

struct MPP_ENC_DATA
{
	// global flow control flag
	uint32_t frm_eos;
	uint32_t pkt_eos;
	uint32_t frame_count;
	uint64_t stream_size;

	// base flow context
	MppCtx ctx;
	MppApi *mpi;
	MppEncPrepCfg prep_cfg;
	MppEncRcCfg rc_cfg;
	MppEncCodecCfg codec_cfg;

	// input / output
	MppBuffer frm_buf;
	MppEncSeiMode sei_mode;

	uint32_t width;
	uint32_t height;
	uint32_t hor_stride;
	uint32_t ver_stride;
	MppFrameFormat fmt;
	MppCodingType type;
	uint32_t num_frames;

	// resources
	size_t frame_size;

	int32_t gop;
	int32_t fps;
	int32_t bps;

	FILE *fp_output;
};

enum IO_METHOD
{
    IO_METHOD_READ,
    IO_METHOD_MMAP,
    //IO_METHOD_USERPTR,
};

struct buffer
{
    void   *start;
    size_t  length;
};


class CameraReader
{

public:

   // static struct buffer *buffers;
   // static unsigned int n_buffers;
   // static IO_METHOD io_method;
   // static MPP_ENC_DATA mpp_enc_data;
    CameraReader();

    void start(const char *pucDevice,const char *output_file);

private:
    bool process_image(uint8_t *p, int size);
    int read_frame(int fd);
    void main_loop(int fd);
    void stop_capturing(int fd);
    int start_capturing(int fd);
    void uninit_device(void);
    int init_read(unsigned int buffer_size);
    int init_mmap(int fd);
    int init_device(int fd);
    void init_mpp(const char *output_file);
    void close_device(int fd);
    void destroy_mpp();
    int open_device(const char *pucDevice);

private:
    int camera_fd;
};

#endif // CAMERAREADER_H
