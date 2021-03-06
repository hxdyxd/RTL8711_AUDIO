#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>

#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/if_tun.h>
#include <linux/if.h>

#include "app_debug.h"
#include "uart2.h"
#include "slip.h"

#define CHECK_SUM_ON   1
#define BUF_SIZE       4000
//#define DEBUG
//Slip
static volatile uint8_t slip_out_task_run_flag = 0;
static pthread_t gs_slip_out_task_pthread_id;

unsigned char tun_out_buffer[BUF_SIZE];
unsigned char slip_out_buffer[BUF_SIZE];

void signal_proc(int signo);

//Tun
int tun_alloc(int flags);

//Slip
int slip_out_task_start(int *tun_fd);
int slip_out_task_stop(void);
void *slip_out_task_proc(void *par);

#if CHECK_SUM_ON
//Checksum
int if_api_calculate_checksum(void *buf, unsigned int len);
int if_api_check(void *buf, unsigned int len);
#endif

int main(int argc, char *argv[])
{
    if(argc < 3) {
        APP_DEBUG("Usage: %s /dev/ttyUSB0 115200\n\n", argv[0]);
        return 0;
    }

    signal(SIGINT, signal_proc);
    signal(SIGTERM, signal_proc);

    APP_DEBUG("Open TUN/TAP device\n" );
    /* Flags: IFF_TUN   - TUN device (no Ethernet headers)
     *        IFF_TAP   - TAP device
     *        IFF_NO_PI - Do not provide packet information
     */
    int tun_fd = tun_alloc(IFF_TAP | IFF_NO_PI);
    if (tun_fd < 0) {
        APP_ERROR("Allocating interface\n");
        return 0;
    }

    APP_DEBUG("Open UART device %s %d\n", argv[1], atoi(argv[2]) );
    int err = uart2_init(argv[1], atoi(argv[2]));
    if(err < 0) {
        APP_ERROR("UART init failed\n\n");
        return 0;
    }

/*  uint8_t buf[256];
    for(int i=0;i<256;i++) {
        buf[i] = i;
    }
    send_packet(buf, 256);

    while(1) {
        send_packet(buf, 256);
        usleep(100000);
    }

    return 0;*/


    slip_out_task_start(&tun_fd);

    while(slip_out_task_run_flag) {
        int total_len = read(tun_fd, tun_out_buffer, BUF_SIZE);
        if (total_len < 0) {
            APP_ERROR("Reading from interface\n");
            close(tun_fd);
            slip_out_task_stop();
            return -1;
        }
        //PRINTF("TUN R=%d\n", len);
#if CHECK_SUM_ON
        total_len = if_api_calculate_checksum(tun_out_buffer, total_len);
#endif

        send_packet(tun_out_buffer, total_len);
    }

    return 0;
}

void *slip_out_task_proc(void *par)
{
    int *tun_fd = (int *)par;
    APP_DEBUG("slip out task enter success!\n");
    while(slip_out_task_run_flag) {
        int total_len = recv_packet(slip_out_buffer, BUF_SIZE);

#if CHECK_SUM_ON
        int temp_len = total_len;
        total_len = if_api_check(slip_out_buffer, total_len);
        if(total_len < 0) {
            APP_DEBUG("lost %d bytes\n", temp_len);
            continue;
        }
#endif

#ifdef DEBUG
        PRINTF("SLIP R=%d\n", total_len);

/*      for(int i=0;i<total_len;i++) {
            printf("%02x ", slip_out_buffer[i]);
        }
        printf("\n");*/
#endif

        write(*tun_fd, slip_out_buffer, total_len);
    }
    APP_DEBUG("slip out task exit success!\n");
    return NULL;
}

int slip_out_task_start(int *tun_fd)
{
    slip_out_task_run_flag = 1;
    APP_DEBUG("slip out task start!\n");

    return pthread_create(&gs_slip_out_task_pthread_id, 0, slip_out_task_proc, tun_fd);
}

int slip_out_task_stop(void)
{
    if (slip_out_task_run_flag == 1) {
        slip_out_task_run_flag = 0;
        pthread_join(gs_slip_out_task_pthread_id, 0);
        //APP_DEBUG("slip out task exit success!\n");
    } else {
        APP_WARN("slip out task failed!\n");
        return -1;
    }
    return 0;
}

void signal_proc(int signo)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    if (SIGINT == signo || SIGTERM == signo)
    {
        printf("\n-----------------------------------------------------\n");
        slip_out_task_stop();
        printf("\033[0;31mprogram termination normally!\033[0;39m\n");
    }
    exit(-1);
}


int tun_alloc(int flags)
{

    struct ifreq ifr;
    int fd, err;
    char *clonedev = "/dev/net/tun";

    if ((fd = open(clonedev, O_RDWR)) < 0) {
        return fd;
    }

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = flags;

    if ((err = ioctl(fd, TUNSETIFF, (void *) &ifr)) < 0) {
        close(fd);
        return err;
    }

    APP_DEBUG("Open tun/tap device: %s for reading...\n", ifr.ifr_name);

    return fd;
}

#if CHECK_SUM_ON
int if_api_calculate_checksum(void *buf, unsigned int len)
{
    unsigned char *p = (unsigned char *)buf;
    unsigned int sum = 0;
    unsigned int i;
    for(i=0;i<len;i++,p++)
        sum += *p;
#ifdef DEBUG_CHECKSUM
    printf("sum = %d \r\n", sum);
#endif
    sum = (sum % 65536) ^ 0xffff;
#ifdef DEBUG_CHECKSUM
    printf("rev.hex = 0x%04x \r\n", sum);
#endif
    *p = (sum & 0x7f00) >> 8;
    p++;
    *p = sum & 0x7f;
    return len + 2;
}

int if_api_check(void *buf, unsigned int len)
{
    unsigned char *p = (unsigned char *)buf;
    if(len < 2)
        return -1;
    uint8_t chechsum_low = p[len-1];
    uint8_t chechsum_high = p[len-2];
    p[len-1] = 0;
    p[len-2] = 0;
    if_api_calculate_checksum(p, len-2);

    if(chechsum_low != p[len-1] || chechsum_high != p[len-2])
        return -1;
    return len - 2;
}
#endif


