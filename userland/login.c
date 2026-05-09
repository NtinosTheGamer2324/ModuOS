#include "libc.h"
#include "userman.h"
#include "string.h"

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;
    printf("ModuOS Login\n");
    printf("legal: THIS SOFTWARE IS PROVIDED AS-IS WITHOUT ANY WARRANTIES. USE AT YOUR OWN RISK.\n\n");

    char username[64];
    char password[64];

retry:
    input_flush();

    printf("Username: ");
    {
        const char *in = input();
        strncpy(username, in ? in : "", sizeof(username) - 1);
        username[sizeof(username) - 1] = 0;
    }
    input_flush();

    printf("\nPassword: ");
    {
        const char *in = input();
        strncpy(password, in ? in : "", sizeof(password) - 1);
        password[sizeof(password) - 1] = 0;
    }
    input_flush();

    /* Authenticate via userman */
    int authfd = open("$/user/users/auth", O_RDWR, 0);
    if (authfd < 0) {
        printf("login: userman not available\n");
        goto retry;
    }

    char req[128];
    safe_strcpy(req, sizeof(req), username);
    safe_strcat(req, sizeof(req), ":");
    safe_strcat(req, sizeof(req), password);
    write(authfd, req, strlen(req));

    char resp[32];
    int rr = read(authfd, resp, sizeof(resp) - 1);
    close(authfd);

    int target_uid = -1;
    if (rr > 0) {
        resp[rr] = 0;
        target_uid = atoi(resp);
    }

    if (target_uid < 0) {
        printf("login failed\n");
        goto retry;
    }

    if (setuid(target_uid) != 0) {
        printf("login: setuid failed (need to be mdman/root to switch)\n");
        return 3;
    }

    printf("\nlogin ok\n");

    {
        int pid = fork();
        char *shell_argv[] = { "/Apps/zenith5.1.sqr", NULL };
        char *envp[] = { NULL };
        execve("/Apps/zenith5.1.sqr", shell_argv, envp);
        int status = 0;
        waitpid(pid, &status, 0);
    }

    goto retry;
    return 0;
}