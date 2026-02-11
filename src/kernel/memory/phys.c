//phys.c
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/spinlock.h"

/* Linker-provided layout constants */
/* Layout constants stored in kernel .rodata by the linker script. */
extern const uint64_t __kernel_virt_offset;
extern uint8_t _kernel_start;
extern uint8_t _kernel_end;
extern uint8_t _boot_end;
#include <stdint.h>
#include <stddef.h>

#define MAX_REGIONS 64
#define MAX_TOTAL_SUPPORTED_BYTES (8ULL * 1024 * 1024 * 1024)  // 8 GiB

static uint64_t regions[MAX_REGIONS * 2];
static size_t region_count = 0;

static uint8_t *bitmap = NULL;
static uint32_t *refcnt = NULL;
static uint64_t frame_count = 0;
static size_t bitmap_size = 0;
static size_t refcnt_size = 0;

/* Which region index the bitmap+frame space begins at */
static size_t alloc_start_region = 0;

/* Spinlock to protect bitmap and refcnt operations (SMP-safe) */
/* Cache-line aligned to prevent false sharing */
static spinlock_t phys_lock __attribute__((aligned(64)));
/* Fine-grained locking: 8 zones for reduced contention */
#define PHYS_NUM_ZONES 8
static spinlock_t zone_locks[PHYS_NUM_ZONES] __attribute__((aligned(64)));

