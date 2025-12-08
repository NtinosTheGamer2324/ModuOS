#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include <stdint.h>
#include <stddef.h>

#define MAX_REGIONS 64
#define MAX_TOTAL_SUPPORTED_BYTES (8ULL * 1024 * 1024 * 1024)  // 8 GiB

static uint64_t regions[MAX_REGIONS * 2];
static size_t region_count = 0;

static uint8_t *bitmap = NULL;
static uint64_t frame_count = 0;
static size_t bitmap_size = 0;

/* Which region index the bitmap+frame space begins at */
static size_t alloc_start_region = 0;

/* Bitmap operations */
static inline void bm_set(uint64_t i)   { bitmap[i >> 3] |=  (1u << (i & 7)); }
static inline void bm_clear(uint64_t i) { bitmap[i >> 3] &= ~(1u << (i & 7)); }
static inline int  bm_test(uint64_t i)  { return (bitmap[i >> 3] >> (i & 7)) & 1u; }

/* Helper functions for logging */
static char *append(char *d, const char *s) {
    while (*s) *d++ = *s++;
    return d;
}

static char *u_dec(char *d, uint64_t v) {
    char t[32];
    int n = 0;
    do {
        t[n++] = '0' + (v % 10);
        v /= 10;
    } while (v && n < 31);
    while (n--) *d++ = t[n];
    *d = 0;
    return d;
}

static char *u_hex(char *d, uint64_t v) {
    char t[32];
    int n = 0;
    do {
        int dgt = v & 0xF;
        t[n++] = (dgt < 10 ? '0'+dgt : 'a'+(dgt-10));
        v >>= 4;
    } while (v && n < 31);
    while (n--) *d++ = t[n];
    *d = 0;
    return d;
}

static void log_phys(uint64_t count, uint64_t bm_addr, uint64_t bsz) {
    char buf[128];
    char *p = buf;
    p = append(p, "[PHYS] frames=");
    p = u_dec(p, count);
    p = append(p, " bitmap@0x");
    p = u_hex(p, bm_addr);
    p = append(p, " size=");
    p = u_dec(p, bsz);
    p = append(p, " bytes\n");
    *p = 0;
    com_write_string(COM1_PORT, buf);
}

/**********************************************************************
 * ADDRESS TRANSLATION: index ↔ physical
 **********************************************************************/

static uint64_t phys_from_idx(uint64_t idx) {
    uint64_t offset = idx * PAGE_SIZE;

    for (size_t r = alloc_start_region; r < region_count; r++) {
        uint64_t base = regions[r*2+0];
        uint64_t len  = regions[r*2+1];

        if (offset < len)
            return base + offset;

        offset -= len;
    }
    return 0;
}

static uint64_t idx_from_phys(uint64_t addr) {
    uint64_t skip = 0;

    for (size_t r = alloc_start_region; r < region_count; r++) {
        uint64_t base = regions[r*2+0];
        uint64_t len  = regions[r*2+1];

        if (addr >= base && addr < base + len) {
            uint64_t off = addr - base;
            return skip + (off / PAGE_SIZE);
        }
        skip += len / PAGE_SIZE;
    }
    return UINT64_MAX;
}

/**********************************************************************
 * INITIALIZATION
 **********************************************************************/

