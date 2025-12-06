#ifndef KERNEL_H
#define KERNEL_H

#include "moduos/fs/fs.h"

extern int acpi_initialized;
extern int boot_drive_index;

int kernel_get_boot_drive(void);
int kernel_get_boot_slot(void);
fs_mount_t* kernel_get_boot_mount(void);

#endif