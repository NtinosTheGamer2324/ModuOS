// emu6502.h
#pragma once
#include <stdint.h>
#include <stddef.h>

/* Minimal 6502/6507 CPU core.
 * - No decimal mode (good for NES/Atari 2600)
 * - Pluggable memory bus callbacks
 */

typedef uint8_t (*emu6502_read8_fn)(void *user, uint16_t addr);
typedef void (*emu6502_write8_fn)(void *user, uint16_t addr, uint8_t value);

typedef struct {
    /* Registers */
    uint16_t pc;
    uint8_t a, x, y;
    uint8_t sp;
    uint8_t p; /* NV-BDIZC */

    /* Cycle counter */
    uint64_t cycles;

    /* Bus */
    void *bus_user;
    emu6502_read8_fn read8;
    emu6502_write8_fn write8;
} emu6502_t;

void emu6502_init(emu6502_t *c, void *bus_user, emu6502_read8_fn r8, emu6502_write8_fn w8);
void emu6502_reset(emu6502_t *c);

/* Execute one instruction, return cycles consumed. */
uint32_t emu6502_step(emu6502_t *c);
