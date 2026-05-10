// calc_client.c
#include "libc.h"

typedef struct {
    int cmd;
    int a;
    int b;
} calc_request_t;

typedef struct {
    int success;
    int result;
    char message[64];
} calc_response_t;

int md_main(long argc, char **argv)
{
    int fd = open("$/user/calc", O_RDWR, 0);
    if (fd < 0) {
        puts("Failed to open $/user/calc (service not running?)\n");
        return 1;
    }

    puts("Connected to Calc Service!\n");

    calc_request_t req;
    calc_response_t resp;

    req.cmd = 1; req.a = 25; req.b = 17;
    if (invoke(fd, &req, sizeof(req), &resp, sizeof(resp)) > 0) {
        printf("25 + 17 = %d\n", resp.result);
    }

    req.cmd = 2; req.a = 6; req.b = 7;
    if (invoke(fd, &req, sizeof(req), &resp, sizeof(resp)) > 0) {
        printf("6 * 7 = %d\n", resp.result);
    }

    close(fd);
    return 0;
}