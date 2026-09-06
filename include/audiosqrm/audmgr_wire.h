#ifndef AUDMGR_WIRE_H
#define AUDMGR_WIRE_H

#include <stdint.h>
#include "audiosqrm/types.h"

/*
 * Wire protocol for $/dev/audio/aud0.
 *
 * This is deliberately its own format, NOT a guess at mvc3.h's actual
 * mvc3_hdr_t layout (which wasn't available). It follows the same
 * *pattern* mvi0 uses — magic/cmd/size header framed over write(),
 * responses buffered for the next read(), a MAP_RING command that hands
 * back a zero-copy user VA when devfs_mmap_region is available and falls
 * back to a copy-batch SUBMIT command otherwise.
 */

#define AUDMGR_MAGIC        0x41554431u /* "AUD1" */
#define AUDMGR_ABI_VERSION  1u

typedef enum {
    AUDMGR_CMD_GET_INFO   = 1, /* -> aud_get_info_resp_t                */
    AUDMGR_CMD_SET_FORMAT = 2, /* aud_set_format_req_t -> aud_hdr_t     */
    AUDMGR_CMD_MAP_RING   = 3, /* aud_map_ring_req_t -> aud_map_ring_resp_t */
    AUDMGR_CMD_SUBMIT     = 4, /* aud_submit_t -> (no resp)             */
    AUDMGR_CMD_SET_VOLUME = 5, /* aud_set_volume_req_t -> aud_hdr_t     */
} audmgr_cmd_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t cmd;
    uint32_t size_bytes; /* total size of this packet including header */
} aud_hdr_t;

/* GET_INFO: no request body beyond the header. */
typedef struct __attribute__((packed)) {
    aud_hdr_t hdr;
    aud_pcm_config_t current;
    uint32_t caps; /* reserved for future use, 0 for now */
    char driver[32];
} aud_get_info_resp_t;

typedef struct __attribute__((packed)) {
    aud_hdr_t hdr;
    aud_pcm_config_t requested;
} aud_set_format_req_t;

/* One ring slot: a fixed-size chunk of interleaved PCM frames. Kept small
 * and fixed so the ring can be a flat array, same idea as mvc3_ring_slot_t
 * but carrying samples instead of draw ops. */
#define AUDMGR_RING_FRAMES_PER_SLOT 256u
#define AUDMGR_RING_MAX_CHANNELS    2u

typedef struct __attribute__((packed)) {
    int16_t samples[AUDMGR_RING_FRAMES_PER_SLOT * AUDMGR_RING_MAX_CHANNELS];
    uint32_t frame_count; /* <= AUDMGR_RING_FRAMES_PER_SLOT, actual fill  */
} audmgr_ring_slot_t;

typedef struct __attribute__((packed)) {
    aud_hdr_t hdr;
    uint64_t requested_size; /* bytes; 0 = use server default */
} aud_map_ring_req_t;

typedef struct __attribute__((packed)) {
    aud_hdr_t hdr;
    uint64_t user_addr;   /* 0 if zero-copy mmap unavailable -> use SUBMIT */
    uint64_t actual_size;
    uint32_t slot_size;   /* sizeof(audmgr_ring_slot_t), for sanity checking */
    uint32_t slot_count;
} aud_map_ring_resp_t;

/* SUBMIT: copy-batch fallback path when there's no mmap'd ring, OR the
 * "I just wrote N more slots into the ring, go play them" notification
 * when there IS a mapped ring (count/slots ignored in the latter case,
 * server just re-reads ring_head..ring_head+advance). */
typedef struct __attribute__((packed)) {
    aud_hdr_t hdr;
    uint32_t advance;         /* number of ring slots newly filled */
    uint32_t inline_count;    /* number of slots appended below, for copy-batch mode */
    audmgr_ring_slot_t slots[]; /* only present in copy-batch mode */
} aud_submit_t;

typedef struct __attribute__((packed)) {
    aud_hdr_t hdr;
    uint8_t left_atten;  /* 0-31, 0 = loudest */
    uint8_t right_atten;
    uint8_t mute;
    uint8_t _pad;
} aud_set_volume_req_t;

/* mmap offsets, same convention as MVC3_OFF_RING */
#define AUDMGR_OFF_RING 0x0ull

#endif /* AUDMGR_WIRE_H */