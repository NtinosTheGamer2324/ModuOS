#include "libc.h"
#include "lib_a2600.h"

static uint8_t a2600_read8(void *user, uint16_t addr);
static void a2600_write8(void *user, uint16_t addr, uint8_t v);

static inline uint8_t cart_read(const a2600_t *a, uint16_t addr) {
    size_t sz = a->cart.rom_size;
    if (!a->cart.rom || sz == 0) return 0xFF;

    uint16_t off = (uint16_t)(addr & 0x0FFF);

    if (sz == 2048) {
        off &= 0x07FF;
    } else if (sz == 4096) {
        off &= 0x0FFF;
    } else {
        off &= 0x0FFF;
    }

    return a->cart.rom[off];
}

/* Update RIOT timer */
static void riot_timer_update(a2600_t *a) {
    if (a->timer.interval == 0) return;

    uint64_t elapsed = a->cpu.cycles - a->timer.last_cycle;
    uint64_t ticks = elapsed / a->timer.interval;

    if (ticks > 0) {
        a->timer.last_cycle = a->cpu.cycles;
        
        if (ticks >= a->timer.timer) {
            a->timer.timer = 0;
            a->timer.underflow = 1;
        } else {
            a->timer.timer -= (uint8_t)ticks;
        }
    }

    a->timer.intim = a->timer.timer;
}

int a2600_load_cart(a2600_t *a, const uint8_t *rom, size_t rom_size) {
    if (!a || !rom || rom_size == 0) return -1;
    memset(a, 0, sizeof(*a));
    a->cart.rom = rom;
    a->cart.rom_size = rom_size;

    if (!(rom_size == 2048 || rom_size == 4096 || rom_size == 8192 || rom_size == 16384)) {
        return -2;
    }

    /* Init CPU */
    emu6502_init(&a->cpu, a, a2600_read8, a2600_write8);

    /* Init input - centered joystick, button up */
    a->input.swcha = 0xFF;  /* All directions released */
    a->input.swchb = 0x0B;  /* Console switches (color, P0/P1 difficulty) */
    a->input.mouse_x = 80;
    a->input.mouse_y = 96;

    /* Init timer */
    a->timer.interval = 1024;
    a->timer.timer = 0xFF;
    a->timer.last_cycle = 0;

#ifdef A2600_TRACE_TIA
    a->tia_trace_budget = 2000;
#else
    a->tia_trace_budget = 0;
#endif

    emu6502_reset(&a->cpu);
    return 0;
}

void a2600_update_input(a2600_t *a, int32_t mouse_x, int32_t mouse_y, uint8_t button) {
    if (!a) return;

    /* Map mouse to screen coords (assume 160x192) */
    a->input.mouse_x = mouse_x;
    a->input.mouse_y = mouse_y;
    a->input.button = button;

    /* Map mouse position to joystick directions
     * SWCHA bits: 7=R P0, 6=L P0, 5=D P0, 4=U P0, 3-0=P1
     * 0=pressed, 1=released (active low)
     */
    uint8_t joy = 0xFF;
    
    if (mouse_x < 40)  joy &= ~0x40;  /* Left */
    if (mouse_x > 120) joy &= ~0x80;  /* Right */
    if (mouse_y < 48)  joy &= ~0x10;  /* Up */
    if (mouse_y > 144) joy &= ~0x20;  /* Down */

    a->input.swcha = joy;
}

void a2600_step_frame(a2600_t *a) {
    if (!a) return;

    a->frame_start_cycles = a->cpu.cycles;
    memset(a->vis, 0, sizeof(a->vis));

    /* PAL: 312 scanlines * 76 cycles = 23712 cycles/frame */
    const uint64_t frame_cycles = 312ULL * 76ULL;
    uint64_t target = a->cpu.cycles + frame_cycles;

    while (a->cpu.cycles < target) {
        uint16_t pc = a->cpu.pc;
        uint8_t op = a->cpu.read8 ? a->cpu.read8(a->cpu.bus_user, pc) : 0xFF;

        /* Update RIOT timer */
        riot_timer_update(a);

        uint32_t cyc = emu6502_step(&a->cpu);
        if (cyc == 0) {
            if (!a->faulted) {
                a->faulted = 1;
                a->fault_pc = pc;
                a->fault_opcode = op;
            }
            break;
        }
    }

    a->cycles = a->cpu.cycles;
}

