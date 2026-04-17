// phys.c - Physical memory allocator (bitmap + refcount)
// SPDX-License-Identifier: GPL-2.0-only

#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/spinlock.h"
#include <stdint.h>
#include <stddef.h>

extern const uint64_t __kernel_virt_offset;
extern uint8_t _kernel_start;
extern uint8_t _kernel_end;
extern uint8_t _boot_end;

#define MAX_REGIONS 64
#define MAX_TOTAL_SUPPORTED_BYTES (8ULL * 1024 * 1024 * 1024)

static uint64_t regions[MAX_REGIONS * 2];
static size_t   region_count = 0;

static uint8_t  *bitmap    = NULL;
static uint32_t *refcnt    = NULL;
static uint64_t  frame_count  = 0;
static size_t    bitmap_size  = 0;
static size_t    refcnt_size  = 0;

/*
 * alloc_start_region: the first region index that the frame allocator manages.
 * Frames in earlier regions are entirely reserved (low RAM, BIOS, etc.).
 */
static size_t alloc_start_region = 0;

static spinlock_t phys_lock __attribute__((aligned(64)));

/* Bitmap helpers */
static inline void bm_set(uint64_t i)   { bitmap[i >> 3] |=  (uint8_t)(1u << (i & 7)); }
static inline void bm_clear(uint64_t i) { bitmap[i >> 3] &= ~(uint8_t)(1u << (i & 7)); }
static inline int  bm_test(uint64_t i)  { return (bitmap[i >> 3] >> (i & 7)) & 1u; }

/* --- Tiny logging helpers (no heap dependency) --- */
static char *_append(char *d, const char *s) { while (*s) *d++ = *s++; return d; }

static char *_u_dec(char *d, uint64_t v) {
    char t[32]; int n = 0;
    do { t[n++] = '0' + (int)(v % 10); v /= 10; } while (v && n < 31);
    while (n--) *d++ = t[n];
    *d = 0; return d;
}

static char *_u_hex(char *d, uint64_t v) {
    char t[32]; int n = 0;
    do { int dg = (int)(v & 0xF); t[n++] = dg < 10 ? '0'+dg : 'a'+(dg-10); v >>= 4; } while (v && n < 31);
    while (n--) *d++ = t[n];
    *d = 0; return d;
}

static void phys_log(const char *msg) { com_write_string(COM1_PORT, msg); }

static void phys_log_addr(const char *prefix, uint64_t v, const char *suffix) {
    char buf[96]; char *p = buf;
    p = _append(p, prefix); p = _append(p, "0x"); p = _u_hex(p, v);
    p = _append(p, suffix); *p = 0;
    com_write_string(COM1_PORT, buf);
}

/* --- Address translation: frame index <-> physical address --- */

/*
 * phys_from_idx: convert a frame index (0-based within the allocatable region
 * set) to a physical address.
 */
static uint64_t phys_from_idx(uint64_t idx) {
    uint64_t offset = idx * PAGE_SIZE;
    for (size_t r = alloc_start_region; r < region_count; r++) {
        uint64_t len = regions[r * 2 + 1];
        if (offset < len) return regions[r * 2 + 0] + offset;
        offset -= len;
    }
    return 0;
}

/*
 * idx_from_phys: convert a physical address to a frame index.
 * Returns UINT64_MAX if the address is not in any managed region.
 */
static uint64_t idx_from_phys(uint64_t addr) {
    uint64_t skip = 0;
    for (size_t r = alloc_start_region; r < region_count; r++) {
        uint64_t base = regions[r * 2 + 0];
        uint64_t len  = regions[r * 2 + 1];
        if (addr >= base && addr < base + len)
            return skip + (addr - base) / PAGE_SIZE;
        skip += len / PAGE_SIZE;
    }
    return UINT64_MAX;
}

/* --- Reservation helpers (called only during init, lock not needed) --- */

static void reserve_frame_idx(uint64_t idx) {
    if (idx == UINT64_MAX || idx >= frame_count) return;
    bm_set(idx);
    refcnt[idx] = 0xFFFFFFFFu; /* pinned */
}

/*
 * reserve_phys_range_init: mark every frame in [start, start+len) as reserved.
 * Safe to call before the spinlock is initialized.
 */
