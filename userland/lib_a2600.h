#pragma once
#include <stdint.h>
#include <stddef.h>
#include "lib_8bit.h"
#include "emu6502.h"

typedef struct {
    const uint8_t *rom;
    size_t rom_size;
} a2600_cart_t;

/* Full TIA sprite/object state */
typedef struct {
    uint8_t grp;      /* Graphics pattern */
    uint8_t color;    /* Color register */
    uint8_t x;        /* Horizontal position */
    uint8_t refp;     /* Reflect flag */
    uint8_t nusiz;    /* Number-size control */
    uint8_t hmove;    /* Horizontal motion */
    uint8_t vdel;     /* Vertical delay */
} tia_sprite_t;

/* RIOT timer state */
typedef struct {
    uint8_t timer;       /* Current timer value */
    uint8_t intim;       /* Last read INTIM value */
    uint16_t interval;   /* Timer interval (1/8/64/1024) */
    uint64_t last_cycle; /* Last update cycle */
    uint8_t underflow;   /* Timer underflow flag */
} riot_timer_t;

typedef struct {
    a2600_cart_t cart;

    /* 128 bytes RIOT RAM */
    uint8_t ram[128];

    /* TIA register file */
    uint8_t tia[0x40];

    /* RIOT registers */
    uint8_t riot[0x20];

    /* RIOT timer */
    riot_timer_t timer;

    /* Full TIA sprite state */
    tia_sprite_t p0, p1;  /* Players */
    tia_sprite_t m0, m1;  /* Missiles */
    tia_sprite_t ball;    /* Ball */

    /* TIA collision flags */
    uint8_t cxm0p;   /* Missile 0 to Player collisions */
    uint8_t cxm1p;   /* Missile 1 to Player collisions */
    uint8_t cxp0fb;  /* Player 0 to Field/Ball */
    uint8_t cxp1fb;  /* Player 1 to Field/Ball */
    uint8_t cxm0fb;  /* Missile 0 to Field/Ball */
    uint8_t cxm1fb;  /* Missile 1 to Field/Ball */
    uint8_t cxblpf;  /* Ball to Playfield */
    uint8_t cxppmm;  /* Player/Missile collisions */

    /* Input state (joystick via mouse) */
    struct {
        int32_t mouse_x;
        int32_t mouse_y;
        uint8_t button;      /* Left mouse button */
        uint8_t swcha;       /* Joystick ports */
        uint8_t swchb;       /* Console switches */
    } input;

    /* 6507 CPU */
    emu6502_t cpu;

    /* Global cycle counter */
    uint64_t cycles;
    uint64_t frame_start_cycles;

    /* Per-scanline state capture (192 visible lines) */
    struct {
        uint8_t valid;
        uint8_t grp0, grp1;
        uint8_t colup0, colup1;
        uint8_t refp0, refp1;
        uint8_t nusiz0, nusiz1;
        uint8_t x0, x1;
        uint8_t m0_x, m1_x, ball_x;
        uint8_t m0_grp, m1_grp, ball_grp;
        uint8_t colupf, colubk;
        uint8_t pf0, pf1, pf2;
        uint8_t ctrlpf;
    } vis[192];

    /* CPU fault info */
    uint8_t faulted;
    uint16_t fault_pc;
    uint8_t fault_opcode;

    /* Debug trace */
    uint32_t tia_trace_budget;
} a2600_t;

int a2600_load_cart(a2600_t *a, const uint8_t *rom, size_t rom_size);
void a2600_step_frame(a2600_t *a);
void a2600_render_full(const a2600_t *a, emu_frame_t *frame);
void a2600_update_input(a2600_t *a, int32_t mouse_x, int32_t mouse_y, uint8_t button);
