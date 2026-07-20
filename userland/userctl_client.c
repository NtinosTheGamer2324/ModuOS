// userctl_client.c
#include "libc.h"

typedef enum {
    CMD_GETUSERNAME,
    CMD_LOGIN,
    CMD_SETPASSWORD,
    CMD_CREATEUSER,
    CMD_DELETEUSER,
    CMD_WHOAMI,
} uctl_cmd_t;

typedef struct {
    uctl_cmd_t cmd;
    uint64_t intarg;
    uint32_t caller_pid;   /* filled with our own getpid() below */
    char arg1[32];
    char arg2[32];
    char arg3[32];
} uctl_packet_t;

typedef struct {
    char type1ret[32];
    uint64_t type2ret;
    uint64_t callerpid;
} uctl_ret_t;

static void print_usage(void) {
    puts("Usage: userctl <command> [args]\n");
    puts("  createuser   <username> <password>\n");
    puts("  login        <username> <password>\n");
    puts("  getusername  <uid>\n");
    puts("  setpassword  <username> <oldpassword> <newpassword>\n");
    puts("  deleteuser   <username>\n");
    puts("  whoami\n");
}

int md_main(long argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    int fd = open("$/user/userctl", O_RDWR, 0);
    if (fd < 0) {
        puts("Failed to open $/user/userctl (service not running?)\n");
        return 1;
    }

    uctl_packet_t req;
    uctl_ret_t resp;
    memset(&req, 0, sizeof(req));
    req.caller_pid = (uint32_t)getpid();

    const char *cmd = argv[1];

    if (strcmp(cmd, "createuser") == 0) {
        if (argc != 4) { puts("usage: userctl createuser <username> <password>\n"); close(fd); return 1; }
        req.cmd = CMD_CREATEUSER;
        strlcpy(req.arg1, argv[2], sizeof(req.arg1));
        strlcpy(req.arg2, argv[3], sizeof(req.arg2));

        if (invoke(fd, &req, sizeof(req), &resp, sizeof(resp)) >= 0) {
            printf("Created '%s', uid=%llu\n", argv[2], (unsigned long long)resp.type2ret);
        } else {
            printf("createuser failed (errno=%d)\n", errno);
        }

    } else if (strcmp(cmd, "login") == 0) {
        if (argc != 4) { puts("usage: userctl login <username> <password>\n"); close(fd); return 1; }
        req.cmd = CMD_LOGIN;
        strlcpy(req.arg1, argv[2], sizeof(req.arg1));
        strlcpy(req.arg2, argv[3], sizeof(req.arg2));

        if (invoke(fd, &req, sizeof(req), &resp, sizeof(resp)) >= 0) {
            printf("Login OK, uid=%llu groups=%s\n", (unsigned long long)resp.type2ret, resp.type1ret);
            printf("(this process's own uid should now be %llu -- check with 'userctl whoami')\n",
                   (unsigned long long)resp.type2ret);
        } else {
            printf("login failed (errno=%d)\n", errno);
        }

    } else if (strcmp(cmd, "getusername") == 0) {
        if (argc != 3) { puts("usage: userctl getusername <uid>\n"); close(fd); return 1; }
        req.cmd = CMD_GETUSERNAME;
        req.intarg = (uint64_t)atoi(argv[2]);

        if (invoke(fd, &req, sizeof(req), &resp, sizeof(resp)) >= 0) {
            printf("uid %s is '%s'\n", argv[2], resp.type1ret);
        } else {
            printf("getusername failed (errno=%d)\n", errno);
        }

    } else if (strcmp(cmd, "setpassword") == 0) {
        if (argc != 5) { puts("usage: userctl setpassword <username> <oldpassword> <newpassword>\n"); close(fd); return 1; }
        req.cmd = CMD_SETPASSWORD;
        strlcpy(req.arg1, argv[2], sizeof(req.arg1));
        strlcpy(req.arg2, argv[3], sizeof(req.arg2));
        strlcpy(req.arg3, argv[4], sizeof(req.arg3));

        if (invoke(fd, &req, sizeof(req), &resp, sizeof(resp)) >= 0) {
            puts("Password changed\n");
        } else {
            printf("setpassword failed (errno=%d)\n", errno);
        }

    } else if (strcmp(cmd, "deleteuser") == 0) {
        if (argc != 3) { puts("usage: userctl deleteuser <username>\n"); close(fd); return 1; }
        req.cmd = CMD_DELETEUSER;
        strlcpy(req.arg1, argv[2], sizeof(req.arg1));

        if (invoke(fd, &req, sizeof(req), &resp, sizeof(resp)) >= 0) {
            printf("'%s' deleted\n", argv[2]);
        } else {
            printf("deleteuser failed (errno=%d)\n", errno);
        }

    } else if (strcmp(cmd, "whoami") == 0) {
        req.cmd = CMD_WHOAMI;

        if (invoke(fd, &req, sizeof(req), &resp, sizeof(resp)) >= 0) {
            printf("%s\n", resp.type1ret);
        } else {
            printf("whoami failed (errno=%d)\n", errno);
        }

    } else {
        print_usage();
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}