#include "libc.h"
#include "string.h"

#define SYS_MOUNT       65
#define SYS_UNMOUNT     66
#define SYS_MOUNTS      67

int sh_running = 1;

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

int md_main(long argc, char** argv) {
    while (sh_running) {
        yield();
        printf("\nsomeuser@pcnames> ");

        char* user_input = input();

        char command[64] = {0};
        char args[192] = {0};
        parse_command(user_input, command, args);

        if (strcmp(command, "help") == 0) {
            printf("Available commands:\n");
            printf("  help        - Show this help\n");
            printf("  exit        - Exit shell\n");
            printf("  whoami      - Show current user\n");
            printf("  ls          - List files (run existing ls program)\n");
            printf("  cat <file>  - Display file contents (run existing cat program)\n");
            printf("  mounts      - List mounted filesystems\n");
            printf("  mount <vdrive> <lba> <type> - Mount filesystem\n");
            printf("  unmount <slot> - Unmount filesystem\n");
        } else if (strcmp(command, "exit") == 0) {
            sh_running = 0;
        } else if (strcmp(command, "whoami") == 0) {
            int uid = getuid();
            printf("\nYou are UID: %d\n", uid);
        } else if (strcmp(command, "mounts") == 0) {
            char buf[2048];
            int count = syscall(SYS_MOUNTS, (long)buf, sizeof(buf), 0);
            if (count < 0) {
                printf("Error listing mounts\n");
            } else if (count == 0) {
                printf("No filesystems mounted\n");
            } else {
                printf("Mounted filesystems:\n%s", buf);
            }
        } else if (strcmp(command, "mount") == 0) {
            int vdrive, lba, type;
            if (sscanf(args, "%d %d %d", &vdrive, &lba, &type) != 3) {
                printf("Usage: mount <vdrive> <lba> <type>\n");
                printf("Example: mount 1 2048 2  (mount vdrive 1, LBA 2048, type FAT32)\n");
            } else {
                int slot = syscall(SYS_MOUNT, vdrive, lba, type);
                if (slot < 0) {
                    printf("Mount failed with error: %d\n", slot);
                } else {
                    printf("Mounted successfully at slot %d\n", slot);
                }
            }
        } else if (strcmp(command, "unmount") == 0) {
            int slot;
            if (sscanf(args, "%d", &slot) != 1) {
                printf("Usage: unmount <slot>\n");
            } else {
                int rc = syscall(SYS_UNMOUNT, slot, 0, 0);
                if (rc == 0) {
                    printf("Unmounted slot %d\n", slot);
                } else {
                    printf("Unmount failed with error: %d\n", rc);
                }
            }
        } else if (strlen(command) == 0) {
            /* empty input */
        } else {
            /* Build the full path to the executable. Search order:
             *   1. Absolute path (starts with '/' or '$/')
             *   2. /Apps/<command>.sqr
             *   3. /ModuOS/System64/<command>.sqr
             */
            char path[256] = {0};

            if (command[0] == '/' || (command[0] == '$' && command[1] == '/')) {
                strncpy(path, command, sizeof(path) - 1);
            } else {
                /* Try /Apps first */
                snprintf(path, sizeof(path), "/Apps/%s.sqr", command);
                int probe = open(path, 0, 0);
                if (probe < 0) {
                    /* Try system directory */
                    snprintf(path, sizeof(path), "/ModuOS/System64/%s.sqr", command);
                    probe = open(path, 0, 0);
                    if (probe < 0) {
                        printf("%s: command not found\n", command);
                        path[0] = '\0';
                    }
                }
                if (probe >= 0) close(probe);
            }

            if (path[0]) {
                /* Build argv for the child: argv[0]=path, then split args */
                char *child_argv[64];
                int child_argc = 0;
                child_argv[child_argc++] = path;

                /* Tokenize args in-place */
                static char args_copy[192];
                strncpy(args_copy, args, sizeof(args_copy) - 1);
                char *tok = args_copy;
                while (*tok && child_argc < 63) {
                    while (*tok == ' ' || *tok == '\t') tok++;
                    if (!*tok) break;
                    child_argv[child_argc++] = tok;
                    while (*tok && *tok != ' ' && *tok != '\t') tok++;
                    if (*tok) { *tok = '\0'; tok++; }
                }
                child_argv[child_argc] = NULL;

                int pid = fork();
                if (pid == 0) {
                    /* child */
                    execve(path, child_argv, NULL);
                    printf("%s: exec failed\n", path);
                    exit(1);
                } else if (pid > 0) {
                    /* parent waits */
                    int status = 0;
                    waitpid(pid, &status, 0);
                } else {
                    printf("fork failed\n");
                }
            }
        }

        // Here you would read input and execute commands...
    }

    return 0;
}