static uint8_t a2600_read8(void *user, uint16_t addr) {
    a2600_t *a = (a2600_t*)user;
    addr &= 0x1FFF;

    /* TIA register reads */
    if (addr < 0x80) {
        uint16_t reg = (uint16_t)(addr & 0x3F);
        
        /* Collision registers */
        if (reg >= 0x00 && reg <= 0x07) {
            switch (reg) {
                case 0x00: return a->cxm0p;
                case 0x01: return a->cxm1p;
                case 0x02: return a->cxp0fb;
                case 0x03: return a->cxp1fb;
                case 0x04: return a->cxm0fb;
                case 0x05: return a->cxm1fb;
                case 0x06: return a->cxblpf;
                case 0x07: return a->cxppmm;
            }
        }
        
        /* Input ports */
        if (reg >= 0x08 && reg <= 0x0D) {
            /* INPT0-5: paddle/joystick button inputs */
            /* Map mouse button to trigger */
            return a->input.button ? 0x00 : 0x80;
        }
        
        return 0;
    }

    /* RIOT RAM */
    if (addr >= 0x80 && addr <= 0xFF) {
        return a->ram[addr & 0x7F];
    }

    /* RIOT I/O */
    if ((addr & 0x0F80) == 0x0280) {
        uint8_t reg = (uint8_t)(addr & 0x1F);
        
        switch (reg) {
            case 0x00: /* SWCHA - joystick ports */
                return a->input.swcha;
            case 0x02: /* SWCHB - console switches */
                return a->input.swchb;
            case 0x04: /* INTIM - timer */
                riot_timer_update(a);
                return a->timer.intim;
            default:
                return a->riot[reg];
        }
    }

    /* Cartridge */
    if (addr >= 0x1000) {
        return cart_read(a, addr);
    }

    return 0xFF;
}

