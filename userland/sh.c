#include "libc.h"
#include "string.h"
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

        printf("\nsomeuser@pcnames> ");

        char* user_input = input();

        char command[64] = {0};
        char args[192] = {0};
        parse_command(user_input, command, args);

        if (strcmp(command, "help") == 0) {
            printf("aaaaa\n");
        } else if (strcmp(command, "exit") == 0) {
            sh_running = 0;
        } else if (strlen(command) == 0) {
            // Do nothing on empty command
        } else {
            printf(
                "\\cr%s : The term '%s' is not recognized as the name of a klet, or operable program.\n"
                "Check the spelling of the name and try again.\n"
                "+ %s %s \n \\rr\\rr",
                command, command, command, args
            );
        }

        // Here you would read input and execute commands...
    }

    return 0;
}
