#include "moduos/kernel/shell/legacy/MSDOS.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/drivers/ps2/ps2.h"
#include "moduos/kernel/shell/helpers.h"
#include "moduos/kernel/shell/art.h"

shell_dos dos_state = {
    .running = 1,
    .user = "system",
    .pcname = "dos_emulate"
};

void parse_command3(char* input, char* command, char* args) {
    /* Skip leading whitespace */
    while (*input == ' ' || *input == '\t') input++;

    /* Extract command */
    while (*input && *input != ' ' && *input != '\t') {
        *command++ = *input++;
    }
    *command = '\0';

    /* Skip whitespace before args */
    while (*input == ' ' || *input == '\t') input++;
    
    /* Copy remaining args */
    while (*input) {
        *args++ = *input++;
    }
    *args = '\0';
}

void msdos_start(){
    dos_state.running = 1;
    VGA_Clear();
    VGA_Write("\\cwModuOS ClassicDOS [Version 0.3.2610.71]\n");
    VGA_Write("(c) New Technologies Software 1998-2016. All Rights Reserved\n");
    while (dos_state.running == 1) {
        VGA_Writef("\\cw?:\\>\\cw");

        char* user_input = input();

        char command[64] = {0};
        char args[192] = {0};
        parse_command3(user_input, command, args);
        if (strcmp(command, "exit") == 0) {
            dos_state.running = 0;
        } else if (strcmp(command, "clear") == 0 || strcmp(command, "cls") == 0) {
            VGA_Clear();
        } else if (strcmp(command, "banner") == 0) {
            dosbanner();
        } else if (strlen(command) == 0) {
            
        } else{
            VGA_Writef("Illegal command: %s\n", command);
        }

    };
}
                          
 