static void reserve_phys_range_init(uint64_t start, uint64_t len) {
    if (len == 0) return;
    uint64_t end = start + len;
    /* align to page boundaries */
    start &= ~(PAGE_SIZE - 1ULL);
    end    = (end + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    for (uint64_t a = start; a < end; a += PAGE_SIZE)
        reserve_frame_idx(idx_from_phys(a));
}

/* --- Initialization --- */

void phys_init(uint64_t total_mem, const void *usable, size_t count) {
    if (total_mem > MAX_TOTAL_SUPPORTED_BYTES)
        total_mem = MAX_TOTAL_SUPPORTED_BYTES;

    region_count = (count > MAX_REGIONS ? MAX_REGIONS : count);
    const uint64_t *src = (const uint64_t *)usable;
    for (size_t i = 0; i < region_count; i++) {
        regions[i * 2 + 0] = src[i * 2 + 0];
        regions[i * 2 + 1] = src[i * 2 + 1];
    }

    /*
     * Compute the minimum physical address at which we may place metadata.
     * Everything below this line belongs to the boot image or kernel image
     * and must never be handed to userland or the heap.
     */
    uint64_t kernel_virt_offset = __kernel_virt_offset;
    uint64_t boot_end_phys      = (uint64_t)(uintptr_t)&_boot_end;
    uint64_t kernel_end_phys    = (uint64_t)(uintptr_t)&_kernel_end - kernel_virt_offset;
    uint64_t min_meta_phys      = boot_end_phys > kernel_end_phys
                                    ? boot_end_phys : kernel_end_phys;
    min_meta_phys = (min_meta_phys + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);

    /* Also reserve the first 1 MiB unconditionally (BIOS, IVT, VGA, etc.). */
    if (min_meta_phys < 0x100000ULL) min_meta_phys = 0x100000ULL;

    /*
     * Find a region with enough room to hold the bitmap + refcount array,
     * starting at or above min_meta_phys.
     *
     * We perform a two-pass scan: first pass picks the smallest region that
     * fits; second pass falls back to the first usable byte in any region.
     */
    uint64_t bitmap_location    = 0;
    alloc_start_region          = 0;

    for (size_t r = 0; r < region_count; r++) {
        uint64_t base = regions[r * 2 + 0];
        uint64_t len  = regions[r * 2 + 1];
        uint64_t end  = base + len;

        uint64_t candidate = base > min_meta_phys ? base : min_meta_phys;
        candidate = (candidate + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);

        if (candidate + PAGE_SIZE * 2 <= end) {
            alloc_start_region = r;
            bitmap_location    = candidate;
            break;
        }
    }

    if (!bitmap_location) {
        /* Last-resort fallback: use the very first region. */
        alloc_start_region = 0;
        bitmap_location    = (min_meta_phys + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);
        phys_log("[PHYS] WARNING: using fallback bitmap placement\n");
    }

    /* Count total frames in regions [alloc_start_region, region_count). */
    uint64_t total_frames = 0;
    for (size_t r = alloc_start_region; r < region_count; r++)
        total_frames += regions[r * 2 + 1] / PAGE_SIZE;

    frame_count  = total_frames;
    bitmap_size  = (frame_count + 7) / 8;
    refcnt_size  = (size_t)(frame_count * sizeof(uint32_t));

    uint64_t meta_bytes   = (uint64_t)bitmap_size + (uint64_t)refcnt_size;
    uint64_t bitmap_frames = (meta_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    phys_log_addr("[PHYS] Bitmap at phys ", bitmap_location, "\n");
    {
        char buf[80]; char *p = buf;
        p = _append(p, "[PHYS] frames="); p = _u_dec(p, frame_count);
        p = _append(p, " bitmap_bytes="); p = _u_dec(p, bitmap_size);
        p = _append(p, " refcnt_bytes="); p = _u_dec(p, refcnt_size);
        p = _append(p, "\n"); *p = 0;
        com_write_string(COM1_PORT, buf);
    }

    /* Map metadata via the kernel physmap. */
    bitmap = (uint8_t  *)phys_to_virt_kernel(bitmap_location);
    refcnt = (uint32_t *)phys_to_virt_kernel(bitmap_location + bitmap_size);

    if (!bitmap || !refcnt) {
        phys_log("[PHYS] FATAL: cannot access bitmap via physmap\n");
        __asm__ volatile("cli; hlt");
    }

    /* Zero-initialize both arrays. */
    for (size_t i = 0; i < bitmap_size; i++) bitmap[i] = 0;
    for (size_t i = 0; i < frame_count;  i++) refcnt[i] = 0;

    /*
     * --- RESERVATION PASS ---
     *
     * Order matters: we must first pin the bitmap frames themselves so
     * reserve_phys_range_init() works correctly for subsequent reservations.
     */

    /* 1. Pin the bitmap + refcount storage. */
    {
        uint64_t bm_start = bitmap_location;
        uint64_t bm_end   = bitmap_location + bitmap_frames * PAGE_SIZE;
        for (uint64_t a = bm_start; a < bm_end; a += PAGE_SIZE)
            reserve_frame_idx(idx_from_phys(a));
        phys_log_addr("[PHYS] Pinned bitmap region ", bm_start, "");
        phys_log_addr(" - ", bm_end, "\n");
    }

    /*
     * 2. Reserve the entire physical range below min_meta_phys.
     *    This covers the boot stub, early page tables, the kernel image, and
     *    the first 1 MiB of conventional RAM.
     *
     *    We iterate over ALL regions (not just alloc_start_region+) because
     *    low RAM may appear in region 0 even when alloc_start_region > 0.
     *    idx_from_phys() returns UINT64_MAX for any address not in the
     *    managed set, so reserve_frame_idx() safely ignores those.
     */
    {
        phys_log_addr("[PHYS] Reserving all frames below ", min_meta_phys, "\n");
        for (size_t r = 0; r < region_count; r++) {
            uint64_t base = regions[r * 2 + 0];
            uint64_t len  = regions[r * 2 + 1];
            uint64_t end  = base + len;
            uint64_t cap  = end < min_meta_phys ? end : min_meta_phys;
            if (cap > base)
                reserve_phys_range_init(base, cap - base);
        }
    }

    /* 3. Reserve VGA text buffer. */
    reserve_phys_range_init(0xB8000ULL, PAGE_SIZE);
    phys_log("[PHYS] Reserved VGA text buffer (0xB8000)\n");

    /* 4. Initialize the spinlock. */
    spinlock_init(&phys_lock);

    /* 5. Report free frame count. */
    {
        uint64_t free_frames = 0;
        for (uint64_t i = 0; i < frame_count; i++)
            if (!bm_test(i)) free_frames++;
        char buf[80]; char *p = buf;
        p = _append(p, "[PHYS] Free frames: "); p = _u_dec(p, free_frames);
        p = _append(p, " (");
        p = _u_dec(p, (free_frames * PAGE_SIZE) / (1024 * 1024));
        p = _append(p, " MB)\n"); *p = 0;
        com_write_string(COM1_PORT, buf);
    }
}

/* --- Allocation --- */

uint64_t phys_alloc_frame(void) {
    if (!bitmap || !frame_count) return 0;
    spinlock_lock(&phys_lock);
    for (uint64_t i = 0; i < frame_count; i++) {
        if (!bm_test(i)) {
            bm_set(i);
            refcnt[i] = 1;
            uint64_t phys = phys_from_idx(i);
            spinlock_unlock(&phys_lock);
            return phys;
        }
    }
    spinlock_unlock(&phys_lock);
    return 0;
}

uint64_t phys_alloc_frame_below(uint64_t max_phys) {
    if (!bitmap || !frame_count || !max_phys) return 0;
    spinlock_lock(&phys_lock);
    for (uint64_t i = 0; i < frame_count; i++) {
        if (bm_test(i)) continue;
        uint64_t phys = phys_from_idx(i);
        if (!phys || phys >= max_phys) continue;
        bm_set(i);
        refcnt[i] = 1;
        spinlock_unlock(&phys_lock);
        return phys;
    }
    spinlock_unlock(&phys_lock);
    return 0;
}

void phys_free_frame(uint64_t phys) {
    if (!bitmap || !phys) return;
    uint64_t idx = idx_from_phys(phys);
    if (idx == UINT64_MAX || idx >= frame_count) return;

    spinlock_lock(&phys_lock);
    if (refcnt[idx] == 0xFFFFFFFFu) { spinlock_unlock(&phys_lock); return; } /* pinned */
    if (refcnt[idx] > 1)            { refcnt[idx]--; spinlock_unlock(&phys_lock); return; }
    refcnt[idx] = 0;
    bm_clear(idx);
    spinlock_unlock(&phys_lock);
}

static int is_pow2(uint64_t x) { return x && !(x & (x - 1)); }

/*
 * phys_alloc_contiguous: find 'nframes' consecutive free frames within a
 * single physical region (guarantees physical contiguity).
 *
 * The lock is held for the entire search+mark to avoid a TOCTOU race.
 */
uint64_t phys_alloc_contiguous(size_t nframes) {
    return phys_alloc_contiguous_aligned(nframes, PAGE_SIZE);
}

uint64_t phys_alloc_contiguous_aligned(size_t nframes, uint64_t align) {
    if (!bitmap || !nframes || !frame_count) return 0;
    if (!align) align = PAGE_SIZE;
    if (!is_pow2(align) || align % PAGE_SIZE) return 0;
    if (nframes > frame_count) return 0;

    spinlock_lock(&phys_lock);

    uint64_t skip = 0; /* frame index offset for the current region */
    for (size_t r = alloc_start_region; r < region_count; r++) {
        uint64_t base   = regions[r * 2 + 0];
        uint64_t len    = regions[r * 2 + 1];
        uint64_t frames = len / PAGE_SIZE;

        if (frames < (uint64_t)nframes) { skip += frames; continue; }

        for (uint64_t s = 0; s <= frames - (uint64_t)nframes; s++) {
            uint64_t phys_cand = base + s * PAGE_SIZE;
            if ((phys_cand & (align - 1)) != 0) continue;

            uint64_t start_idx = skip + s;
            int ok = 1;
            for (size_t j = 0; j < nframes; j++) {
                if (bm_test(start_idx + j)) { s += j; ok = 0; break; }
            }
            if (!ok) continue;

            for (size_t j = 0; j < nframes; j++) {
                bm_set(start_idx + j);
                refcnt[start_idx + j] = 1;
            }
            spinlock_unlock(&phys_lock);
            return phys_cand;
        }
        skip += frames;
    }

    spinlock_unlock(&phys_lock);
    return 0;
}

/*
 * phys_reserve_range: pin a physical range so the allocator never returns it.
 * Called after phys_init() (e.g. from paging_reserve_bootloader_tables,
 * memory_init, etc.).  The spinlock is taken internally.
 */
void phys_reserve_range(uint64_t pstart, uint64_t plen) {
    if (!bitmap || !plen) return;

    uint64_t end = pstart + plen;
    if (end <= pstart) return; /* overflow guard */

    spinlock_lock(&phys_lock);

    for (uint64_t a = pstart & ~(PAGE_SIZE - 1ULL); a < end; a += PAGE_SIZE) {
        uint64_t idx = idx_from_phys(a);
        if (idx == UINT64_MAX || idx >= frame_count) continue;
        bm_set(idx);
        refcnt[idx] = 0xFFFFFFFFu;
    }

    spinlock_unlock(&phys_lock);
}

uint64_t phys_total_frames(void) { return frame_count; }

uint64_t phys_count_free_frames(void) {
    if (!bitmap || !frame_count) return 0;
    uint64_t free = 0;
    /* Read-only scan: no lock needed (result is approximate by nature). */
    for (uint64_t i = 0; i < frame_count; i++)
        if (!bm_test(i)) free++;
    return free;
}

void phys_ref_inc(uint64_t phys) {
    if (!refcnt || !phys) return;
    uint64_t idx = idx_from_phys(phys);
    if (idx == UINT64_MAX || idx >= frame_count) return;
    spinlock_lock(&phys_lock);
    if (refcnt[idx] != 0xFFFFFFFFu) {
        if (refcnt[idx] == 0) refcnt[idx] = 1;
        else                  refcnt[idx]++;
    }
    spinlock_unlock(&phys_lock);
}

void phys_ref_dec(uint64_t phys) { phys_free_frame(phys); }

uint32_t phys_ref_get(uint64_t phys) {
    if (!refcnt || !phys) return 0;
    uint64_t idx = idx_from_phys(phys);
    if (idx == UINT64_MAX || idx >= frame_count) return 0;
    return refcnt[idx];
}

void phys_get_stats(uint64_t *total, uint64_t *free, uint64_t *used) {
    uint64_t f = phys_count_free_frames();
    if (total) *total = frame_count;
    if (free)  *free  = f;
    if (used)  *used  = frame_count - f;
}