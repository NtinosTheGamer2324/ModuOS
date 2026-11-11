#include "libc/libc.h"
#include "libc/string.h"
#include "userctl_ipc.h"

#define LOGIN_SHELL "/Apps/zenith5.1.sqr"
/*
 * The session runs in a forked child so this process keeps euid 0 and can
 * authenticate the next user after logout. userctl only reports the ids;
 * the child applies them itself, gid first because setgid needs the
 * privilege that setuid gives up.
 */
static void login_run_session(const char *username, int uid, int gid) {
    int pid = fork();
    if (pid < 0) {
        printf("login: fork failed\n");
        return;
    }
    if (pid == 0) {
        if (setgid(gid) != 0 || setuid(uid) != 0) {
            printf("login: cannot drop privileges\n");
            _exit(126);
        }

        char env_user[48], env_logname[52];
        env_user[0] = 0;
        env_logname[0] = 0;
        safe_strcpy(env_user, sizeof(env_user), "USER=");
        safe_strcat(env_user, sizeof(env_user), username);
        safe_strcpy(env_logname, sizeof(env_logname), "LOGNAME=");
        safe_strcat(env_logname, sizeof(env_logname), username);

        char *shell_argv[] = { LOGIN_SHELL, NULL };
        char *envp[] = { env_user, env_logname, NULL };
        execve(LOGIN_SHELL, shell_argv, envp);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;
    printf("ModuOS Login\n");
    printf("legal: THIS SOFTWARE IS PROVIDED AS-IS WITHOUT ANY WARRANTIES. USE AT YOUR OWN RISK.\n\n");

    uctl_request_t req;
    uctl_response_t resp;
    uctl_shm_t *shm = NULL;

retry:
    memset(&req, 0, sizeof(req));
    req.cmd = UCTL_CMD_LOGIN;
    input_flush();

    printf("\033[33m Username: \033[36m");
    {
        const char *in = input();
        strlcpy(req.arg_user, in ? in : "", sizeof(req.arg_user));
    }
    input_flush();
    printf("\033[32m");
    printf("\n\033[33m Password: \033[36m");
    {
        const char *in = input();
        strlcpy(req.arg_pass, in ? in : "", sizeof(req.arg_pass));
    }
    input_flush();
    printf("\033[32m");
    /* Each attach consumes a segment handle, so map the mailbox only once. */
    if (!shm) shm = uctl_attach();
    if (!shm) {
        printf("\nlogin: \033[31m userctl not available \033[32m\n");
        uctl_scrub(&req, sizeof(req));
        sleep(1);
        goto retry;
    }

    int rc = uctl_call(shm, &req, &resp);
    char username[UCTL_NAME_MAX];
    strlcpy(username, req.arg_user, sizeof(username));
    uctl_scrub(&req, sizeof(req));

    if (rc != 0) {
        printf("\nlogin \033[31m failed \033[0m\n");
        goto retry;
    }

    printf("\nlogin \033[32m ok \033[0m\n");
    login_run_session(username, (int)resp.uid, (int)resp.gid);
    goto retry;
    return 0;
}