static void a2600_write8(void *user, uint16_t addr, uint8_t v) {
    a2600_t *a = (a2600_t*)user;
    addr &= 0x1FFF;

    if (addr < 0x80) {
        uint16_t reg = (uint16_t)(addr & 0x3F);
        a->tia[reg] = v;

        /* Derive scanline and X position */
        uint64_t fc = (a->cpu.cycles >= a->frame_start_cycles) ? 
                      (a->cpu.cycles - a->frame_start_cycles) : 0;
        uint32_t sl = (uint32_t)(fc / 76ULL);
        uint32_t cyc_in = (uint32_t)(fc % 76ULL);
        uint32_t cc = cyc_in * 3u;
        uint32_t px = (cc * 160u) / 228u;
        if (px > 159u) px = 159u;
        uint32_t y = sl % 192u;

        /* Initialize scanline state if needed */
        if (!a->vis[y].valid) {
            if (y > 0 && a->vis[y - 1].valid) {
                a->vis[y] = a->vis[y - 1];
            } else {
                a->vis[y].colup0 = a->tia[0x06];
                a->vis[y].colup1 = a->tia[0x07];
                a->vis[y].colupf = a->tia[0x08];
                a->vis[y].colubk = a->tia[0x09];
                a->vis[y].pf0 = a->tia[0x0D];
                a->vis[y].pf1 = a->tia[0x0E];
                a->vis[y].pf2 = a->tia[0x0F];
                a->vis[y].ctrlpf = a->tia[0x0A];
                a->vis[y].grp0 = a->tia[0x1B];
                a->vis[y].grp1 = a->tia[0x1C];
                a->vis[y].refp0 = a->tia[0x0B];
                a->vis[y].refp1 = a->tia[0x0C];
                a->vis[y].nusiz0 = a->tia[0x04];
                a->vis[y].nusiz1 = a->tia[0x05];
            }
            a->vis[y].valid = 1;
        }

        /* Update based on register */
        switch (reg) {
            /* Colors */
            case 0x06: a->vis[y].colup0 = v; break;
            case 0x07: a->vis[y].colup1 = v; break;
            case 0x08: a->vis[y].colupf = v; break;
            case 0x09: a->vis[y].colubk = v; break;
            
            /* Playfield */
            case 0x0D: a->vis[y].pf0 = v; break;
            case 0x0E: a->vis[y].pf1 = v; break;
            case 0x0F: a->vis[y].pf2 = v; break;
            case 0x0A: a->vis[y].ctrlpf = v; break;
            
            /* Player 0 */
            case 0x04: a->vis[y].nusiz0 = v; break;
            case 0x0B: a->vis[y].refp0 = v; break;
            case 0x10: a->vis[y].x0 = (uint8_t)px; break;
            case 0x1B: a->vis[y].grp0 = v; break;
            
            /* Player 1 */
            case 0x05: a->vis[y].nusiz1 = v; break;
            case 0x0C: a->vis[y].refp1 = v; break;
            case 0x11: a->vis[y].x1 = (uint8_t)px; break;
            case 0x1C: a->vis[y].grp1 = v; break;
            
            /* Missiles */
            case 0x12: a->vis[y].m0_x = (uint8_t)px; break;
            case 0x13: a->vis[y].m1_x = (uint8_t)px; break;
            case 0x1D: a->vis[y].m0_grp = v; break;
            case 0x1E: a->vis[y].m1_grp = v; break;
            
            /* Ball */
            case 0x14: a->vis[y].ball_x = (uint8_t)px; break;
            case 0x1F: a->vis[y].ball_grp = v; break;
            
            /* Clear collisions */
            case 0x2C:
                a->cxm0p = a->cxm1p = 0;
                a->cxp0fb = a->cxp1fb = 0;
                a->cxm0fb = a->cxm1fb = 0;
                a->cxblpf = a->cxppmm = 0;
                break;
        }
        return;
    }

    if (addr >= 0x80 && addr <= 0xFF) {
        a->ram[addr & 0x7F] = v;
        return;
    }

    if ((addr & 0x0F80) == 0x0280) {
        uint8_t reg = (uint8_t)(addr & 0x1F);
        a->riot[reg] = v;
        
        /* Timer writes */
        if (reg >= 0x14 && reg <= 0x17) {
            a->timer.timer = v;
            a->timer.last_cycle = a->cpu.cycles;
            a->timer.underflow = 0;
            
            switch (reg) {
                case 0x14: a->timer.interval = 1; break;
                case 0x15: a->timer.interval = 8; break;
                case 0x16: a->timer.interval = 64; break;
                case 0x17: a->timer.interval = 1024; break;
            }
        }
        return;
    }
}

