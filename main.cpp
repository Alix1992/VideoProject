#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include "camerareader.h"

void *thread_function(void*)
{
    printf("thread");
   // const char *pucDevice =  "/dev/video1";
    //CameraReader reader;
    //reader.start(pucDevice);
}
int main()
{
   
    int tmp1, tmp2;
    void *retval;
    pthread_t thread1,thread2;
    int ret_thrd1, ret_thrd2;
    ret_thrd1 = pthread_create(&thread1,NULL,thread_function,NULL);

    if(1 == ret_thrd1)
    {
	printf("thread create err\n");
    }

    pid_t pid,pid1;
    pid = fork(); 
    if(pid<0)
    {
    }
    else if(pid == 0)
    {      
      
      static struct buffer *buffers;
      static unsigned int n_buffers;
      static IO_METHOD io_method;
      static MPP_ENC_DATA mpp_enc_data;

      const char *pucDevice =  "/dev/video2";
      const char *output_file =  "/tmp/pid0.h264";
      CameraReader reader;
      reader.start(pucDevice,output_file);

    }

    pid1 = fork();
    if(pid1<0)
    {
    }
    else if(pid1 == 0)
    {
      static struct buffer *buffers;
      static unsigned int n_buffers;
      static IO_METHOD io_method;
      static MPP_ENC_DATA mpp_enc_data;

      const char *pucDevice =  "/dev/video1";
      const char *output_file =  "/tmp/pid1.h264";
      CameraReader reader_pid1;
      reader_pid1.start(pucDevice,output_file);

    }


   

    return 0;

}
