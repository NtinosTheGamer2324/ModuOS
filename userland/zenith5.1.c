#include "libc.h"
#include <stdbool.h>
#include <stdint.h>
#include "string.h"

#define ANSI_RED     "\033[31m\b"
#define ANSI_PURPLE  "\033[35m\b"
#define ANSI_GREEN   "\033[32m\b"
#define ANSI_CYAN    "\033[36m\b"
#define ANSI_ORANGE  "\033[38;5;214m\b"
#define ANSI_RESET   "\033[0m\b"

int g_running = 1;

void parse_command(char* input, char* command, char* args) {
    while (*input == ' ' || *input == '\t') input++;
    while (*input && *input != ' ' && *input != '\t') {
        *command++ = *input++;
    }
    *command = '\0';
    while (*input == ' ' || *input == '\t') input++;
    while (*input) {
        *args++ = *input++;
    }
    *args = '\0';
}

void zsbanner(void) {
    printf(ANSI_PURPLE "$$$$$$$$\\                    $$\\   $$\\     $$\\       \n");
    printf(ANSI_PURPLE "\\____$$  |                   \\__|  $$ |    $$ |      \n");
    printf(ANSI_PURPLE "    $$  / $$$$$$\\  $$$$$$$\\  $$\\ $$$$$$\\   $$$$$$$\\  \n");
    printf(ANSI_PURPLE "   $$  / $$  __$$\\ $$  __$$\\ $$ |\\_$$  _|  $$  __$$\\ \n");
    printf(ANSI_PURPLE "  $$  /  $$$$$$$$ |$$ |  $$ |$$ |  $$ |    $$ |  $$ |\n");
    printf(ANSI_PURPLE " $$  /   $$   ____|$$ |  $$ |$$ |  $$ |$$\\ $$ |  $$ |\n");
    printf(ANSI_PURPLE "$$$$$$$$\\ $$$$$$$\\ $$ |  $$ |$$ |  \\$$$$  |$$ |  $$ |\n");
    printf(ANSI_PURPLE "\\________|\\_______|\\__|  \\__|\\__|   \\____/ \\__|  \\__| ");
    printf(ANSI_CYAN "v0.5.1\n" ANSI_RESET);
}

const char* get_pc_name() {
    const char* path = "/ModuOS/System64/pcname.txt";
    fs_file_info_t file_info;

    if (stat(path, &file_info) < 0) {
        return NULL;
    }

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        return NULL;
    }

    char* buffer = (char*)malloc(file_info.size + 1);
    if (!buffer) {
        close(fd);
        return NULL;
    }

    ssize_t bytes_read = read(fd, buffer, file_info.size);
    if (bytes_read < 0) {
        free(buffer);
        close(fd);
        return NULL;
    }

    buffer[bytes_read] = '\0';
    close(fd);

    return (const char*)buffer;
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    zsbanner();

    printf(ANSI_CYAN "New Technologies Software (c) 2026\n" ANSI_RESET);
    printf(ANSI_ORANGE "Type 'help' for available builtin commands.\n" ANSI_RESET);
    printf(ANSI_ORANGE "Run ls /Apps/ to see installed applications.\n" ANSI_RESET);

    static char cwd[256];
    static char prev_cwd[256] = "/";
    static char command[64];
    static char args[192];
    static char app_path[256];
    static char args_copy[192];

    const char* host = get_pc_name();

    while (g_running) {
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            strcpy(cwd, "?");
        }

        int who = (int)syscall(SYS_GETUID, 0, 0, 0);
        if (who < 0) who = 1;

        if (host) {
            if (who == 0) {
                printf("\n┌─%s# %s%d@%s\n└─[%s]>", ANSI_RED, ANSI_RESET, who, host, cwd);
            } else {
                printf("\n┌─%s# %s%d@%s\n└─[%s]>", ANSI_GREEN, ANSI_RESET, who, host, cwd);
            }
        } else {
            if (who == 0) {
                printf("\n┌─%s# %s%d@{unknown}\n└─[%s]>", ANSI_RED, ANSI_RESET, who, cwd);
            } else {
                printf("\n┌─%s# %s%d@{unknown}\n└─[%s]>", ANSI_GREEN, ANSI_RESET, who, cwd);
            }
        }

        char *user_input = input();
        memset(command, 0, sizeof(command));
        memset(args,    0, sizeof(args));
        parse_command(user_input, command, args);

        if (strcmp(command, "help") == 0) {
            printf("Available commands:\n");
            printf("Command       Description\n");
            printf("--------------------------\n");
            printf(" help     |    Show this help\n");
            printf(" cd <dir> |    Change directory\n");
            printf(" pwd      |    Show current directory\n");
            printf(" exit     |    Exit shell\n");
            printf("--------------------------\n");
        } else if (strcmp(command, "exit") == 0) {
            g_running = 0;

        } else if (strcmp(command, "cd") == 0) {
            // Strip leading whitespace
            char *p = args;
            while (*p == ' ' || *p == '\t') p++;
        
            // Strip trailing whitespace and control chars
            int len = strlen(p);
            while (len > 0 && (unsigned char)p[len - 1] <= ' ') {
                p[--len] = '\0';
            }
        
            // Strip trailing slash unless it's just "/"
            while (len > 1 && p[len - 1] == '/') {
                p[--len] = '\0';
            }
        
            const char *target = (len == 0) ? "/" : p;
        
            char saved[256];
            if (getcwd(saved, sizeof(saved)) == NULL) {
                strcpy(saved, "/");
            }
        
            int ret = chdir(target);
            if (ret != 0) {
                printf("%scd: %s: error %d%s\n", ANSI_RED, target, -ret, ANSI_RESET);
            } else {
                strcpy(prev_cwd, saved);
            }
        } else if (strcmp(command, "pwd") == 0) {
            printf("%s\n", cwd);

        } else if (strlen(command) > 0) {
            snprintf(app_path, sizeof(app_path), "/Apps/%s.sqr", command);

            int fdex = open(app_path, O_RDONLY, 0);
            if (fdex >= 0) {
                close(fdex);
                printf("\n");

                int pid = fork();
                if (pid == 0) {
                    char *argv_exec[16];
                    int argc_exec = 0;
                    argv_exec[argc_exec++] = app_path;

                    strncpy(args_copy, args, sizeof(args_copy) - 1);
                    args_copy[sizeof(args_copy) - 1] = 0;

                    char *tok = strtok(args_copy, " \t");
                    while (tok && argc_exec < 15) {
                        argv_exec[argc_exec++] = tok;
                        tok = strtok(NULL, " \t");
                    }
                    argv_exec[argc_exec] = NULL;

                    char *envp[] = { NULL };
                    execve(app_path, argv_exec, envp);
                    printf("%sFailed to execute %s%s\n", ANSI_RED, app_path, ANSI_RESET);
                    exit(1);
                } else if (pid > 0) {
                    int status = 0;
                    waitpid(pid, &status, 0);
                } else {
                    printf("%sError: fork failed%s\n", ANSI_RED, ANSI_RESET);
                }
            } else {
                printf("%s%s : The term '%s' is not recognized as a klet or program.\n"
                       "Check spelling and try again.\n+ %s %s\n%s",
                       ANSI_RED, command, command, command, args, ANSI_RESET);
            }
        }
    }

    printf("Goodbye!\n");
    return 0;
}