void phys_init(uint64_t total_mem, const void *usable, size_t count) {
    if (total_mem > MAX_TOTAL_SUPPORTED_BYTES)
        total_mem = MAX_TOTAL_SUPPORTED_BYTES;

    region_count = (count > MAX_REGIONS ? MAX_REGIONS : count);
    const uint64_t *src = usable;

    for (size_t i = 0; i < region_count; i++) {
        regions[i*2+0] = src[i*2+0];
        regions[i*2+1] = src[i*2+1];
    }

    /* Find the best region to place bitmap - prefer region starting at 1MB */
    alloc_start_region = 0;
    uint64_t bitmap_location = 0;
    
    for (size_t i = 0; i < region_count; i++) {
        uint64_t base = regions[i*2+0];
        uint64_t len = regions[i*2+1];
        
        // Skip the low memory region (below 1MB) if possible
        if (base >= 0x100000 && len >= PAGE_SIZE * 2) {
            alloc_start_region = i;
            bitmap_location = base;
            break;
        }
    }
    
    // If no suitable region found above 1MB, use first region but skip low addresses
    if (bitmap_location == 0) {
        alloc_start_region = 0;
        // Place bitmap at 64KB to avoid BIOS data area (0-1KB) and other low memory structures
        bitmap_location = 0x10000; // 64 KB
        
        // Make sure this is within the first region
        if (bitmap_location >= regions[0] + regions[1]) {
            bitmap_location = regions[0] + PAGE_SIZE; // Fallback: 1 page in
        }
    }

    /* Calculate total frames from regions starting at alloc_start_region */
    uint64_t total_frames = 0;
    for (size_t r = alloc_start_region; r < region_count; r++) {
        total_frames += regions[r*2+1] / PAGE_SIZE;
    }

    frame_count = total_frames;
    bitmap_size = (frame_count + 7) / 8;
    
    /* Calculate how many frames the bitmap itself occupies */
    uint64_t bitmap_frames = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;

    com_write_string(COM1_PORT, "[PHYS] Placing bitmap at physical address 0x");
    char hexbuf[20];
    char *p = hexbuf;
    uint64_t v = bitmap_location;
    do {
        int dgt = v & 0xF;
        *p++ = (dgt < 10 ? '0'+dgt : 'a'+(dgt-10));
        v >>= 4;
    } while (v);
    // Reverse the string
    for (int i = 0; i < (p - hexbuf) / 2; i++) {
        char tmp = hexbuf[i];
        hexbuf[i] = hexbuf[p - hexbuf - 1 - i];
        hexbuf[p - hexbuf - 1 - i] = tmp;
    }
    *p = 0;
    com_write_string(COM1_PORT, hexbuf);
    com_write_string(COM1_PORT, "\n");

    /* Set bitmap pointer - IDENTITY MAPPED so physical == virtual */
    bitmap = (uint8_t *)(uintptr_t)bitmap_location;
    
    /* Initialize bitmap - clear all bits (mark all as free) */
    for (size_t i = 0; i < bitmap_size; i++) {
        bitmap[i] = 0;
    }
    
    /* Reserve frames used by bitmap itself */
    uint64_t bitmap_start_idx = idx_from_phys(bitmap_location);
    if (bitmap_start_idx != UINT64_MAX) {
        for (uint64_t i = 0; i < bitmap_frames && (bitmap_start_idx + i) < frame_count; i++) {
            bm_set(bitmap_start_idx + i);
        }
    }
    
    /* Reserve low memory (first 1MB) - frame index 0 to 255 */
    // Only reserve if we're using region 0
    if (alloc_start_region == 0) {
        uint64_t low_mem_frames = 0x100000 / PAGE_SIZE; // 256 frames = 1MB
        for (uint64_t i = 0; i < low_mem_frames && i < frame_count; i++) {
            bm_set(i);
        }
        com_write_string(COM1_PORT, "[PHYS] Reserved low memory (first 1MB)\n");
    }
    
    /* CRITICAL: Reserve VGA text buffer at 0xB8000 */
    uint64_t vga_idx = idx_from_phys(0xB8000);
    if (vga_idx != UINT64_MAX && vga_idx < frame_count) {
        bm_set(vga_idx);
        com_write_string(COM1_PORT, "[PHYS] Reserved VGA text buffer at 0xB8000\n");
    }

    log_phys(frame_count, (uint64_t)(uintptr_t)bitmap, bitmap_size);
    
    // Debug: show how many frames are free
    uint64_t free_frames = 0;
    for (uint64_t i = 0; i < frame_count; i++) {
        if (!bm_test(i)) free_frames++;
    }
    com_write_string(COM1_PORT, "[PHYS] Free frames: ");
    char buf[32];
    p = buf;
    p = u_dec(p, free_frames);
    *p = 0;
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " (");
    p = buf;
    p = u_dec(p, (free_frames * PAGE_SIZE) / (1024 * 1024));
    *p = 0;
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " MB)\n");
}

/**********************************************************************
 * ALLOCATION
 **********************************************************************/

uint64_t phys_alloc_frame(void) {
    if (!bitmap || frame_count == 0)
        return 0;

    /* Simple linear search for free frame */
    for (uint64_t i = 0; i < frame_count; i++) {
        if (!bm_test(i)) {
            bm_set(i);
            return phys_from_idx(i);
        }
    }
    
    return 0; /* Out of memory */
}

void phys_free_frame(uint64_t phys) {
    if (!bitmap || phys == 0)
        return;

    uint64_t idx = idx_from_phys(phys);
    if (idx != UINT64_MAX && idx < frame_count) {
        bm_clear(idx);
    }
}

uint64_t phys_alloc_contiguous(size_t n) {
    if (!bitmap || n == 0 || frame_count == 0)
        return 0;

    /* Find n contiguous free frames */
    for (uint64_t start = 0; start <= frame_count - n; start++) {
        int found = 1;
        for (size_t j = 0; j < n; j++) {
            if (bm_test(start + j)) {
                found = 0;
                start += j; /* Skip ahead */
                break;
            }
        }

        if (found) {
            /* Mark all frames as used */
            for (size_t j = 0; j < n; j++) {
                bm_set(start + j);
            }
            return phys_from_idx(start);
        }
    }

    return 0; /* Could not find contiguous block */
}

void phys_reserve_range(uint64_t pstart, uint64_t plen) {
    if (!bitmap)
        return;

    uint64_t start_idx = idx_from_phys(pstart);
    uint64_t end_addr = pstart + plen;
    uint64_t end_idx = idx_from_phys(end_addr);

    if (start_idx == UINT64_MAX)
        return;

    if (end_idx == UINT64_MAX)
        end_idx = frame_count - 1;

    for (uint64_t i = start_idx; i <= end_idx && i < frame_count; i++) {
        bm_set(i);
    }
}

uint64_t phys_total_frames(void) {
    return frame_count;
}

/* Add this function to phys.c */

/**
 * Count how many free frames are currently available
 * Returns: number of free (unset) frames in the bitmap
 */
uint64_t phys_count_free_frames(void) {
    if (!bitmap || frame_count == 0)
        return 0;
    
    uint64_t free = 0;
    for (uint64_t i = 0; i < frame_count; i++) {
        if (!bm_test(i)) {
            free++;
        }
    }
    return free;
}

/**
 * Get memory statistics
 */
void phys_get_stats(uint64_t *total_frames, uint64_t *free_frames, uint64_t *used_frames) {
    if (total_frames) *total_frames = frame_count;
    
    uint64_t free = phys_count_free_frames();
    if (free_frames) *free_frames = free;
    if (used_frames) *used_frames = frame_count - free;
}