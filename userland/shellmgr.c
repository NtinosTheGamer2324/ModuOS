#include "libc.h"

#define AUTOSTART "/ModuOS/shared/config/.autoshell.cfg"

#define ANSI_RED     "\033[31m\b"
#define ANSI_PURPLE  "\033[35m\b"
#define ANSI_GREEN   "\033[32m\b"
#define ANSI_CYAN    "\033[36m\b"
#define ANSI_ORANGE  "\033[38;5;214m\b"
#define ANSI_RESET   "\033[0m\b"

void execapp(const char *path, const char *args) {
    int fdex = open(path, O_RDONLY, 0);
    if (fdex >= 0) {
        close(fdex);
        printf("\n");

        int pid = fork();
        if (pid == 0) {
            char *argv_exec[16];
            int argc_exec = 0;
            argv_exec[argc_exec++] = (char *)path;
            
            char args_copy[256];
            strncpy(args_copy, args, sizeof(args_copy) - 1);
            args_copy[sizeof(args_copy) - 1] = 0;
            
            char *tok = strtok(args_copy, " \t");
            while (tok && argc_exec < 15) {
                argv_exec[argc_exec++] = tok;
                tok = strtok(NULL, " \t");
            }
            argv_exec[argc_exec] = NULL;
            
            char *envp[] = { NULL };
            execve(path, argv_exec, envp);
            printf("%sFailed to execute %s%s\n", ANSI_RED, path, ANSI_RESET);
            exit(1);
        } else {
            printf("%sError: fork failed%s\n", ANSI_RED, ANSI_RESET);
        }
    } else {
        printf("ERR: App does not exist. (%s)\n", path);
    }
}

int md_main(long argc, char **argv) {
    int fd = open(AUTOSTART, O_RDONLY, 0);
    if (fd < 0) return -1;

    char scriptfile[1024];
    int bytes_read = read(fd, &scriptfile, 1024);
    if (bytes_read <= 0) {
        close(fd);
        return 0;
    }
    scriptfile[bytes_read] = '\0';

    char *ptr = scriptfile;
    while ((ptr = strstr(ptr, "exec=")) != NULL) {
        ptr += 5;

        char *path_start = strchr(ptr, '"');
        if (!path_start) break;
        path_start++;

        char *path_end = strchr(path_start, '"');
        if (!path_end) break;

        char *args_start = strchr(path_end + 1, '"');
        if (!args_start) break;
        args_start++;

        char *args_end = strchr(args_start, '"');
        if (!args_end) break;

        char path[128];
        char args[128];

        int path_len = path_end - path_start;
        if (path_len >= sizeof(path)) path_len = sizeof(path) - 1;
        memcpy(path, path_start, path_len);
        path[path_len] = '\0';

        int args_len = args_end - args_start;
        if (args_len >= sizeof(args)) args_len = sizeof(args) - 1;
        memcpy(args, args_start, args_len);
        args[args_len] = '\0';

        printf("Executing -> Path: [%s], Args: [%s]\n", path, args);
        execapp(path, args);

        ptr = args_end + 1;
    }

    close(fd);
    return 0;
}