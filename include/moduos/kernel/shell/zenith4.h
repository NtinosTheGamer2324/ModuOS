#ifndef ZENITH4_SHELL_H
#define ZENITH4_SHELL_H

#include <stddef.h>
#include <stdint.h>

/* History configuration */
#define HISTORY_SIZE 50
#define COMMAND_MAX_LEN 256

/* Public shell type and functions used across translation units */
typedef struct {
    int running;
    int fat32_mounted;
    int iso9660_mounted;
    char last_command[64];
    int command_count;
    int show_timestamps;
    char path[256];
    char user[32];
    char pcname[32];
    char history[HISTORY_SIZE][COMMAND_MAX_LEN];
    int history_count;
    int history_index;
    int browsing_history;
    int current_slot;        // Currently active filesystem slot (-1 = none)
    char cwd[256];           // Current working directory path
    int boot_slot;           // NEW: Slot where boot drive is mounted (-1 = none)
} shell_state_t;

/* Global shell state */
extern shell_state_t shell_state;

/* Shell control functions */
void zenith4_start(void);
void panicer_close_shell4(void);
void up_arrow_pressed(void);
void down_arrow_pressed(void);

/* History management */
void add_to_history(const char* command);
const char* get_history_prev(void);
const char* get_history_next(void);
void reset_history_browsing(void);

#endif /* ZENITH4_SHELL_H */