/* Enhanced TIA color conversion */
static inline uint32_t tia_color_to_xrgb(uint8_t c) {
    uint32_t hue = (uint32_t)(c >> 4) & 0x0Fu;
    uint32_t luma = ((uint32_t)c >> 1) & 0x07u;

    uint32_t v = 32u + luma * 28u;
    uint32_t s = (luma <= 1) ? 80u : 200u;

    uint32_t r=0, g=0, b=0;
    switch (hue) {
        case 0:  r=v; g=v/4; b=0; break;
        case 1:  r=v; g=v/2; b=0; break;
        case 2:  r=v; g=v; b=0; break;
        case 3:  r=v*3/4; g=v; b=0; break;
        case 4:  r=0; g=v; b=0; break;
        case 5:  r=0; g=v; b=v/2; break;
        case 6:  r=0; g=v; b=v; break;
        case 7:  r=0; g=v*3/4; b=v; break;
        case 8:  r=0; g=0; b=v; break;
        case 9:  r=v/2; g=0; b=v; break;
        case 10: r=v; g=0; b=v; break;
        case 11: r=v; g=0; b=v*3/4; break;
        case 12: r=v; g=0; b=0; break;
        case 13: r=v; g=v/4; b=v/8; break;
        case 14: r=v*3/4; g=v/8; b=0; break;
        default: r=v/2; g=v/4; b=0; break;
    }

    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;

    return ((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) | (b & 0xFFu);
}

/* Full rendering with playfield, sprites, missiles, ball */
void a2600_render_full(const a2600_t *a, emu_frame_t *frame) {
    if (!a || !frame || !frame->pixels || frame->fmt != (uint32_t)MD64API_GRP_FMT_XRGB8888) return;

    uint32_t pitch_words = frame->pitch_bytes / 4u;
    uint32_t *dst = (uint32_t*)frame->pixels;

    for (uint32_t y = 0; y < 192; y++) {
        uint32_t *row = dst + y * pitch_words;
        
        /* Get scanline state */
        uint32_t bg = tia_color_to_xrgb(a->vis[y].colubk);
        uint32_t fg_pf = tia_color_to_xrgb(a->vis[y].colupf);
        uint32_t col0 = tia_color_to_xrgb(a->vis[y].colup0);
        uint32_t col1 = tia_color_to_xrgb(a->vis[y].colup1);
        
        int reflect = (a->vis[y].ctrlpf & 0x01) ? 1 : 0;
        int priority = (a->vis[y].ctrlpf & 0x04) ? 1 : 0;
        
        for (uint32_t x = 0; x < 160; x++) {
            /* Playfield */
            int pf_bit = 0;
            {
                int half = (x >= 80);
                int x_in_half = (int)(x % 80);
                int bit = x_in_half / 4;
                int pf_idx = bit;
                
                if (half) {
                    pf_idx = reflect ? (19 - bit) : bit;
                }
                
                if (pf_idx < 4) {
                    int b = 7 - pf_idx;
                    pf_bit = (a->vis[y].pf0 >> b) & 1;
                } else if (pf_idx < 12) {
                    int b = 11 - pf_idx;
                    pf_bit = (a->vis[y].pf1 >> b) & 1;
                } else {
                    int b = pf_idx - 12;
                    pf_bit = (a->vis[y].pf2 >> b) & 1;
                }
            }
            
            uint32_t color = bg;
            int drawn = 0;
            
            /* Priority mode */
            if (priority && pf_bit) {
                color = fg_pf;
                drawn = 1;
            }
            
            /* Ball */
            if (!drawn && a->vis[y].ball_grp) {
                int ball_size = (a->vis[y].ctrlpf >> 4) & 3;
                int ball_width = 1 << ball_size;
                int dx = (int)x - (int)a->vis[y].ball_x;
                if (dx >= 0 && dx < ball_width) {
                    color = fg_pf;
                    drawn = 1;
                }
            }
            
            /* Missiles */
            if (!drawn) {
                int m0_size = (a->vis[y].nusiz0 >> 4) & 3;
                int m0_width = 1 << m0_size;
                int dx0 = (int)x - (int)a->vis[y].m0_x;
                if (a->vis[y].m0_grp && dx0 >= 0 && dx0 < m0_width) {
                    color = col0;
                    drawn = 1;
                }
                
                int m1_size = (a->vis[y].nusiz1 >> 4) & 3;
                int m1_width = 1 << m1_size;
                int dx1 = (int)x - (int)a->vis[y].m1_x;
                if (a->vis[y].m1_grp && dx1 >= 0 && dx1 < m1_width) {
                    color = col1;
                    drawn = 1;
                }
            }
            
            /* Player sprites */
            if (!drawn) {
                /* P0 */
                for (uint32_t i = 0; i < 8; i++) {
                    uint32_t bit = (a->vis[y].refp0 & 0x08) ? (1u << i) : (1u << (7u - i));
                    if (a->vis[y].grp0 & bit) {
                        uint32_t px = (uint32_t)a->vis[y].x0 + i;
                        if (px == x) {
                            color = col0;
                            drawn = 1;
                            break;
                        }
                    }
                }
                
                /* P1 */
                if (!drawn) {
                    for (uint32_t i = 0; i < 8; i++) {
                        uint32_t bit = (a->vis[y].refp1 & 0x08) ? (1u << i) : (1u << (7u - i));
                        if (a->vis[y].grp1 & bit) {
                            uint32_t px = (uint32_t)a->vis[y].x1 + i;
                            if (px == x) {
                                color = col1;
                                drawn = 1;
                                break;
                            }
                        }
                    }
                }
            }
            
            /* Non-priority playfield */
            if (!drawn && !priority && pf_bit) {
                color = fg_pf;
            }
            
            row[x] = color;
        }
    }
}