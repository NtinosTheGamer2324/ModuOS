// userctl_client.c - command line front end for the userctl service
#include "libc/libc.h"
#include "libc/string.h"
#include "userctl_ipc.h"

static void print_usage(void) {
    puts("Usage: userctl <command> [args]\n");
    puts("  createuser   <username> <password> [<admin> <adminpassword>]\n");
    puts("  deleteuser   <username> <admin> <adminpassword>\n");
    puts("  setpassword  <username> <oldpassword> <newpassword>\n");
    puts("  resetpasswd  <username> <newpassword> <admin> <adminpassword>\n");
    puts("  login        <username> <password>     (check credentials only)\n");
    puts("  getusername  <uid>\n");
    puts("  whoami\n");
    puts("The admin account must have uid 0. It may be omitted for createuser\n");
    puts("only while no accounts exist yet.\n");
}

static int fail(const char *what, int rc) {
    if (rc == -1) printf("%s failed: userctl did not answer\n", what);
    else          printf("%s failed (errno=%d)\n", what, -rc);
    return 1;
}

int md_main(long argc, char **argv) {
    if (argc < 2) { print_usage(); return 1; }

    uctl_shm_t *shm = uctl_attach();
    if (!shm) {
        puts("userctl: service not running\n");
        return 1;
    }

    uctl_request_t req;
    uctl_response_t resp;
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));

    const char *cmd = argv[1];
    int rc = 0;

    if (strcmp(cmd, "createuser") == 0) {
        if (argc != 4 && argc != 6) { print_usage(); return 1; }
        req.cmd = UCTL_CMD_CREATEUSER;
        strlcpy(req.arg_user, argv[2], sizeof(req.arg_user));
        strlcpy(req.arg_pass, argv[3], sizeof(req.arg_pass));
        if (argc == 6) {
            strlcpy(req.admin_user, argv[4], sizeof(req.admin_user));
            strlcpy(req.admin_pass, argv[5], sizeof(req.admin_pass));
        }
        rc = uctl_call(shm, &req, &resp);
        if (rc != 0) { uctl_scrub(&req, sizeof(req)); return fail("createuser", rc); }
        printf("Created '%s', uid=%llu\n", argv[2], (unsigned long long)resp.uid);

    } else if (strcmp(cmd, "deleteuser") == 0) {
        if (argc != 5) { print_usage(); return 1; }
        req.cmd = UCTL_CMD_DELETEUSER;
        strlcpy(req.arg_user, argv[2], sizeof(req.arg_user));
        strlcpy(req.admin_user, argv[3], sizeof(req.admin_user));
        strlcpy(req.admin_pass, argv[4], sizeof(req.admin_pass));
        rc = uctl_call(shm, &req, &resp);
        if (rc != 0) { uctl_scrub(&req, sizeof(req)); return fail("deleteuser", rc); }
        printf("'%s' deleted\n", argv[2]);

    } else if (strcmp(cmd, "setpassword") == 0) {
        if (argc != 5) { print_usage(); return 1; }
        req.cmd = UCTL_CMD_SETPASSWORD;
        strlcpy(req.arg_user, argv[2], sizeof(req.arg_user));
        strlcpy(req.arg_pass, argv[3], sizeof(req.arg_pass));
        strlcpy(req.arg_new, argv[4], sizeof(req.arg_new));
        rc = uctl_call(shm, &req, &resp);
        if (rc != 0) { uctl_scrub(&req, sizeof(req)); return fail("setpassword", rc); }
        puts("Password changed\n");

    } else if (strcmp(cmd, "resetpasswd") == 0) {
        if (argc != 6) { print_usage(); return 1; }
        req.cmd = UCTL_CMD_SETPASSWORD;
        strlcpy(req.arg_user, argv[2], sizeof(req.arg_user));
        strlcpy(req.arg_new, argv[3], sizeof(req.arg_new));
        strlcpy(req.admin_user, argv[4], sizeof(req.admin_user));
        strlcpy(req.admin_pass, argv[5], sizeof(req.admin_pass));
        rc = uctl_call(shm, &req, &resp);
        if (rc != 0) { uctl_scrub(&req, sizeof(req)); return fail("resetpasswd", rc); }
        puts("Password changed\n");

    } else if (strcmp(cmd, "login") == 0) {
        if (argc != 4) { print_usage(); return 1; }
        req.cmd = UCTL_CMD_LOGIN;
        strlcpy(req.arg_user, argv[2], sizeof(req.arg_user));
        strlcpy(req.arg_pass, argv[3], sizeof(req.arg_pass));
        rc = uctl_call(shm, &req, &resp);
        if (rc != 0) { uctl_scrub(&req, sizeof(req)); return fail("login", rc); }
        printf("Credentials OK: uid=%llu gid=%llu groups=%s\n",
               (unsigned long long)resp.uid, (unsigned long long)resp.gid, resp.groups);

    } else if (strcmp(cmd, "getusername") == 0) {
        if (argc != 3) { print_usage(); return 1; }
        req.cmd = UCTL_CMD_GETUSERNAME;
        req.intarg = (uint64_t)atoi(argv[2]);
        rc = uctl_call(shm, &req, &resp);
        if (rc != 0) return fail("getusername", rc);
        printf("uid %s is '%s'\n", argv[2], resp.name);

    } else if (strcmp(cmd, "whoami") == 0) {
        int euid = geteuid();
        if (uctl_name_of_uid(shm, (uint64_t)euid, resp.name, sizeof(resp.name)) == 0)
            printf("%s (uid=%d)\n", resp.name, euid);
        else
            printf("uid=%d\n", euid);

    } else {
        print_usage();
        return 1;
    }

    uctl_scrub(&req, sizeof(req));
    return 0;
}
