#ifndef ATAPI_H
#define ATAPI_H

#include <stdint.h>


int atapi_read_blocks_pio(int drive_index, uint32_t lba, uint32_t count, void* out);
int atapi_read_sector(int drive_index, uint32_t lba, void* out);
int atapi_has_media(int drive_index);

#endif /* ATAPI_H */