static inline int get_zone_for_frame(uint64_t frame_idx) {
    return (int)(frame_idx % PHYS_NUM_ZONES);
}

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

    /* Find the best region to place bitmap.
     * IMPORTANT: This metadata must NOT overlap the boot image or the kernel image.
     */
    alloc_start_region = 0;
    uint64_t bitmap_location = 0;

    uint64_t kernel_virt_offset = __kernel_virt_offset;
    uint64_t boot_end_phys = (uint64_t)(uintptr_t)&_boot_end;
    uint64_t kernel_end_phys = (uint64_t)(uintptr_t)&_kernel_end - kernel_virt_offset;

    uint64_t min_meta_phys = boot_end_phys;
    if (kernel_end_phys > min_meta_phys) min_meta_phys = kernel_end_phys;
    min_meta_phys = (min_meta_phys + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    /* Reserve everything below min_meta_phys so we never allocate from the
     * boot image / early paging structures / kernel physical image area.
     */
    uint64_t reserve_below_phys = min_meta_phys;
    for (size_t i = 0; i < region_count; i++) {
        uint64_t base = regions[i*2+0];
        uint64_t len  = regions[i*2+1];
        uint64_t end  = base + len;

        uint64_t candidate = base;
        if (candidate < 0x100000) candidate = 0x100000;
        if (candidate < min_meta_phys) candidate = min_meta_phys;
        candidate = (candidate + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        if (candidate >= base && candidate + (PAGE_SIZE * 2) <= end) {
            alloc_start_region = i;
            bitmap_location = candidate;
            break;
        }
    }

    if (bitmap_location == 0) {
        /* Fallback */
        alloc_start_region = 0;
        uint64_t base = regions[0];
        uint64_t len  = regions[1];
        uint64_t end  = base + len;
        uint64_t candidate = base + PAGE_SIZE;
        if (candidate < min_meta_phys) candidate = min_meta_phys;
        candidate = (candidate + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (candidate + (PAGE_SIZE * 2) <= end) {
            bitmap_location = candidate;
        } else {
            bitmap_location = 0x10000;
        }
    }

    /* Calculate total frames from regions starting at alloc_start_region */
    uint64_t total_frames = 0;
    for (size_t r = alloc_start_region; r < region_count; r++) {
        total_frames += regions[r*2+1] / PAGE_SIZE;
    }

    frame_count = total_frames;
    bitmap_size = (frame_count + 7) / 8;

    // Refcount array sits right after the bitmap.
    refcnt_size = (size_t)(frame_count * sizeof(uint32_t));

    /* Calculate how many frames the bitmap+refcount storage occupies */
    uint64_t meta_bytes = bitmap_size + refcnt_size;
    uint64_t bitmap_frames = (meta_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

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

    /* Set bitmap pointer via kernel physmap (do NOT assume identity mapping). */
    bitmap = (uint8_t *)phys_to_virt_kernel(bitmap_location);
    refcnt = (uint32_t *)phys_to_virt_kernel(bitmap_location + bitmap_size);

    /* Initialize metadata */
    for (size_t i = 0; i < bitmap_size; i++) {
        bitmap[i] = 0;
    }
    // Clear refcount table
    for (size_t i = 0; i < (refcnt_size / sizeof(uint32_t)); i++) {
        refcnt[i] = 0;
    }
    
    /* Reserve frames used by bitmap+refcount itself */
    uint64_t bitmap_start_idx = idx_from_phys(bitmap_location);
    if (bitmap_start_idx != UINT64_MAX) {
        for (uint64_t i = 0; i < bitmap_frames && (bitmap_start_idx + i) < frame_count; i++) {
            bm_set(bitmap_start_idx + i);
            // Pin metadata frames
            refcnt[bitmap_start_idx + i] = 0xFFFFFFFFu;
        }
    }

    /* Reserve the entire early boot/kernel physical region (from alloc_start_region base). */
    {
        uint64_t base = regions[alloc_start_region*2+0];
        if (reserve_below_phys > base) {
            uint64_t nframes = (reserve_below_phys - base) / PAGE_SIZE;
            if (nframes > frame_count) nframes = frame_count;
            for (uint64_t i = 0; i < nframes; i++) {
                bm_set(i);
                if (refcnt) refcnt[i] = 0xFFFFFFFFu;
            }
            com_write_string(COM1_PORT, "[PHYS] Reserved early region below ");
            char hb[32];
            char *pp = hb; pp = u_hex(pp, reserve_below_phys); *pp = 0;
            com_write_string(COM1_PORT, "0x");
            com_write_string(COM1_PORT, hb);
            com_write_string(COM1_PORT, "\n");
        }
    }

    /* Reserve low memory (first 1MB) - only meaningful if allocator starts at 0. */
    if (alloc_start_region == 0) {
        uint64_t low_mem_frames = 0x100000 / PAGE_SIZE; // 256 frames = 1MB
        for (uint64_t i = 0; i < low_mem_frames && i < frame_count; i++) {
            bm_set(i);
            if (refcnt) refcnt[i] = 0xFFFFFFFFu;
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
    
    // Initialize the physical memory spinlock
    spinlock_init(&phys_lock);
    for (int z = 0; z < PHYS_NUM_ZONES; z++) { spinlock_init(&zone_locks[z]); }
    
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

    spinlock_lock(&phys_lock);
    /* Simple linear search for free frame */
    for (uint64_t i = 0; i < frame_count; i++) {
        if (!bm_test(i)) {
            bm_set(i);
            if (refcnt) refcnt[i] = 1;
            uint64_t phys = phys_from_idx(i);
            spinlock_unlock(&phys_lock);
            return phys;
        }
    }
    spinlock_unlock(&phys_lock);
    return 0; /* Out of memory */
}

uint64_t phys_alloc_frame_below(uint64_t max_phys) {
    if (!bitmap || frame_count == 0 || max_phys == 0)
        return 0;

    spinlock_lock(&phys_lock);
    for (uint64_t i = 0; i < frame_count; i++) {
        if (bm_test(i)) continue;

        uint64_t phys = phys_from_idx(i);
        if (phys == 0) continue;
        if (phys >= max_phys) continue;

        bm_set(i);
        if (refcnt) refcnt[i] = 1;
        spinlock_unlock(&phys_lock);
        return phys;
    }
    spinlock_unlock(&phys_lock);
    return 0;
}

void phys_free_frame(uint64_t phys) {
    if (!bitmap || phys == 0)
        return;

    uint64_t idx = idx_from_phys(phys);
    if (idx != UINT64_MAX && idx < frame_count) {
        spinlock_lock(&phys_lock);
        if (!refcnt) {
            bm_clear(idx);
            spinlock_unlock(&phys_lock);
            return;
        }
        if (refcnt[idx] == 0xFFFFFFFFu) {
            spinlock_unlock(&phys_lock);
            return; /* pinned */
        }
        if (refcnt[idx] > 1) {
            refcnt[idx]--;
            spinlock_unlock(&phys_lock);
            return;
        }
        refcnt[idx] = 0;
        bm_clear(idx);
        spinlock_unlock(&phys_lock);
    }
}

static int is_pow2_u64(uint64_t x) { return x && ((x & (x - 1)) == 0); }

// Internal helper: allocate within a *single* physical region so the returned run is
// guaranteed physically contiguous.
static uint64_t phys_alloc_contiguous_in_region(size_t region_idx, size_t nframes, uint64_t align) {
    if (nframes == 0) return 0;
    if (!is_pow2_u64(align)) return 0;
    if ((align % PAGE_SIZE) != 0) return 0;

    uint64_t base = regions[region_idx * 2 + 0];
    uint64_t len  = regions[region_idx * 2 + 1];
    uint64_t frames = len / PAGE_SIZE;
    if (frames < nframes) return 0;

    // Compute the bitmap index offset for this region.
    uint64_t skip = 0;
    for (size_t r = alloc_start_region; r < region_idx; r++) {
        skip += regions[r * 2 + 1] / PAGE_SIZE;
    }

    // Find n contiguous free frames in this region, honoring alignment.
    for (uint64_t start_in_region = 0; start_in_region <= (frames - (uint64_t)nframes); start_in_region++) {
        uint64_t phys_candidate = base + start_in_region * PAGE_SIZE;
        if ((phys_candidate & (align - 1)) != 0) continue;

        uint64_t start_idx = skip + start_in_region;

        int found = 1;
        for (size_t j = 0; j < nframes; j++) {
            if (bm_test(start_idx + j)) {
                found = 0;
                start_in_region += j; // skip ahead
                break;
            }
        }

        if (found) {
            for (size_t j = 0; j < nframes; j++) {
                bm_set(start_idx + j);
                if (refcnt) refcnt[start_idx + j] = 1;
            }
            return phys_candidate;
        }
    }

    return 0;
}

uint64_t phys_alloc_contiguous_aligned(size_t nframes, uint64_t align) {
    if (!bitmap || nframes == 0 || frame_count == 0) return 0;
    if (nframes > frame_count) return 0;

    if (align == 0) align = PAGE_SIZE;
    if (!is_pow2_u64(align)) return 0;
    if ((align % PAGE_SIZE) != 0) return 0;

    for (size_t r = alloc_start_region; r < region_count; r++) {
        uint64_t p = phys_alloc_contiguous_in_region(r, nframes, align);
        if (p) return p;
    }

    return 0;
}

uint64_t phys_alloc_contiguous(size_t nframes) {
    return phys_alloc_contiguous_aligned(nframes, PAGE_SIZE);
}

void phys_reserve_range(uint64_t pstart, uint64_t plen) {
    if (!bitmap || plen == 0)
        return;

    /* Reserve [pstart, pstart+plen) (half-open interval).
     * Be careful with end boundaries: idx_from_phys(end) is not valid when end==region_end.
     */
    uint64_t start_idx = idx_from_phys(pstart);
    if (start_idx == UINT64_MAX) return;

    uint64_t end_addr_excl = pstart + plen;
    if (end_addr_excl <= pstart) {
        /* overflow */
        return;
    }

    uint64_t last_addr_incl = end_addr_excl - 1;
    uint64_t end_idx = idx_from_phys(last_addr_incl);
    if (end_idx == UINT64_MAX) {
        /* If the end lands outside our allocatable regions, clamp to last frame. */
        end_idx = frame_count ? (frame_count - 1) : 0;
    }

    if (end_idx < start_idx) return;

    for (uint64_t i = start_idx; i <= end_idx && i < frame_count; i++) {
        bm_set(i);
        if (refcnt) refcnt[i] = 0xFFFFFFFFu; /* pin reserved frames */
    }
}

uint64_t phys_total_frames(void) {
    return frame_count;
}

void phys_ref_inc(uint64_t phys) {
    if (!refcnt || phys == 0) return;
    uint64_t idx = idx_from_phys(phys);
    if (idx == UINT64_MAX || idx >= frame_count) return;
    if (refcnt[idx] == 0xFFFFFFFFu) return;
    if (refcnt[idx] == 0) refcnt[idx] = 1;
    else refcnt[idx]++;
}

void phys_ref_dec(uint64_t phys) {
    phys_free_frame(phys);
}

uint32_t phys_ref_get(uint64_t phys) {
    if (!refcnt || phys == 0) return 0;
    uint64_t idx = idx_from_phys(phys);
    if (idx == UINT64_MAX || idx >= frame_count) return 0;
    return refcnt[idx];
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