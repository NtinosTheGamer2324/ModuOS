#pragma once

typedef struct
{
    int running;
    const char *user;
    const char *pcname;
} shell_dos;



void msdos_start(void);