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
    printf(ANSI_PURPLE "$$$$$$$$\\  $$$$$$$\\ $$ |  $$ |$$ |  \\$$$$  |$$ |  $$ |\n");
    printf(ANSI_PURPLE "\\________|\\_______|\\__|  \\__|\\__|   \\____/ \\__|  \\__| ");
    printf(ANSI_CYAN "v0.5.1\n" ANSI_RESET);
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    zsbanner();

    printf(ANSI_CYAN "New Technologies Software (c) 2026\n" ANSI_RESET);
    printf(ANSI_ORANGE "Type 'help' for available builtin commands.\n" ANSI_RESET);
    printf(ANSI_ORANGE "Run ls /Apps/ to see installed applications.\n" ANSI_RESET);

    /* Keep large buffers as statics to avoid stack growth on each iteration. */
    static char cwd[256];
    static char command[64];
    static char args[192];
    static char app_path[256];
    static char args_copy[192];

    while (g_running) {
        /* CWD syscall not yet implemented; show root. */
        cwd[0] = '/'; cwd[1] = '\0';

        /* Use the md64api identity syscall to get UID; fall back to 1. */
        int who = (int)syscall(SYS_GETUID, 0, 0, 0);
        if (who < 0) who = 1;

        if (who == 0) {
            printf("\n%s# %s%d@{pcname} %s> ",
                   ANSI_RED, ANSI_RESET, who, cwd);
        } else {
            printf("\n%s$ %s%d@{pcname} %s> ",
                   ANSI_GREEN, ANSI_RESET, who, cwd);
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
            printf(" exit     |    Exit shell\n");
            printf("--------------------------\n");
        } else if (strcmp(command, "exit") == 0) {
            g_running = 0;
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