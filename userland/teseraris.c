/*
 * Teseraris - A falling blocks puzzle game
 * 
 * LEGAL NOTICE:
 * This is an original implementation of a falling blocks puzzle game.
 * The name "Teseraris" is derived from Greek "tessera" (four) and does not
 * infringe on any trademarks. The game mechanics (falling blocks that clear
 * lines) are not copyrightable game mechanics dating back to the 1980s.
 * 
 * This implementation is completely original code written for ModuOS.
 */

#include "libc.h"
#include "NodGL.h"
#include "string.h"
#include "lib_fnt.h"
#include "../include/moduos/kernel/events/events.h"

#define BOARD_WIDTH 10
#define BOARD_HEIGHT 20
#define BLOCK_SIZE 24
#define OFFSET_X 80
#define OFFSET_Y 40

// Piece shapes (4x4 grid, 1=filled, 0=empty)
static const int PIECES[7][4][4] = {
    // I piece
    {{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}},
    // O piece
    {{0,0,0,0},{0,1,1,0},{0,1,1,0},{0,0,0,0}},
    // T piece
    {{0,0,0,0},{0,1,0,0},{1,1,1,0},{0,0,0,0}},
    // S piece
    {{0,0,0,0},{0,1,1,0},{1,1,0,0},{0,0,0,0}},
    // Z piece
    {{0,0,0,0},{1,1,0,0},{0,1,1,0},{0,0,0,0}},
    // J piece
    {{0,0,0,0},{1,0,0,0},{1,1,1,0},{0,0,0,0}},
    // L piece
    {{0,0,0,0},{0,0,1,0},{1,1,1,0},{0,0,0,0}}
};

typedef enum {
    THEME_CLASSIC,
    THEME_NEON,
    THEME_PASTEL,
    THEME_RETRO,
    THEME_OCEAN,
    THEME_RUSSIA,
    THEME_CHINA,
    THEME_JAPAN,
    THEME_EUROPE,
    THEME_NORTH_AMERICA,
    THEME_SOUTH_AMERICA,
    THEME_TEXTMODE,
    THEME_COUNT
} Theme;

static const uint32_t THEME_COLORS[THEME_COUNT][8] = {
    // THEME_CLASSIC
    {
        0xFF1A1A1A, // Empty
        0xFF00F0F0, // I (Cyan)
        0xFFF0F000, // O (Yellow)
        0xFFA000F0, // T (Purple)
        0xFF00F050, // S (Green)
        0xFFF00000, // Z (Red)
        0xFF0050F0, // J (Blue)
        0xFFF0A000  // L (Orange)
    },
    // THEME_NEON
    {
        0xFF0A0A0A, // Empty
        0xFF00FFFF, // I (Bright Cyan)
        0xFFFFFF00, // O (Bright Yellow)
        0xFFFF00FF, // T (Bright Magenta)
        0xFF00FF00, // S (Bright Green)
        0xFFFF0000, // Z (Bright Red)
        0xFF0080FF, // J (Bright Blue)
        0xFFFF8000  // L (Bright Orange)
    },
    // THEME_PASTEL
    {
        0xFF252525, // Empty
        0xFFAAE6FF, // I (Pastel Cyan)
        0xFFFFF4AA, // O (Pastel Yellow)
        0xFFE6AAFF, // T (Pastel Purple)
        0xFFAAFFAA, // S (Pastel Green)
        0xFFFFAAAA, // Z (Pastel Red)
        0xFFAAAAFF, // J (Pastel Blue)
        0xFFFFD4AA  // L (Pastel Orange)
    },
    // THEME_RETRO
    {
        0xFF101010, // Empty
        0xFF00AAAA, // I (CGA Cyan)
        0xFFAAAA00, // O (CGA Yellow)
        0xFFAA00AA, // T (CGA Magenta)
        0xFF00AA00, // S (CGA Green)
        0xFFAA0000, // Z (CGA Red)
        0xFF0000AA, // J (CGA Blue)
        0xFFAA5500  // L (CGA Brown)
    },
    // THEME_OCEAN
    {
        0xFF0A1A2A, // Empty
        0xFF00D0FF, // I (Sky Blue)
        0xFFFFD700, // O (Gold)
        0xFF9370DB, // T (Medium Purple)
        0xFF20B2AA, // S (Light Sea Green)
        0xFFFF6B6B, // Z (Coral)
        0xFF4169E1, // J (Royal Blue)
        0xFFFF8C42  // L (Mango)
    },
    // THEME_RUSSIA
    {
        0xFF1A1A1A, // Empty
        0xFFFF0000, // I (Red)
        0xFFFFFFFF, // O (White)
        0xFF0039A6, // T (Blue - Russian flag)
        0xFFFFD700, // S (Gold)
        0xFFFF0000, // Z (Red)
        0xFF0039A6, // J (Blue)
        0xFFFFFFFF  // L (White)
    },
    // THEME_CHINA
    {
        0xFF1A0A0A, // Empty
        0xFFFF0000, // I (Red)
        0xFFFFFF00, // O (Yellow)
        0xFFFF0000, // T (Red - China flag)
        0xFFFFFF00, // S (Yellow - stars)
        0xFFFF4500, // Z (Orange-Red)
        0xFFFFD700, // J (Gold)
        0xFFFF0000  // L (Red)
    },
    // THEME_JAPAN
    {
        0xFF1A1A1A, // Empty
        0xFFFF1744, // I (Deep Pink - Cherry Blossom)
        0xFFFFFFFF, // O (White)
        0xFFBC002D, // T (Japanese Red)
        0xFFFFB7C5, // S (Sakura Pink)
        0xFFBC002D, // Z (Japanese Red)
        0xFF4A148C, // J (Purple)
        0xFFFFFFFF  // L (White)
    },
    // THEME_EUROPE
    {
        0xFF1A1A1A, // Empty
        0xFF003399, // I (EU Blue)
        0xFFFFCC00, // O (EU Gold/Yellow)
        0xFF00247D, // T (Royal Blue)
        0xFF009246, // S (Italian Green)
        0xFFCE1126, // Z (Red)
        0xFF003399, // J (Blue)
        0xFFFFCC00  // L (Gold)
    },
    // THEME_NORTH_AMERICA
    {
        0xFF1A1A1A, // Empty
        0xFF0052A5, // I (USA Blue)
        0xFFFFFFFF, // O (White)
        0xFFE4002B, // T (USA Red)
        0xFF0052A5, // S (Blue)
        0xFFE4002B, // Z (Red)
        0xFF0052A5, // J (Blue)
        0xFFFFFFFF  // L (White - stars)
    },
    // THEME_SOUTH_AMERICA
    {
        0xFF0A1A0A, // Empty
        0xFF009B3A, // I (Brazil Green)
        0xFFFFDF00, // O (Brazil Yellow)
        0xFF002776, // T (Brazil Blue)
        0xFF009B3A, // S (Green)
        0xFFFF0000, // Z (Red)
        0xFF002776, // J (Blue)
        0xFFFFDF00  // L (Yellow)
    },
    // THEME_TEXTMODE - Original Tetris monochrome style
    {
        0xFF000000, // Empty (Black)
        0xFFCCCCCC, // I (Light Gray)
        0xFFCCCCCC, // O (Light Gray)
        0xFFCCCCCC, // T (Light Gray)
        0xFFCCCCCC, // S (Light Gray)
        0xFFCCCCCC, // Z (Light Gray)
        0xFFCCCCCC, // J (Light Gray)
        0xFFCCCCCC  // L (Light Gray)
    }
};

static const char* THEME_NAMES[THEME_COUNT] = {
    "Classic",
    "Neon",
    "Pastel",
    "Retro",
    "Ocean",
    "Russia",
    "China",
    "Japan",
    "Europe",
    "North America",
    "South America",
    "Textmode"
};

static Theme current_theme = THEME_CLASSIC;

#define COLORS (THEME_COLORS[current_theme])

typedef struct {
    md64api_grp_video_info_t vi;
    uint8_t *bb;
    uint32_t bb_pitch;
    uint32_t fmt;
} Gfx;

static fnt_font_t *game_font = NULL;

typedef enum {
    STATE_MENU,
    STATE_LOBBY,
    STATE_PLAYING,
    STATE_GAME_OVER,
    STATE_PAUSED
} GameState;

typedef enum {
    MODE_CLASSIC,
    MODE_SPEED_RUN,
    MODE_ZEN,
    MODE_MARATHON,
    MODE_SPRINT,
    MODE_INVISIBLE,
    MODE_GIANT,
    MODE_CHAOS,
    MODE_MULTIPLAYER,
    MODE_COUNT
} GameMode;

static const char* MODE_NAMES[MODE_COUNT] = {
    "Classic",
    "Speed Run",
    "Zen Mode",
    "Marathon",
    "Sprint",
    "Invisible",
    "Giant",
    "Chaos Storm",
    "Multiplayer VS"
};

static const char* MODE_DESCRIPTIONS[MODE_COUNT] = {
    "Standard gameplay",
    "Speed increases rapidly",
    "Slow, relaxing pace",
    "Clear 150 lines to win",
    "Clear 40 lines ASAP",
    "Pieces vanish after placement",
    "Larger 15x25 board",
    "Random effects every 10 seconds!",
    "2-Player competitive battle!"
};

typedef enum {
    MENU_TAB_MAIN,
    MENU_TAB_MODES,
    MENU_TAB_CHALLENGES,
    MENU_TAB_SETTINGS,
    MENU_TAB_CONTROLS,
    MENU_TAB_COUNT
} MenuTab;

static MenuTab current_menu_tab = MENU_TAB_MAIN;

typedef enum {
    CHALLENGE_NONE,
    CHALLENGE_SPEED_DEMON,
    CHALLENGE_PERFECTIONIST,
    CHALLENGE_MINIMALIST,
    CHALLENGE_ENDURANCE,
    CHALLENGE_COUNT
} Challenge;

static Challenge current_challenge = CHALLENGE_NONE;

static const char* CHALLENGE_NAMES[CHALLENGE_COUNT] = {
    "None",
    "Speed Demon",
    "Perfectionist",
    "Minimalist",
    "Endurance Run"
};

static const char* CHALLENGE_DESCRIPTIONS[CHALLENGE_COUNT] = {
    "No active challenge",
    "Clear 100 lines in 5 minutes",
    "No mistakes - game over on bad placement",
    "Use only 3 pieces total (I, O, T)",
    "Survive 30 minutes without game over"
};

// Player state structure for multiplayer
typedef struct {
    float x, y;
    float vx, vy;
    int life;
    uint32_t color;
} Particle;

#define MAX_PARTICLES 200

// Simple math approximations
static float cosf_approx(float x) { return 1.0f - x*x/2.0f; }
static float sinf_approx(float x) { return x; }


typedef struct {
    int board[BOARD_HEIGHT][BOARD_WIDTH];
    int current_piece;
    int current_rotation;
    int current_x, current_y;
    int next_piece;
    int score;
    int lines_cleared;
    int game_over;
    int held_piece;
    int can_hold;
    int combo_count;
    int level;
    int lines_for_next_level;
    int pending_garbage;
    Particle particles[MAX_PARTICLES];
    int attack_flash;
    int receive_flash;
    int shake_x, shake_y;
} PlayerState;

// Forward declarations for multiplayer particle functions
void spawn_particles(PlayerState *p, int x, int y, uint32_t color, int count);
void update_particles(PlayerState *p);
void draw_particles(Gfx *g, PlayerState *p, int offset_x, int offset_y);

// Single player state (kept for compatibility)
static int board[BOARD_HEIGHT][BOARD_WIDTH];
static int current_piece;
static int current_rotation;
static int current_x, current_y;
static int next_piece;
static int score;
static int lines_cleared;
static int game_over;
static GameState game_state = STATE_MENU;

static int held_piece = -1;
static int can_hold = 1;
static int combo_count = 0;
static int level = 1;
static int lines_for_next_level = 10;
static GameMode current_mode = MODE_CLASSIC;
static int selected_mode = 0;
static int selected_theme = 0;
static int selected_challenge = 0;

#define MAX_PLAYERS 4

// Multiplayer states
static PlayerState players[MAX_PLAYERS];
static int num_players = 2; // Default 2 players
static int mp_winner = 0; // 0=none, 1-4=player number
static char player_names[MAX_PLAYERS][32] = {
    "Player 1", "Player 2", "Player 3", "Player 4"
};

// Compatibility aliases for existing 2-player code
#define player1 players[0]
#define player2 players[1]
#define player1_name player_names[0]
#define player2_name player_names[1]



static int chaos_timer = 0;
static int chaos_effect = 0;
static int gravity_reversed = 0;
static int pieces_invisible = 0;

static int sprint_lines_goal = 40;
static uint64_t sprint_start_time = 0;
static int marathon_lines_goal = 150;
static int game_won = 0;

#define MAX_MENU_PIECES 15
typedef struct {
    int piece_type;
    int rotation;
    float x;
    float y;
    float speed;
    int active;
} MenuPiece;

static MenuPiece menu_pieces[MAX_MENU_PIECES];

static uint32_t rng_seed = 1;
static uint32_t rnd_u32(void) {
    rng_seed = (rng_seed * 1103515245u + 12345u) & 0x7fffffffu;
    return rng_seed;
}

static uint32_t xrgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t rr = (uint16_t)((r * 31u) / 255u);
    uint16_t gg = (uint16_t)((g * 63u) / 255u);
    uint16_t bb = (uint16_t)((b * 31u) / 255u);
    return (uint16_t)((rr << 11) | (gg << 5) | bb);
}

static NodGL_Device g_device = NULL;
static NodGL_Context g_ctx = NULL;
static NodGL_Texture g_backbuffer_tex = 0;

int gfx_init(Gfx *g) {
    memset(g, 0, sizeof(*g));
    
    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &g_device, &g_ctx, NULL) != NodGL_OK) return -1;

    uint32_t screen_w, screen_h;
    NodGL_GetScreenResolution(g_device, &screen_w, &screen_h);

    g->vi.width = screen_w;
    g->vi.height = screen_h;
    g->vi.bpp = 32;
    g->fmt = MD64API_GRP_FMT_XRGB8888;

    NodGL_TextureDesc tex_desc = {
        .width = screen_w,
        .height = screen_h,
        .format = NodGL_FORMAT_R8G8B8A8_UNORM,
        .mip_levels = 1,
        .initial_data = NULL,
        .initial_data_size = 0
    };

    if (NodGL_CreateTexture(g_device, &tex_desc, &g_backbuffer_tex) != NodGL_OK) {
        NodGL_ReleaseDevice(g_device);
        return -2;
    }

    if (NodGL_MapResource(g_ctx, g_backbuffer_tex, (void**)&g->bb, &g->bb_pitch) != NodGL_OK) {
        NodGL_ReleaseResource(g_device, g_backbuffer_tex);
        NodGL_ReleaseDevice(g_device);
        return -4;
    }
    
    return 0;
}

void gfx_fill_rect(Gfx *g, int x, int y, int w, int h, uint32_t argb) {
    if (!g || !g->bb) return;
    if (x < 0 || y < 0) return;
    
    uint32_t ux = (uint32_t)x;
    uint32_t uy = (uint32_t)y;
    uint32_t uw = (uint32_t)w;
    uint32_t uh = (uint32_t)h;
    
    if (ux >= g->vi.width || uy >= g->vi.height) return;
    if (ux + uw > g->vi.width) uw = g->vi.width - ux;
    if (uy + uh > g->vi.height) uh = g->vi.height - uy;
    
    if (g->fmt == MD64API_GRP_FMT_XRGB8888 && g->vi.bpp == 32) {
        for (uint32_t yy = 0; yy < uh; yy++) {
            uint32_t *row = (uint32_t*)(g->bb + (uint64_t)(uy + yy) * g->bb_pitch);
            for (uint32_t xx = 0; xx < uw; xx++) {
                row[ux + xx] = argb;
            }
        }
    } else if (g->fmt == MD64API_GRP_FMT_RGB565 && g->vi.bpp == 16) {
        uint8_t r = (uint8_t)((argb >> 16) & 0xFF);
        uint8_t gr = (uint8_t)((argb >> 8) & 0xFF);
        uint8_t b = (uint8_t)(argb & 0xFF);
        uint16_t c565 = rgb565(r, gr, b);
        for (uint32_t yy = 0; yy < uh; yy++) {
            uint16_t *row = (uint16_t*)(g->bb + (uint64_t)(uy + yy) * g->bb_pitch);
            for (uint32_t xx = 0; xx < uw; xx++) {
                row[ux + xx] = c565;
            }
        }
    }
}

void gfx_present_full(Gfx *g) {
    if (!g || !g->bb) return;
    NodGL_DrawTexture(g_ctx, g_backbuffer_tex, 0, 0, 0, 0, g->vi.width, g->vi.height);
    NodGL_PresentContext(g_ctx, 0);
}

// Game logic
void rotate_piece(int piece[4][4], int rotated[4][4]) {
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            rotated[x][3-y] = piece[y][x];
        }
    }
}

int check_collision(int piece_type, int rotation, int px, int py) {
    int temp[4][4];
    memcpy(temp, PIECES[piece_type], sizeof(temp));
    
    // Rotate temp piece
    for (int r = 0; r < rotation; r++) {
        int rotated[4][4];
        rotate_piece(temp, rotated);
        memcpy(temp, rotated, sizeof(temp));
    }
    
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (temp[y][x]) {
                int bx = px + x;
                int by = py + y;
                if (bx < 0 || bx >= BOARD_WIDTH || by >= BOARD_HEIGHT) return 1;
                if (by >= 0 && board[by][bx]) return 1;
            }
        }
    }
    return 0;
}

// Multiplayer version of collision check
int mp_check_collision(PlayerState *p, int piece_type, int rotation, int px, int py) {
    int temp[4][4];
    memcpy(temp, PIECES[piece_type], sizeof(temp));
    
    for (int r = 0; r < rotation; r++) {
        int rotated[4][4];
        rotate_piece(temp, rotated);
        memcpy(temp, rotated, sizeof(temp));
    }
    
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (temp[y][x]) {
                int bx = px + x;
                int by = py + y;
                if (bx < 0 || bx >= BOARD_WIDTH || by >= BOARD_HEIGHT) return 1;
                if (by >= 0 && p->board[by][bx]) return 1;
            }
        }
    }
    return 0;
}

void lock_piece() {
    int temp[4][4];
    memcpy(temp, PIECES[current_piece], sizeof(temp));
    
    for (int r = 0; r < current_rotation; r++) {
        int rotated[4][4];
        rotate_piece(temp, rotated);
        memcpy(temp, rotated, sizeof(temp));
    }
    
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (temp[y][x]) {
                int bx = current_x + x;
                int by = current_y + y;
                if (by >= 0 && by < BOARD_HEIGHT && bx >= 0 && bx < BOARD_WIDTH) {
                    board[by][bx] = current_piece + 1;
                }
            }
        }
    }
}

// Multiplayer version of lock piece
void mp_lock_piece(PlayerState *p) {
    int temp[4][4];
    memcpy(temp, PIECES[p->current_piece], sizeof(temp));
    
    for (int r = 0; r < p->current_rotation; r++) {
        int rotated[4][4];
        rotate_piece(temp, rotated);
        memcpy(temp, rotated, sizeof(temp));
    }
    
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (temp[y][x]) {
                int bx = p->current_x + x;
                int by = p->current_y + y;
                if (by >= 0 && by < BOARD_HEIGHT && bx >= 0 && bx < BOARD_WIDTH) {
                    p->board[by][bx] = p->current_piece + 1;
                }
            }
        }
    }
}

int clear_lines() {
    int cleared = 0;
    for (int y = BOARD_HEIGHT - 1; y >= 0; y--) {
        int full = 1;
        for (int x = 0; x < BOARD_WIDTH; x++) {
            if (!board[y][x]) { full = 0; break; }
        }
        if (full) {
            cleared++;
            for (int yy = y; yy > 0; yy--) {
                for (int x = 0; x < BOARD_WIDTH; x++) {
                    board[yy][x] = board[yy-1][x];
                }
            }
            for (int x = 0; x < BOARD_WIDTH; x++) {
                board[0][x] = 0;
            }
            y++;
        }
    }
    
    if (cleared > 0) {
        combo_count++;
    } else {
        combo_count = 0;
    }
    
    return cleared;
}

// Multiplayer version of clear lines
int mp_clear_lines(PlayerState *p, PlayerState *opponent) {
    int cleared = 0;
    for (int y = BOARD_HEIGHT - 1; y >= 0; y--) {
        int full = 1;
        for (int x = 0; x < BOARD_WIDTH; x++) {
            if (!p->board[y][x]) { full = 0; break; }
        }
        if (full) {
            // Spawn particles for cleared line
            for(int x = 0; x < BOARD_WIDTH; x++) {
                if (p->board[y][x] > 0 && p->board[y][x] <= 7) {
                    uint32_t color = THEME_COLORS[current_theme][p->board[y][x] - 1];
                    spawn_particles(p, x * BLOCK_SIZE + BLOCK_SIZE/2, 
                                   y * BLOCK_SIZE + BLOCK_SIZE/2, color, 3);
                }
            }
            
            cleared++;
            for (int yy = y; yy > 0; yy--) {
                for (int x = 0; x < BOARD_WIDTH; x++) {
                    p->board[yy][x] = p->board[yy-1][x];
                }
            }
            for (int x = 0; x < BOARD_WIDTH; x++) {
                p->board[0][x] = 0;
            }
            y++;
        }
    }
    
    if (cleared > 0) {
        p->combo_count++;
        
        // Send garbage to opponent with visual effects
        if (cleared >= 2 && opponent) {
            int garbage = cleared - 1;
            if (cleared == 4) garbage = 3; // Tetris bonus
            opponent->pending_garbage += garbage;
            
            // Attack effects
            p->attack_flash = 15;
            opponent->receive_flash = 15;
            opponent->shake_x = (rnd_u32() % 5) - 2;
            opponent->shake_y = (rnd_u32() % 5) - 2;
        }
    } else {
        p->combo_count = 0;
    }
    
    return cleared;
}

// Add garbage lines to a player's board
void mp_add_garbage(PlayerState *p, int lines) {
    if (lines <= 0) return;
    
    // Move all rows up
    for (int y = 0; y < BOARD_HEIGHT - lines; y++) {
        for (int x = 0; x < BOARD_WIDTH; x++) {
            p->board[y][x] = p->board[y + lines][x];
        }
    }
    
    // Add garbage rows at bottom with one random gap
    for (int i = 0; i < lines; i++) {
        int gap = rnd_u32() % BOARD_WIDTH;
        for (int x = 0; x < BOARD_WIDTH; x++) {
            p->board[BOARD_HEIGHT - lines + i][x] = (x == gap) ? 0 : 8; // 8 = gray garbage block
        }
    }
}

void spawn_piece() {
    current_piece = next_piece;
    next_piece = rnd_u32() % 7;
    current_rotation = 0;
    current_x = BOARD_WIDTH / 2 - 2;
    current_y = 0;
    can_hold = 1;
    
    if (check_collision(current_piece, current_rotation, current_x, current_y)) {
        game_over = 1;
    }
}

// Multiplayer version of spawn piece
void mp_spawn_piece(PlayerState *p) {
    p->current_piece = p->next_piece;
    p->next_piece = rnd_u32() % 7;
    p->current_rotation = 0;
    p->current_x = BOARD_WIDTH / 2 - 2;
    p->current_y = 0;
    p->can_hold = 1;
    
    // Add any pending garbage lines
    if (p->pending_garbage > 0) {
        mp_add_garbage(p, p->pending_garbage);
        p->pending_garbage = 0;
    }
    
    if (mp_check_collision(p, p->current_piece, p->current_rotation, p->current_x, p->current_y)) {
        p->game_over = 1;
    }
}

void hold_piece() {
    if (!can_hold) return;
    
    if (held_piece == -1) {
        held_piece = current_piece;
        spawn_piece();
    } else {
        int temp = current_piece;
        current_piece = held_piece;
        held_piece = temp;
        current_rotation = 0;
        current_x = BOARD_WIDTH / 2 - 2;
        current_y = 0;
        
        if (check_collision(current_piece, current_rotation, current_x, current_y)) {
            game_over = 1;
        }
    }
    
    can_hold = 0;
}

// Multiplayer version of hold piece
void mp_hold_piece(PlayerState *p) {
    if (!p->can_hold) return;
    
    if (p->held_piece == -1) {
        p->held_piece = p->current_piece;
        mp_spawn_piece(p);
    } else {
        int temp = p->current_piece;
        p->current_piece = p->held_piece;
        p->held_piece = temp;
        p->current_rotation = 0;
        p->current_x = BOARD_WIDTH / 2 - 2;
        p->current_y = 0;
        
        if (mp_check_collision(p, p->current_piece, p->current_rotation, p->current_x, p->current_y)) {
            p->game_over = 1;
        }
    }
    
    p->can_hold = 0;
}

int get_ghost_y() {
    int ghost_y = current_y;
    while (!check_collision(current_piece, current_rotation, current_x, ghost_y + 1)) {
        ghost_y++;
    }
    return ghost_y;
}

// Multiplayer version of get ghost y
int mp_get_ghost_y(PlayerState *p) {
    int ghost_y = p->current_y;
    while (!mp_check_collision(p, p->current_piece, p->current_rotation, p->current_x, ghost_y + 1)) {
        ghost_y++;
    }
    return ghost_y;
}

void draw_block(Gfx *g, int x, int y, uint32_t color) {
    int px = OFFSET_X + x * BLOCK_SIZE;
    int py = OFFSET_Y + y * BLOCK_SIZE;
    
    if (current_theme == THEME_TEXTMODE) {
        // Textmode: ASCII-style blocks with thick borders
        if (color == COLORS[0]) {
            // Empty cell - just black
            gfx_fill_rect(g, px, py, BLOCK_SIZE, BLOCK_SIZE, 0xFF000000);
        } else {
            // Filled cell - light gray with thick black border (like [])
            gfx_fill_rect(g, px, py, BLOCK_SIZE, BLOCK_SIZE, 0xFF000000);
            gfx_fill_rect(g, px + 2, py + 2, BLOCK_SIZE - 4, BLOCK_SIZE - 4, color);
            // Add inner border for ASCII effect
            gfx_fill_rect(g, px + 3, py + 3, BLOCK_SIZE - 6, BLOCK_SIZE - 6, 0xFF000000);
            gfx_fill_rect(g, px + 4, py + 4, BLOCK_SIZE - 8, BLOCK_SIZE - 8, color);
        }
        return;
    }
    
    if (color == COLORS[0]) {
        gfx_fill_rect(g, px, py, BLOCK_SIZE, BLOCK_SIZE, 0xFF0A0A0A);
        gfx_fill_rect(g, px + 1, py + 1, BLOCK_SIZE - 2, BLOCK_SIZE - 2, color);
        return;
    }
    
    gfx_fill_rect(g, px, py, BLOCK_SIZE, BLOCK_SIZE, 0xFF000000);
    gfx_fill_rect(g, px + 2, py + 2, BLOCK_SIZE - 4, BLOCK_SIZE - 4, color);
    
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t gr = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    
    uint32_t highlight = 0xFF000000 | 
                        (((r + 80) > 255 ? 255 : (r + 80)) << 16) |
                        (((gr + 80) > 255 ? 255 : (gr + 80)) << 8) |
                        ((b + 80) > 255 ? 255 : (b + 80));
    
    uint32_t shadow = 0xFF000000 | 
                     ((r > 60 ? (r - 60) : 0) << 16) |
                     ((gr > 60 ? (gr - 60) : 0) << 8) |
                     (b > 60 ? (b - 60) : 0);
    
    gfx_fill_rect(g, px + 2, py + 2, BLOCK_SIZE - 4, 3, highlight);
    gfx_fill_rect(g, px + 2, py + 2, 3, BLOCK_SIZE - 4, highlight);
    
    gfx_fill_rect(g, px + 2, py + BLOCK_SIZE - 5, BLOCK_SIZE - 4, 3, shadow);
    gfx_fill_rect(g, px + BLOCK_SIZE - 5, py + 2, 3, BLOCK_SIZE - 4, shadow);
}

void draw_mini_block(Gfx *g, int x, int y, uint32_t color) {
    gfx_fill_rect(g, x, y, 12, 12, 0xFF000000);
    gfx_fill_rect(g, x + 1, y + 1, 10, 10, color);
    
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t gr = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    
    uint32_t highlight = 0xFF000000 | 
                        (((r + 60) > 255 ? 255 : (r + 60)) << 16) |
                        (((gr + 60) > 255 ? 255 : (gr + 60)) << 8) |
                        ((b + 60) > 255 ? 255 : (b + 60));
    
    gfx_fill_rect(g, x + 1, y + 1, 10, 2, highlight);
    gfx_fill_rect(g, x + 1, y + 1, 2, 10, highlight);
}

void draw_text_char_scaled(Gfx *g, int x, int y, char c, uint32_t color, int scale) {
    if (!game_font) return;
    if (scale <= 0) scale = 1;
    
    fnt_glyph_t *glyph = fnt_get_glyph(game_font, (uint32_t)(unsigned char)c);
    if (!glyph) return;
    
    for (int dy = 0; dy < glyph->bitmap_height; dy++) {
        for (int dx = 0; dx < glyph->bitmap_width; dx++) {
            if (fnt_get_pixel(glyph, dx, dy)) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + dx * scale + sx;
                        int py = y + dy * scale + sy;
                        if (px >= 0 && py >= 0 && px < (int)g->vi.width && py < (int)g->vi.height) {
                            if (g->fmt == MD64API_GRP_FMT_XRGB8888 && g->vi.bpp == 32) {
                                uint32_t *row = (uint32_t*)(g->bb + (uint64_t)py * g->bb_pitch);
                                row[px] = color;
                            } else if (g->fmt == MD64API_GRP_FMT_RGB565 && g->vi.bpp == 16) {
                                uint8_t r = (uint8_t)((color >> 16) & 0xFF);
                                uint8_t gr = (uint8_t)((color >> 8) & 0xFF);
                                uint8_t b = (uint8_t)(color & 0xFF);
                                uint16_t c565 = rgb565(r, gr, b);
                                uint16_t *row = (uint16_t*)(g->bb + (uint64_t)py * g->bb_pitch);
                                row[px] = c565;
                            }
                        }
                    }
                }
            }
        }
    }
}

void draw_text_char(Gfx *g, int x, int y, char c, uint32_t color) {
    draw_text_char_scaled(g, x, y, c, color, 1);
}

void draw_text_scaled(Gfx *g, int x, int y, const char *str, uint32_t color, int scale) {
    if (!game_font) return;
    if (scale <= 0) scale = 1;
    
    int cx = x;
    while (*str) {
        fnt_glyph_t *glyph = fnt_get_glyph(game_font, (uint32_t)(unsigned char)*str);
        if (glyph) {
            draw_text_char_scaled(g, cx, y, *str, color, scale);
            cx += glyph->width * scale;
        }
        str++;
    }
}

void draw_text(Gfx *g, int x, int y, const char *str, uint32_t color) {
    draw_text_scaled(g, x, y, str, color, 1);
}

void draw_number(Gfx *g, int x, int y, int num, uint32_t color) {
    char buf[16];
    itoa(num, buf, 10);
    draw_text(g, x, y, buf, color);
}

void draw_number_scaled(Gfx *g, int x, int y, int num, uint32_t color, int scale) {
    char buf[16];
    itoa(num, buf, 10);
    draw_text_scaled(g, x, y, buf, color, scale);
}

void draw_next_piece(Gfx *g, int piece_type) {
    int base_x = OFFSET_X + BOARD_WIDTH * BLOCK_SIZE + 30;
    int base_y = OFFSET_Y + 60;
    
    draw_text(g, base_x, base_y - 25, "NEXT", 0xFF00FFFF);
    
    gfx_fill_rect(g, base_x - 8, base_y - 8, 76, 76, 0xFF404040);
    gfx_fill_rect(g, base_x - 6, base_y - 6, 72, 72, 0xFF000000);
    gfx_fill_rect(g, base_x - 5, base_y - 5, 70, 70, 0xFF0A0A0A);
    
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (PIECES[piece_type][y][x]) {
                int px = base_x + x * 12;
                int py = base_y + y * 12;
                draw_mini_block(g, px, py, COLORS[piece_type + 1]);
            }
        }
    }
}

void draw_held_piece(Gfx *g) {
    int base_x = OFFSET_X - 110;
    int base_y = OFFSET_Y + 60;
    
    draw_text(g, base_x, base_y - 25, "HOLD", 0xFF00FFFF);
    
    gfx_fill_rect(g, base_x - 8, base_y - 8, 76, 76, 0xFF404040);
    gfx_fill_rect(g, base_x - 6, base_y - 6, 72, 72, 0xFF000000);
    gfx_fill_rect(g, base_x - 5, base_y - 5, 70, 70, 0xFF0A0A0A);
    
    if (held_piece >= 0) {
        uint32_t hold_color = can_hold ? COLORS[held_piece + 1] : 0xFF404040;
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                if (PIECES[held_piece][y][x]) {
                    int px = base_x + x * 12;
                    int py = base_y + y * 12;
                    draw_mini_block(g, px, py, hold_color);
                }
            }
        }
    }
}

void draw_ui(Gfx *g) {
    int info_x = OFFSET_X + BOARD_WIDTH * BLOCK_SIZE + 30;
    int info_y = OFFSET_Y;
    
    draw_text_scaled(g, info_x - 5, info_y, "TESERARIS", 0xFF00FFFF, 2);
    
    gfx_fill_rect(g, info_x - 8, info_y + 160, 120, 50, 0xFF1A1A1A);
    gfx_fill_rect(g, info_x - 6, info_y + 162, 116, 46, 0xFF0A0A0A);
    draw_text(g, info_x, info_y + 168, "SCORE", 0xFF888888);
    draw_number_scaled(g, info_x + 5, info_y + 183, score, 0xFFFFDD00, 2);
    
    gfx_fill_rect(g, info_x - 8, info_y + 220, 120, 50, 0xFF1A1A1A);
    gfx_fill_rect(g, info_x - 6, info_y + 222, 116, 46, 0xFF0A0A0A);
    draw_text(g, info_x, info_y + 228, "LINES", 0xFF888888);
    draw_number_scaled(g, info_x + 5, info_y + 243, lines_cleared, 0xFFFFDD00, 2);
    
    gfx_fill_rect(g, info_x - 8, info_y + 280, 120, 50, 0xFF1A1A1A);
    gfx_fill_rect(g, info_x - 6, info_y + 282, 116, 46, 0xFF0A0A0A);
    draw_text(g, info_x, info_y + 288, "LEVEL", 0xFF888888);
    draw_number_scaled(g, info_x + 40, info_y + 298, level, 0xFFFF8800, 1);
    
    if (current_mode == MODE_SPRINT && !game_over) {
        uint64_t elapsed = (time_ms() - sprint_start_time) / 1000;
        char time_buf[32];
        itoa((int)elapsed, time_buf, 10);
        draw_text(g, info_x, info_y + 320, "TIME:", 0xFF888888);
        draw_text(g, info_x + 50, info_y + 320, time_buf, 0xFF00FFFF);
    } else if (current_mode == MODE_CHAOS && chaos_effect > 0) {
        const char* effects[] = {"", "GRAVITY!", "INVISIBLE!", "SPEED UP!"};
        draw_text(g, info_x, info_y + 320, effects[chaos_effect], 0xFFFF0000);
    }
    
    if (combo_count > 1) {
        int combo_x = OFFSET_X + (BOARD_WIDTH * BLOCK_SIZE) / 2 - 50;
        int combo_y = OFFSET_Y - 30;
        char combo_text[32];
        itoa(combo_count, combo_text, 10);
        strcat(combo_text, "x COMBO!");
        
        gfx_fill_rect(g, combo_x - 5, combo_y - 5, 110, 25, 0xFF000000);
        draw_text_scaled(g, combo_x, combo_y, combo_text, 0xFFFFFF00, 2);
    }
    
    int help_y = OFFSET_Y + BOARD_HEIGHT * BLOCK_SIZE + 15;
    gfx_fill_rect(g, OFFSET_X - 5, help_y - 5, BOARD_WIDTH * BLOCK_SIZE + 10, 55, 0xFF1A1A1A);
    gfx_fill_rect(g, OFFSET_X - 3, help_y - 3, BOARD_WIDTH * BLOCK_SIZE + 6, 51, 0xFF0A0A0A);
    
    draw_text(g, OFFSET_X + 5, help_y + 2, "CONTROLS", 0xFF00FFFF);
    draw_text(g, OFFSET_X + 5, help_y + 17, "WASD/ARROWS - Move  C - Hold", 0xFF888888);
    draw_text(g, OFFSET_X + 5, help_y + 32, "SPACE - Drop  ESC - Menu", 0xFF888888);
}

// Draw a player's board for multiplayer (with custom offset)
void draw_player_board(Gfx *g, PlayerState *p, int offset_x, int offset_y, const char *player_name, int show_controls) {
    // Draw border
    gfx_fill_rect(g, offset_x - 6, offset_y - 6, 
                  BOARD_WIDTH * BLOCK_SIZE + 12, 
                  BOARD_HEIGHT * BLOCK_SIZE + 12, 0xFF808080);
    gfx_fill_rect(g, offset_x - 4, offset_y - 4, 
                  BOARD_WIDTH * BLOCK_SIZE + 8, 
                  BOARD_HEIGHT * BLOCK_SIZE + 8, 0xFF000000);
    gfx_fill_rect(g, offset_x - 3, offset_y - 3, 
                  BOARD_WIDTH * BLOCK_SIZE + 6, 
                  BOARD_HEIGHT * BLOCK_SIZE + 6, 0xFF202020);
    
    // Draw board cells
    for (int y = 0; y < BOARD_HEIGHT; y++) {
        for (int x = 0; x < BOARD_WIDTH; x++) {
            int cell = p->board[y][x];
            int px = offset_x + x * BLOCK_SIZE;
            int py = offset_y + y * BLOCK_SIZE;
            
            uint32_t color = (cell == 8) ? 0xFF404040 : COLORS[cell]; // 8 = garbage
            
            if (color == COLORS[0]) {
                gfx_fill_rect(g, px, py, BLOCK_SIZE, BLOCK_SIZE, 0xFF0A0A0A);
                gfx_fill_rect(g, px + 1, py + 1, BLOCK_SIZE - 2, BLOCK_SIZE - 2, color);
            } else {
                gfx_fill_rect(g, px, py, BLOCK_SIZE, BLOCK_SIZE, 0xFF000000);
                gfx_fill_rect(g, px + 2, py + 2, BLOCK_SIZE - 4, BLOCK_SIZE - 4, color);
            }
        }
    }
    
    // Draw current piece if not game over
    if (!p->game_over) {
        int temp[4][4];
        memcpy(temp, PIECES[p->current_piece], sizeof(temp));
        
        for (int r = 0; r < p->current_rotation; r++) {
            int rotated[4][4];
            rotate_piece(temp, rotated);
            memcpy(temp, rotated, sizeof(temp));
        }
        
        // Draw ghost piece
        int ghost_y = mp_get_ghost_y(p);
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                if (temp[y][x] && ghost_y + y >= 0 && ghost_y != p->current_y) {
                    int px = offset_x + (p->current_x + x) * BLOCK_SIZE;
                    int py = offset_y + (ghost_y + y) * BLOCK_SIZE;
                    gfx_fill_rect(g, px + 2, py + 2, BLOCK_SIZE - 4, 2, 0xFF888888);
                    gfx_fill_rect(g, px + 2, py + BLOCK_SIZE - 4, BLOCK_SIZE - 4, 2, 0xFF888888);
                }
            }
        }
        
        // Draw actual piece
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                if (temp[y][x] && p->current_y + y >= 0) {
                    int px = offset_x + (p->current_x + x) * BLOCK_SIZE;
                    int py = offset_y + (p->current_y + y) * BLOCK_SIZE;
                    uint32_t color = COLORS[p->current_piece + 1];
                    gfx_fill_rect(g, px, py, BLOCK_SIZE, BLOCK_SIZE, 0xFF000000);
                    gfx_fill_rect(g, px + 2, py + 2, BLOCK_SIZE - 4, BLOCK_SIZE - 4, color);
                }
            }
        }
    }
    
    // Apply screen shake
    int shake_offset_x = offset_x + p->shake_x;
    int shake_offset_y = offset_y + p->shake_y;
    
    // Draw attack flash (green outline when sending garbage)
    if (p->attack_flash > 0) {
        int flash_alpha = (p->attack_flash * 255) / 15;
        uint32_t flash_color = 0x00FF00 | (flash_alpha << 24);
        for(int i = 0; i < 3; i++) {
            gfx_fill_rect(g, shake_offset_x - 3 - i, shake_offset_y - 3 - i, 
                         BOARD_WIDTH * BLOCK_SIZE + 6 + i*2, 3, flash_color);
            gfx_fill_rect(g, shake_offset_x - 3 - i, shake_offset_y + BOARD_HEIGHT * BLOCK_SIZE + i, 
                         BOARD_WIDTH * BLOCK_SIZE + 6 + i*2, 3, flash_color);
            gfx_fill_rect(g, shake_offset_x - 3 - i, shake_offset_y - 3 - i, 
                         3, BOARD_HEIGHT * BLOCK_SIZE + 6 + i*2, flash_color);
            gfx_fill_rect(g, shake_offset_x + BOARD_WIDTH * BLOCK_SIZE + i, shake_offset_y - 3 - i, 
                         3, BOARD_HEIGHT * BLOCK_SIZE + 6 + i*2, flash_color);
        }
    }
    
    // Draw receive flash (red overlay when receiving garbage)
    if (p->receive_flash > 0) {
        int flash_alpha = (p->receive_flash * 80) / 15;
        uint32_t flash_color = 0xFF0000 | (flash_alpha << 24);
        gfx_fill_rect(g, shake_offset_x, shake_offset_y, 
                     BOARD_WIDTH * BLOCK_SIZE, BOARD_HEIGHT * BLOCK_SIZE, flash_color);
    }
    
    // Draw particles
    for(int i = 0; i < MAX_PARTICLES; i++) {
        if(p->particles[i].life > 0) {
            int px = shake_offset_x + (int)p->particles[i].x;
            int py = shake_offset_y + (int)p->particles[i].y;
            int alpha = (p->particles[i].life * 255) / 50;
            uint32_t color = (p->particles[i].color & 0x00FFFFFF) | (alpha << 24);
            gfx_fill_rect(g, px, py, 3, 3, color);
        }
    }
    
    // Draw player name and stats with background
    gfx_fill_rect(g, offset_x - 5, offset_y - 50, BOARD_WIDTH * BLOCK_SIZE + 10, 12, 0xFF222222);
    draw_text(g, offset_x, offset_y - 45, player_name, 0xFF00FFFF);
    
    char score_buf[32];
    itoa(p->score, score_buf, 10);
    draw_text(g, offset_x, offset_y - 35, "Score: ", 0xFF888888);
    draw_text(g, offset_x + 50, offset_y - 35, score_buf, 0xFFFFDD00);
    
    // Draw combo counter
    if (p->combo_count > 1) {
        char combo_buf[32];
        itoa(p->combo_count, combo_buf, 10);
        draw_text(g, offset_x, offset_y - 25, "COMBO: ", 0xFFFF8800);
        draw_text(g, offset_x + 50, offset_y - 25, combo_buf, 0xFFFFFF00);
    }
    
    // Draw pending garbage warning
    if (p->pending_garbage > 0) {
        char garbage_buf[32];
        itoa(p->pending_garbage, garbage_buf, 10);
        draw_text(g, offset_x, offset_y - 15, "INCOMING: ", 0xFFFF0000);
        draw_text(g, offset_x + 70, offset_y - 15, garbage_buf, 0xFFFFFFFF);
    }
    
    // Draw additional stats
    char lines_buf[32], level_buf[32];
    itoa(p->lines_cleared, lines_buf, 10);
    itoa(p->level, level_buf, 10);
    
    int stats_y = offset_y + (BOARD_HEIGHT * BLOCK_SIZE) + 10;
    draw_text(g, offset_x, stats_y, "Lines: ", 0xFF888888);
    draw_text(g, offset_x + 45, stats_y, lines_buf, 0xFFFFFFFF);
    draw_text(g, offset_x + 80, stats_y, "Lv: ", 0xFF888888);
    draw_text(g, offset_x + 105, stats_y, level_buf, 0xFFFFFFFF);
    
    // Draw controls hint (if enabled)
    if (show_controls) {
        int ctrl_y = stats_y + 15;
        draw_text(g, offset_x, ctrl_y, "Move: ", 0xFF666666);
        draw_text(g, offset_x, ctrl_y + 10, "Rotate: ", 0xFF666666);
        draw_text(g, offset_x, ctrl_y + 20, "Hold: ", 0xFF666666);
        draw_text(g, offset_x, ctrl_y + 30, "Drop: ", 0xFF666666);
    }
    
    // Game over overlay
    if (p->game_over) {
        int msg_x = shake_offset_x + (BOARD_WIDTH * BLOCK_SIZE) / 2 - 40;
        int msg_y = shake_offset_y + (BOARD_HEIGHT * BLOCK_SIZE) / 2 - 20;
        gfx_fill_rect(g, msg_x - 10, msg_y - 10, 100, 40, 0xFF800000);
        draw_text(g, msg_x, msg_y, "DEFEATED!", 0xFFFF0000);
    }
}

void draw_board(Gfx *g) {
    // Check if multiplayer mode
    if (current_mode == MODE_MULTIPLAYER && game_state == STATE_PLAYING) {
        gfx_fill_rect(g, 0, 0, g->vi.width, g->vi.height, 0xFF000000);
        
        if (num_players == 2) {
            // 2-player side-by-side layout
            int p1_offset_x = 40;
            int p2_offset_x = g->vi.width / 2 + 20;
            int board_offset_y = 60;
            
            // Draw center divider line
            int center_x = g->vi.width / 2;
            for(int y = 0; y < g->vi.height; y += 8) {
                gfx_fill_rect(g, center_x - 2, y, 4, 4, 0xFF444444);
            }
            
            // Draw "VS" in the center top
            draw_text(g, center_x - 10, 20, "VS", 0xFFFFFF00);
            
            // Draw both player boards
            draw_player_board(g, &players[0], p1_offset_x, board_offset_y, player_names[0], 1);
            draw_player_board(g, &players[1], p2_offset_x, board_offset_y, player_names[1], 1);
            
            // Draw controls for both players
            int ctrl_y = board_offset_y + (BOARD_HEIGHT * BLOCK_SIZE) + 25;
            draw_text(g, p1_offset_x + 45, ctrl_y, "WASD+C+SPC", 0xFF888888);
            draw_text(g, p2_offset_x + 55, ctrl_y, "Arrows+N+0", 0xFF888888);
        } else {
            // 3-4 player 2x2 grid layout
            int grid_w = g->vi.width / 2;
            int grid_h = g->vi.height / 2;
            
            int offsets[4][2] = {
                {15, 10},                    // Top-left
                {grid_w + 15, 10},           // Top-right
                {15, grid_h + 10},           // Bottom-left
                {grid_w + 15, grid_h + 10}   // Bottom-right
            };
            
            // Draw grid dividers
            int center_x = g->vi.width / 2;
            int center_y = g->vi.height / 2;
            for(int x = 0; x < g->vi.width; x += 8) {
                gfx_fill_rect(g, x, center_y - 2, 4, 4, 0xFF444444);
            }
            for(int y = 0; y < g->vi.height; y += 8) {
                gfx_fill_rect(g, center_x - 2, y, 4, 4, 0xFF444444);
            }
            
            // Draw all active players (compact, no controls shown)
            for (int i = 0; i < num_players; i++) {
                draw_player_board(g, &players[i], offsets[i][0], offsets[i][1], player_names[i], 0);
            }
        }
        
        // Draw winner message
        if (mp_winner > 0) {
            int msg_x = g->vi.width / 2 - 100;
            int msg_y = 20;
            gfx_fill_rect(g, msg_x - 10, msg_y - 10, 220, 40, 0xFF000000);
            gfx_fill_rect(g, msg_x - 12, msg_y - 12, 224, 44, 0xFFFFD700);
            gfx_fill_rect(g, msg_x - 10, msg_y - 10, 220, 40, 0xFF000000);
            
            const char *winner_text = (mp_winner == 1) ? "PLAYER 1 WINS!" : "PLAYER 2 WINS!";
            draw_text_scaled(g, msg_x, msg_y, winner_text, 0xFFFFD700, 2);
            draw_text(g, msg_x + 20, msg_y + 25, "Press R to Restart", 0xFF00FF00);
        }
        
        return;
    }
    
    switch (current_theme) {
        case THEME_TEXTMODE: {
            // Retro text-mode style background
            gfx_fill_rect(g, 0, 0, g->vi.width, g->vi.height, 0xFF000000);
            
            // Draw heavy grid lines like ASCII art
            for (int x = 0; x <= BOARD_WIDTH; x++) {
                int px = OFFSET_X + x * BLOCK_SIZE;
                gfx_fill_rect(g, px, OFFSET_Y, 1, BOARD_HEIGHT * BLOCK_SIZE, 0xFF444444);
            }
            for (int y = 0; y <= BOARD_HEIGHT; y++) {
                int py = OFFSET_Y + y * BLOCK_SIZE;
                gfx_fill_rect(g, OFFSET_X, py, BOARD_WIDTH * BLOCK_SIZE, 1, 0xFF444444);
            }
            
            // CRT scanlines effect
            for (uint32_t y = 0; y < g->vi.height; y += 2) {
                gfx_fill_rect(g, 0, y, g->vi.width, 1, 0x20000000);
            }
            
            break;
        }
        case THEME_RUSSIA: {
            for (int y = 0; y < g->vi.height; y += 3) {
                gfx_fill_rect(g, 0, y, g->vi.width, 2, 0xFF1A0A0A);
            }
            
            for (int i = 0; i < 3; i++) {
                int x = 50 + i * 200;
                int y = g->vi.height - 200 + i * 30;
                
                gfx_fill_rect(g, x, y, 80, 120, 0xFF8B0000);
                gfx_fill_rect(g, x + 10, y + 10, 60, 30, 0xFFFFD700);
                
                for (int j = 0; j < 5; j++) {
                    gfx_fill_rect(g, x + 15 + j * 10, y + 50, 8, 70, 0xFFA0522D);
                }
                
                int dome_x = x + 35;
                int dome_y = y - 20;
                for (int r = 0; r < 15; r++) {
                    gfx_fill_rect(g, dome_x - r, dome_y + r, r * 2, 2, 0xFFFFD700);
                }
                gfx_fill_rect(g, dome_x - 3, dome_y - 15, 6, 15, 0xFFFFD700);
            }
            break;
        }
        case THEME_CHINA: {
            for (int y = 0; y < g->vi.height; y += 2) {
                uint32_t sky = (y < g->vi.height / 2) ? 0xFF8B0000 : 0xFF5A0000;
                gfx_fill_rect(g, 0, y, g->vi.width, 1, sky);
            }
            
            for (int i = 0; i < 2; i++) {
                int x = 100 + i * 300;
                int y = g->vi.height - 250;
                
                for (int level = 0; level < 5; level++) {
                    int width = 100 - level * 10;
                    int height = 35;
                    gfx_fill_rect(g, x - width / 2, y + level * height, width, height - 5, 0xFF8B4513);
                    gfx_fill_rect(g, x - width / 2 - 5, y + level * height, width + 10, 8, 0xFFFF0000);
                    
                    for (int w = 0; w < width; w += 8) {
                        gfx_fill_rect(g, x - width / 2 + w, y + level * height + 8, 6, 3, 0xFFFFD700);
                    }
                }
            }
            
            int lantern_y = 80;
            for (int i = 0; i < 4; i++) {
                int lx = 80 + i * 150;
                gfx_fill_rect(g, lx, lantern_y, 2, 20, 0xFF8B4513);
                gfx_fill_rect(g, lx - 10, lantern_y + 20, 22, 30, 0xFFFF0000);
                gfx_fill_rect(g, lx - 8, lantern_y + 25, 18, 20, 0xFFFFD700);
            }
            break;
        }
        case THEME_JAPAN: {
            for (int y = 0; y < g->vi.height; y++) {
                uint32_t sky_color = 0xFF000000 | 
                    ((240 - y * 100 / g->vi.height) << 16) |
                    ((200 - y * 80 / g->vi.height) << 8) |
                    (220 - y * 60 / g->vi.height);
                gfx_fill_rect(g, 0, y, g->vi.width, 1, sky_color);
            }
            
            int sun_x = g->vi.width - 150;
            int sun_y = 100;
            for (int r = 0; r < 40; r++) {
                for (int a = 0; a < 360; a += 10) {
                    int px = sun_x + (r * 3) / 4;
                    int py = sun_y + (r * 3) / 4;
                    if (px >= 0 && px < (int)g->vi.width && py >= 0 && py < (int)g->vi.height) {
                        gfx_fill_rect(g, px, py, 2, 2, 0xFFFFDDDD);
                    }
                }
            }
            
            for (int i = 0; i < 3; i++) {
                int tree_x = 80 + i * 200;
                int tree_y = g->vi.height - 200;
                
                gfx_fill_rect(g, tree_x, tree_y, 15, 150, 0xFF4A2511);
                
                for (int b = 0; b < 20; b++) {
                    int bx = tree_x - 40 + (rnd_u32() % 80);
                    int by = tree_y - 20 + (rnd_u32() % 60);
                    int size = 8 + (rnd_u32() % 8);
                    gfx_fill_rect(g, bx, by, size, size, 0xFFFFB7C5);
                }
            }
            break;
        }
        case THEME_EUROPE: {
            for (int y = 0; y < g->vi.height; y += 2) {
                uint32_t sky = 0xFF000000 | (100 << 16) | (150 << 8) | 200;
                gfx_fill_rect(g, 0, y, g->vi.width, 1, sky);
            }
            
            int castle_x = g->vi.width / 2 - 100;
            int castle_y = g->vi.height - 280;
            
            gfx_fill_rect(g, castle_x, castle_y + 100, 200, 180, 0xFF8B7355);
            
            for (int i = 0; i < 4; i++) {
                int tower_x = castle_x + i * 60;
                gfx_fill_rect(g, tower_x, castle_y, 50, 200, 0xFF696969);
                
                for (int j = 0; j < 3; j++) {
                    gfx_fill_rect(g, tower_x + j * 15, castle_y - 15, 10, 15, 0xFF696969);
                }
                
                for (int r = 0; r < 25; r++) {
                    gfx_fill_rect(g, tower_x + 25 - r, castle_y + r, r * 2, 2, 0xFF003399);
                }
            }
            
            for (int i = 0; i < 5; i++) {
                int win_y = castle_y + 120 + (i / 2) * 30;
                int win_x = castle_x + 30 + (i % 2) * 60;
                gfx_fill_rect(g, win_x, win_y, 20, 25, 0xFFFFCC00);
            }
            break;
        }
        case THEME_NORTH_AMERICA: {
            for (int y = 0; y < g->vi.height; y++) {
                uint32_t sky = 0xFF000000 | (50 << 16) | (100 << 8) | 180;
                gfx_fill_rect(g, 0, y, g->vi.width, 1, sky);
            }
            
            int city_y = g->vi.height - 300;
            for (int i = 0; i < 6; i++) {
                int bldg_x = 50 + i * 100;
                int bldg_h = 150 + (rnd_u32() % 150);
                
                gfx_fill_rect(g, bldg_x, city_y + (300 - bldg_h), 80, bldg_h, 0xFF2F4F4F);
                
                for (int floor = 0; floor < bldg_h / 20; floor++) {
                    for (int win = 0; win < 3; win++) {
                        int wx = bldg_x + 10 + win * 25;
                        int wy = city_y + (300 - bldg_h) + floor * 20 + 5;
                        uint32_t light = ((floor + win) % 3 == 0) ? 0xFFFFFF00 : 0xFF404040;
                        gfx_fill_rect(g, wx, wy, 15, 12, light);
                    }
                }
            }
            
            for (int s = 0; s < 15; s++) {
                int sx = 60 + (rnd_u32() % 500);
                int sy = 40 + (rnd_u32() % 150);
                gfx_fill_rect(g, sx, sy, 2, 2, 0xFFFFFFFF);
            }
            break;
        }
        case THEME_SOUTH_AMERICA: {
            for (int y = 0; y < g->vi.height; y++) {
                if (y < g->vi.height / 2) {
                    gfx_fill_rect(g, 0, y, g->vi.width, 1, 0xFF87CEEB);
                } else {
                    gfx_fill_rect(g, 0, y, g->vi.width, 1, 0xFF228B22);
                }
            }
            
            for (int i = 0; i < 4; i++) {
                int palm_x = 100 + i * 150;
                int palm_y = g->vi.height - 180;
                
                gfx_fill_rect(g, palm_x, palm_y, 20, 150, 0xFF8B4513);
                
                for (int leaf = 0; leaf < 8; leaf++) {
                    int lx = palm_x + 10;
                    int ly = palm_y - 20;
                    int angle_off = leaf * 45;
                    
                    if (angle_off < 180) {
                        gfx_fill_rect(g, lx - 40 + angle_off / 6, ly - angle_off / 8, 50, 15, 0xFF228B22);
                    } else {
                        gfx_fill_rect(g, lx - 10 + (angle_off - 180) / 6, ly - (360 - angle_off) / 8, 50, 15, 0xFF228B22);
                    }
                }
            }
            
            int sun_x = g->vi.width - 100;
            int sun_y = 80;
            for (int r = 0; r < 30; r++) {
                int px = sun_x - r / 2;
                int py = sun_y - r / 2;
                gfx_fill_rect(g, px, py, r, r, 0xFFFFFF00);
            }
            break;
        }
        default:
            break;
    }
    
    gfx_fill_rect(g, OFFSET_X - 6, OFFSET_Y - 6, 
                  BOARD_WIDTH * BLOCK_SIZE + 12, 
                  BOARD_HEIGHT * BLOCK_SIZE + 12, 0xFF808080);
    gfx_fill_rect(g, OFFSET_X - 4, OFFSET_Y - 4, 
                  BOARD_WIDTH * BLOCK_SIZE + 8, 
                  BOARD_HEIGHT * BLOCK_SIZE + 8, 0xFF000000);
    gfx_fill_rect(g, OFFSET_X - 3, OFFSET_Y - 3, 
                  BOARD_WIDTH * BLOCK_SIZE + 6, 
                  BOARD_HEIGHT * BLOCK_SIZE + 6, 0xFF202020);
    
    for (int y = 0; y < BOARD_HEIGHT; y++) {
        for (int x = 0; x < BOARD_WIDTH; x++) {
            int cell = board[y][x];
            draw_block(g, x, y, COLORS[cell]);
        }
    }
    
    if (!game_over) {
        int temp[4][4];
        memcpy(temp, PIECES[current_piece], sizeof(temp));
        
        for (int r = 0; r < current_rotation; r++) {
            int rotated[4][4];
            rotate_piece(temp, rotated);
            memcpy(temp, rotated, sizeof(temp));
        }
        
        int ghost_y = get_ghost_y();
        
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                if (temp[y][x] && ghost_y + y >= 0 && ghost_y != current_y) {
                    int px = OFFSET_X + (current_x + x) * BLOCK_SIZE;
                    int py = OFFSET_Y + (ghost_y + y) * BLOCK_SIZE;
                    
                    uint32_t ghost_color = COLORS[current_piece + 1];
                    uint8_t r = (ghost_color >> 16) & 0xFF;
                    uint8_t gr = (ghost_color >> 8) & 0xFF;
                    uint8_t b = ghost_color & 0xFF;
                    uint32_t faded = 0xFF000000 | ((r / 4) << 16) | ((gr / 4) << 8) | (b / 4);
                    
                    gfx_fill_rect(g, px + 2, py + 2, BLOCK_SIZE - 4, BLOCK_SIZE - 4, faded);
                    gfx_fill_rect(g, px + 2, py + 2, BLOCK_SIZE - 4, 2, ghost_color);
                    gfx_fill_rect(g, px + 2, py + BLOCK_SIZE - 4, BLOCK_SIZE - 4, 2, ghost_color);
                    gfx_fill_rect(g, px + 2, py + 2, 2, BLOCK_SIZE - 4, ghost_color);
                    gfx_fill_rect(g, px + BLOCK_SIZE - 4, py + 2, 2, BLOCK_SIZE - 4, ghost_color);
                }
            }
        }
        
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                if (temp[y][x] && current_y + y >= 0) {
                    draw_block(g, current_x + x, current_y + y, COLORS[current_piece + 1]);
                }
            }
        }
    }
    
    draw_held_piece(g);
    draw_next_piece(g, next_piece);
    draw_ui(g);
    
    if (game_over) {
        int msg_x = OFFSET_X + (BOARD_WIDTH * BLOCK_SIZE) / 2 - 80;
        int msg_y = OFFSET_Y + (BOARD_HEIGHT * BLOCK_SIZE) / 2 - 35;
        
        gfx_fill_rect(g, msg_x - 15, msg_y - 15, 200, 80, 0xFF000000);
        gfx_fill_rect(g, msg_x - 17, msg_y - 17, 204, 84, 0xFFFF0000);
        gfx_fill_rect(g, msg_x - 15, msg_y - 15, 200, 80, 0xFF200000);
        gfx_fill_rect(g, msg_x - 12, msg_y - 12, 194, 74, 0xFF000000);
        
        draw_text_scaled(g, msg_x, msg_y, "GAME OVER!", 0xFFFF0000, 2);
        
        gfx_fill_rect(g, msg_x, msg_y + 35, 160, 20, 0xFF1A1A1A);
        draw_text(g, msg_x + 5, msg_y + 40, "Press R to Restart", 0xFF00FF00);
    }
}

void init_menu(Gfx *g) {
    for (int i = 0; i < MAX_MENU_PIECES; i++) {
        menu_pieces[i].piece_type = rnd_u32() % 7;
        menu_pieces[i].rotation = rnd_u32() % 4;
        menu_pieces[i].x = (float)(rnd_u32() % (g->vi.width - 64));
        menu_pieces[i].y = -(float)(rnd_u32() % 400 + i * 30);
        menu_pieces[i].speed = 1.0f + (float)(rnd_u32() % 150) / 100.0f;
        menu_pieces[i].active = 1;
    }
}

void update_menu_pieces(Gfx *g) {
    for (int i = 0; i < MAX_MENU_PIECES; i++) {
        if (menu_pieces[i].active) {
            menu_pieces[i].y += menu_pieces[i].speed * 2.0f;
            
            if (menu_pieces[i].y > (float)g->vi.height) {
                menu_pieces[i].piece_type = rnd_u32() % 7;
                menu_pieces[i].rotation = rnd_u32() % 4;
                menu_pieces[i].x = (float)(rnd_u32() % (g->vi.width - 64));
                menu_pieces[i].y = -80.0f;
                menu_pieces[i].speed = 1.0f + (float)(rnd_u32() % 150) / 100.0f;
            }
        }
    }
}

void draw_menu_piece(Gfx *g, MenuPiece *mp) {
    int temp[4][4];
    memcpy(temp, PIECES[mp->piece_type], sizeof(temp));
    
    for (int r = 0; r < mp->rotation; r++) {
        int rotated[4][4];
        rotate_piece(temp, rotated);
        memcpy(temp, rotated, sizeof(temp));
    }
    
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (temp[y][x]) {
                int px = (int)mp->x + x * 20;
                int py = (int)mp->y + y * 20;
                
                if (px >= 0 && py >= 0 && px < (int)g->vi.width - 20 && py < (int)g->vi.height - 20) {
                    uint32_t color = COLORS[mp->piece_type + 1];
                    uint8_t r = (color >> 16) & 0xFF;
                    uint8_t gr = (color >> 8) & 0xFF;
                    uint8_t b = color & 0xFF;
                    
                    uint32_t faded = 0xFF000000 | ((r / 2) << 16) | ((gr / 2) << 8) | (b / 2);
                    
                    gfx_fill_rect(g, px, py, 19, 19, 0xFF000000);
                    gfx_fill_rect(g, px + 1, py + 1, 17, 17, faded);
                    
                    uint32_t highlight = 0xFF000000 | 
                                        (((r * 3 / 4) << 16)) |
                                        (((gr * 3 / 4) << 8)) |
                                        ((b * 3 / 4));
                    
                    uint32_t shadow = 0xFF000000 | 
                                     ((r / 4) << 16) |
                                     ((gr / 4) << 8) |
                                     (b / 4);
                    
                    gfx_fill_rect(g, px + 1, py + 1, 17, 3, highlight);
                    gfx_fill_rect(g, px + 1, py + 1, 3, 17, highlight);
                    gfx_fill_rect(g, px + 1, py + 15, 17, 3, shadow);
                    gfx_fill_rect(g, px + 15, py + 1, 3, 17, shadow);
                }
            }
        }
    }
}

void draw_menu(Gfx *g) {
    // Animated gradient background
    static uint32_t bg_anim = 0;
    bg_anim++;
    
    for (uint32_t y = 0; y < g->vi.height; y++) {
        uint8_t r = (uint8_t)((y + bg_anim / 2) % 60);
        uint8_t g_val = (uint8_t)((y * 2 + bg_anim / 3) % 40);
        uint8_t b = (uint8_t)((y + bg_anim) % 80);
        uint32_t color = 0xFF000000 | (r << 16) | (g_val << 8) | b;
        gfx_fill_rect(g, 0, y, g->vi.width, 1, color);
    }
    
    // Scanlines for depth
    for (uint32_t y = 0; y < g->vi.height; y += 3) {
        gfx_fill_rect(g, 0, y, g->vi.width, 1, 0x30000000);
    }
    
    for (int i = 0; i < MAX_MENU_PIECES; i++) {
        if (menu_pieces[i].active) {
            draw_menu_piece(g, &menu_pieces[i]);
        }
    }
    
    int center_x = g->vi.width / 2;
    int center_y = g->vi.height / 2;
    
    gfx_fill_rect(g, center_x - 250, center_y - 180, 500, 380, 0xFF404040);
    gfx_fill_rect(g, center_x - 248, center_y - 178, 496, 376, 0xFF000000);
    gfx_fill_rect(g, center_x - 245, center_y - 175, 490, 370, 0xFF0A0A0A);
    
    int tab_y = center_y - 160;
    const char *tab_names[] = {"MAIN", "MODES", "CHALLENGES", "SETTINGS", "CONTROLS"};
    
    int tab_width = 90;
    int total_tab_width = MENU_TAB_COUNT * tab_width;
    int tab_start_x = center_x - total_tab_width / 2;
    
    for (int i = 0; i < MENU_TAB_COUNT; i++) {
        int tab_x = tab_start_x + i * tab_width;
        uint32_t tab_color = (i == current_menu_tab) ? 0xFF00FFFF : 0xFF404040;
        uint32_t text_color = (i == current_menu_tab) ? 0xFF00FFFF : 0xFF888888;
        
        gfx_fill_rect(g, tab_x, tab_y, tab_width - 5, 25, tab_color);
        gfx_fill_rect(g, tab_x + 2, tab_y + 2, tab_width - 9, 21, 0xFF0A0A0A);
        
        int name_width = fnt_string_width(game_font, tab_names[i]);
        draw_text(g, tab_x + (tab_width - 5) / 2 - name_width / 2, tab_y + 7, tab_names[i], text_color);
    }
    
    gfx_fill_rect(g, center_x - 230, tab_y + 30, 460, 2, 0xFF404040);
    
    if (current_menu_tab == MENU_TAB_MAIN) {
        draw_text_scaled(g, center_x - 140, center_y - 100, "TESERARIS", 0xFF00FFFF, 3);
        
        int subtitle_width = fnt_string_width(game_font, "A Falling Blocks Puzzle Game");
        draw_text(g, center_x - subtitle_width / 2, center_y - 45, "A Falling Blocks Puzzle Game", 0xFF888888);
        
        gfx_fill_rect(g, center_x - 150, center_y, 300, 60, 0xFF1A1A1A);
        gfx_fill_rect(g, center_x - 148, center_y + 2, 296, 56, 0xFF004040);
        
        int press_width = fnt_string_width_scaled(game_font, "PRESS ENTER TO START", 2);
        draw_text_scaled(g, center_x - press_width / 2, center_y + 18, "PRESS ENTER TO START", 0xFF00FFFF, 2);
        
        draw_text(g, center_x - 100, center_y + 90, "TAB or -> to change tabs", 0xFF666666);
        draw_text(g, center_x - 80, center_y + 110, "ESC to quit", 0xFF666666);
        
    } else if (current_menu_tab == MENU_TAB_MODES) {
        draw_text_scaled(g, center_x - 80, center_y - 90, "GAME MODES", 0xFF00FFFF, 2);
        
        gfx_fill_rect(g, center_x - 180, center_y - 30, 360, 100, 0xFF1A1A1A);
        gfx_fill_rect(g, center_x - 178, center_y - 28, 356, 96, 0xFF0A0A0A);
        
        draw_text_scaled(g, center_x - 100, center_y - 10, "<", 0xFF00FFFF, 2);
        
        int mode_name_width = fnt_string_width_scaled(game_font, MODE_NAMES[selected_mode], 2);
        draw_text_scaled(g, center_x - mode_name_width / 2, center_y - 8, MODE_NAMES[selected_mode], 0xFFFFDD00, 2);
        
        draw_text_scaled(g, center_x + 80, center_y - 10, ">", 0xFF00FFFF, 2);
        
        int desc_width = fnt_string_width(game_font, MODE_DESCRIPTIONS[selected_mode]);
        draw_text(g, center_x - desc_width / 2, center_y + 30, MODE_DESCRIPTIONS[selected_mode], 0xFF888888);
        
        draw_text(g, center_x - 110, center_y + 100, "W/S or Up/Down to change mode", 0xFF666666);
        
    } else if (current_menu_tab == MENU_TAB_CHALLENGES) {
        draw_text_scaled(g, center_x - 90, center_y - 90, "CHALLENGES", 0xFF00FFFF, 2);
        
        gfx_fill_rect(g, center_x - 180, center_y - 30, 360, 120, 0xFF1A1A1A);
        gfx_fill_rect(g, center_x - 178, center_y - 28, 356, 116, 0xFF0A0A0A);
        
        draw_text_scaled(g, center_x - 100, center_y - 10, "<", 0xFF00FFFF, 2);
        
        int chal_name_width = fnt_string_width_scaled(game_font, CHALLENGE_NAMES[selected_challenge], 2);
        uint32_t chal_color = (selected_challenge == CHALLENGE_NONE) ? 0xFF888888 : 0xFFFF0000;
        draw_text_scaled(g, center_x - chal_name_width / 2, center_y - 8, CHALLENGE_NAMES[selected_challenge], chal_color, 2);
        
        draw_text_scaled(g, center_x + 80, center_y - 10, ">", 0xFF00FFFF, 2);
        
        int chal_desc_width = fnt_string_width(game_font, CHALLENGE_DESCRIPTIONS[selected_challenge]);
        draw_text(g, center_x - chal_desc_width / 2, center_y + 30, CHALLENGE_DESCRIPTIONS[selected_challenge], 0xFF888888);
        
        if (selected_challenge != CHALLENGE_NONE) {
            draw_text(g, center_x - 110, center_y + 55, "Challenge activates on game start!", 0xFFFFDD00);
        }
        
        draw_text(g, center_x - 120, center_y + 100, "W/S or Up/Down to change challenge", 0xFF666666);
        
    } else if (current_menu_tab == MENU_TAB_SETTINGS) {
        draw_text_scaled(g, center_x - 70, center_y - 90, "SETTINGS", 0xFF00FFFF, 2);
        
        gfx_fill_rect(g, center_x - 150, center_y - 30, 300, 80, 0xFF1A1A1A);
        gfx_fill_rect(g, center_x - 148, center_y - 28, 296, 76, 0xFF0A0A0A);
        
        draw_text(g, center_x - 100, center_y - 15, "Theme:", 0xFFFFFFFF);
        
        draw_text_scaled(g, center_x - 100, center_y + 5, "<", 0xFF00FFFF, 2);
        
        int theme_name_width = fnt_string_width_scaled(game_font, THEME_NAMES[current_theme], 2);
        draw_text_scaled(g, center_x - theme_name_width / 2, center_y + 7, THEME_NAMES[current_theme], 0xFFFFDD00, 2);
        
        draw_text_scaled(g, center_x + 80, center_y + 5, ">", 0xFF00FFFF, 2);
        
        draw_text(g, center_x - 120, center_y + 90, "W/S or Up/Down to change theme", 0xFF666666);
        
    } else if (current_menu_tab == MENU_TAB_CONTROLS) {
        draw_text_scaled(g, center_x - 80, center_y - 90, "CONTROLS", 0xFF00FFFF, 2);
        
        int ctrl_y = center_y - 50;
        draw_text(g, center_x - 150, ctrl_y, "Movement:", 0xFF00FFFF);
        draw_text(g, center_x - 130, ctrl_y + 20, "Move Left/Right: A/D or Arrow Keys", 0xFFFFFFFF);
        draw_text(g, center_x - 130, ctrl_y + 35, "Drop Faster: S or Down Arrow", 0xFFFFFFFF);
        draw_text(g, center_x - 130, ctrl_y + 50, "Rotate: W or Up Arrow", 0xFFFFFFFF);
        
        draw_text(g, center_x - 150, ctrl_y + 75, "Actions:", 0xFF00FFFF);
        draw_text(g, center_x - 130, ctrl_y + 95, "Instant Drop: SPACE", 0xFFFFFFFF);
        draw_text(g, center_x - 130, ctrl_y + 110, "Hold Piece: C", 0xFFFFFFFF);
        draw_text(g, center_x - 130, ctrl_y + 125, "Pause: P", 0xFFFFFFFF);
        
        draw_text(g, center_x - 100, ctrl_y + 150, "TAB or ESC to go back", 0xFF666666);
    }
}

// Initialize a player state for multiplayer
void init_player_state(PlayerState *p) {
    for (int y = 0; y < BOARD_HEIGHT; y++) {
        for (int x = 0; x < BOARD_WIDTH; x++) {
            p->board[y][x] = 0;
        }
    }
    p->current_piece = rnd_u32() % 7;
    p->next_piece = rnd_u32() % 7;
    p->current_rotation = 0;
    p->current_x = BOARD_WIDTH / 2 - 2;
    p->current_y = 0;
    p->score = 0;
    p->lines_cleared = 0;
    p->game_over = 0;
    p->held_piece = -1;
    p->can_hold = 1;
    p->combo_count = 0;
    p->level = 1;
    p->lines_for_next_level = 10;
    p->pending_garbage = 0;
    p->attack_flash = 0;
    p->receive_flash = 0;
    p->shake_x = 0;
    p->shake_y = 0;
    for(int i=0; i<MAX_PARTICLES; i++) {
        p->particles[i].life = 0;
    }
}

// Particle system functions
void spawn_particles(PlayerState *p, int x, int y, uint32_t color, int count) {
    for(int i=0; i<MAX_PARTICLES && count > 0; i++) {
        if(p->particles[i].life <= 0) {
            p->particles[i].x = (float)x;
            p->particles[i].y = (float)y;
            float angle = ((float)(rnd_u32() % 360)) * 3.14159f / 180.0f;
            float speed = 1.0f + ((float)(rnd_u32() % 100)) / 50.0f;
            p->particles[i].vx = cosf_approx(angle) * speed;
            p->particles[i].vy = sinf_approx(angle) * speed;
            p->particles[i].life = 30 + (rnd_u32() % 20);
            p->particles[i].color = color;
            count--;
        }
    }
}

void update_particles(PlayerState *p) {
    for(int i=0; i<MAX_PARTICLES; i++) {
        if(p->particles[i].life > 0) {
            p->particles[i].x += p->particles[i].vx;
            p->particles[i].y += p->particles[i].vy;
            p->particles[i].vy += 0.15f; // Gravity
            p->particles[i].life--;
        }
    }
}

void init_game() {
    if (current_mode == MODE_MULTIPLAYER) {
        init_player_state(&players[0]);
        init_player_state(&players[1]);
        mp_winner = 0;
        game_state = STATE_PLAYING;
        return;
    }
    
    for (int y = 0; y < BOARD_HEIGHT; y++) {
        for (int x = 0; x < BOARD_WIDTH; x++) {
            board[y][x] = 0;
        }
    }
    current_piece = rnd_u32() % 7;
    next_piece = rnd_u32() % 7;
    current_rotation = 0;
    current_x = BOARD_WIDTH / 2 - 2;
    current_y = 0;
    score = 0;
    lines_cleared = 0;
    game_over = 0;
    game_state = STATE_PLAYING;
    held_piece = -1;
    can_hold = 1;
    combo_count = 0;
    level = 1;
    lines_for_next_level = 10;
    chaos_timer = 0;
    chaos_effect = 0;
    gravity_reversed = 0;
    pieces_invisible = 0;
    
    // Mode-specific initialization
    if (current_mode == MODE_SPRINT) {
        sprint_start_time = time_ms();
    } else if (current_mode == MODE_INVISIBLE) {
        pieces_invisible = 1;
    }
    
    game_won = 0;
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;
    
    printf("Teseraris - Falling Blocks Puzzle\n");
    printf("Loading font...\n");
    
    int font_fd = open("/ModuOS/shared/usr/assets/fonts/Unicode.fnt", O_RDONLY, 0);
    if (font_fd < 0) {
        printf("Teseraris: cannot open font file\n");
        sleep(2);
        return 1;
    }
    
    long font_size = lseek(font_fd, 0, 2);
    lseek(font_fd, 0, 0);
    
    printf("Font size: %ld bytes\n", font_size);
    
    if (font_size <= 0 || font_size > 10 * 1024 * 1024) {
        printf("Teseraris: invalid font size (%ld)\n", font_size);
        close(font_fd);
        sleep(2);
        return 1;
    }
    
    void *font_data = malloc((size_t)font_size);
    if (!font_data) {
        printf("Teseraris: cannot allocate font buffer\n");
        close(font_fd);
        sleep(2);
        return 1;
    }
    
    size_t total_read = 0;
    while (total_read < (size_t)font_size) {
        ssize_t r = read(font_fd, (uint8_t*)font_data + total_read, (size_t)font_size - total_read);
        if (r <= 0) {
            printf("Teseraris: font read error (read %lu of %ld bytes)\n", (unsigned long)total_read, font_size);
            close(font_fd);
            free(font_data);
            sleep(2);
            return 1;
        }
        total_read += (size_t)r;
    }
    close(font_fd);
    
    printf("Successfully read %lu bytes\n", (unsigned long)total_read);
    
    printf("Parsing font data...\n");
    game_font = fnt_load_font(font_data, (size_t)font_size);
    free(font_data);
    
    if (!game_font) {
        printf("Teseraris: font load failed (invalid FNT format or corrupt data)\n");
        sleep(2);
        return 1;
    }
    
    printf("Font loaded: %u glyphs, %ux%u\n", 
           game_font->header.glyph_count,
           game_font->header.glyph_width,
           game_font->header.glyph_height);
    
    printf("Initializing graphics...\n");
    
    Gfx gfx;
    int gr = gfx_init(&gfx);
    if (gr != 0) {
        printf("Teseraris: graphics init failed (%d). Need framebuffer mode.\n", gr);
        fnt_free_font(game_font);
        sleep(2);
        return 1;
    }
    
    printf("Opening input device...\n");
    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) {
        printf("Teseraris: cannot open event device\n");
        sleep(2);
        return 2;
    }
    
    printf("Starting game...\n");
    rng_seed = (uint32_t)(time_ms() & 0x7fffffff);
    init_menu(&gfx);
    game_state = STATE_MENU;
    
    uint64_t last_fall = time_ms();
    uint64_t last_tick = time_ms();
    int quit = 0;
    
    printf("Entering main loop...\n");

    while (!quit) {
        // Read input events
        Event ev;
        while (read(efd, &ev, sizeof(ev)) > 0) {
            if (ev.type == EVENT_KEY_PRESSED) {
                KeyCode kc = ev.data.keyboard.keycode;
                char ch = ev.data.keyboard.ascii;
                
                // Handle menu input
                if (game_state == STATE_MENU) {
                    // TAB or LEFT/RIGHT arrows switch tabs
                    if (ch == '\t' || kc == KEY_TAB) {
                        current_menu_tab = (current_menu_tab + 1) % MENU_TAB_COUNT;
                    } else if (kc == KEY_ARROW_LEFT) {
                        current_menu_tab = (current_menu_tab - 1 + MENU_TAB_COUNT) % MENU_TAB_COUNT;
                    } else if (kc == KEY_ARROW_RIGHT) {
                        current_menu_tab = (current_menu_tab + 1) % MENU_TAB_COUNT;
                    }
                    // UP/DOWN arrows navigate within tab
                    else if (kc == KEY_ARROW_UP) {
                        if (current_menu_tab == MENU_TAB_MODES) {
                            selected_mode = (selected_mode - 1 + MODE_COUNT) % MODE_COUNT;
                        } else if (current_menu_tab == MENU_TAB_SETTINGS) {
                            selected_theme = (selected_theme - 1 + THEME_COUNT) % THEME_COUNT;
                            current_theme = selected_theme;
                        } else if (current_menu_tab == MENU_TAB_CHALLENGES) {
                            selected_challenge = (selected_challenge - 1 + CHALLENGE_COUNT) % CHALLENGE_COUNT;
                        }
                    } else if (kc == KEY_ARROW_DOWN) {
                        if (current_menu_tab == MENU_TAB_MODES) {
                            selected_mode = (selected_mode + 1) % MODE_COUNT;
                        } else if (current_menu_tab == MENU_TAB_SETTINGS) {
                            selected_theme = (selected_theme + 1) % THEME_COUNT;
                            current_theme = selected_theme;
                        } else if (current_menu_tab == MENU_TAB_CHALLENGES) {
                            selected_challenge = (selected_challenge + 1) % CHALLENGE_COUNT;
                        }
                    } else if (kc == KEY_ENTER || ch == ' ') {
                        current_mode = selected_mode;
                        current_challenge = selected_challenge;
                        if (current_mode == MODE_MULTIPLAYER) {
                            game_state = STATE_LOBBY;
                        } else {
                            init_game();
                            game_state = STATE_PLAYING;
                        }
                    } else if (kc == KEY_ESCAPE || ch == 0x1b) {
                        quit = 1;
                    }
                } else if (game_state == STATE_LOBBY) {
                    // Lobby input - player count selection
                    if (kc == KEY_ARROW_UP && num_players < MAX_PLAYERS) {
                        num_players++;
                    } else if (kc == KEY_ARROW_DOWN && num_players > 2) {
                        num_players--;
                    } else if (kc == KEY_ENTER || ch == ' ') {
                        // Start multiplayer game
                        for (int i = 0; i < num_players; i++) {
                            init_player_state(&players[i]);
                        }
                        mp_winner = 0;
                        game_state = STATE_PLAYING;
                    } else if (kc == KEY_ESCAPE || ch == 0x1b) {
                        game_state = STATE_MENU;
                    }
                } else if (game_state == STATE_PAUSED) {
                    if (ch == 'p' || ch == 'P') {
                        game_state = STATE_PLAYING;
                    } else if (kc == KEY_ESCAPE || ch == 0x1b) {
                        game_state = STATE_MENU;
                        game_over = 0;
                        mp_winner = 0;
                    }
                } else if (game_state == STATE_PLAYING && current_mode != MODE_MULTIPLAYER) {
                    // Handle input during game over
                    if (game_over) {
                        if (ch == 'r' || ch == 'R') {
                            init_game();
                        } else if (kc == KEY_ESCAPE || ch == 0x1b) {
                            game_state = STATE_MENU;
                        }
                    } else {
                    // Single-player input
                    if (kc == KEY_ARROW_LEFT || ch == 'a' || ch == 'A') {
                        if (!check_collision(current_piece, current_rotation, current_x - 1, current_y)) {
                            current_x--;
                        }
                    } else if (kc == KEY_ARROW_RIGHT || ch == 'd' || ch == 'D') {
                        if (!check_collision(current_piece, current_rotation, current_x + 1, current_y)) {
                            current_x++;
                        }
                    } else if (kc == KEY_ARROW_DOWN || ch == 's' || ch == 'S') {
                        if (!check_collision(current_piece, current_rotation, current_x, current_y + 1)) {
                            current_y++;
                        }
                    } else if (kc == KEY_ARROW_UP || ch == 'w' || ch == 'W') {
                        int new_rotation = (current_rotation + 1) % 4;
                        if (!check_collision(current_piece, new_rotation, current_x, current_y)) {
                            current_rotation = new_rotation;
                        }
                    } else if (ch == ' ') {
                        while (!check_collision(current_piece, current_rotation, current_x, current_y + 1)) {
                            current_y++;
                        }
                    } else if (ch == 'c' || ch == 'C') {
                        hold_piece();
                    } else if (ch == 'p' || ch == 'P') {
                        game_state = STATE_PAUSED;
                    } else if (kc == KEY_ESCAPE || ch == 0x1b) {
                        game_state = STATE_MENU;
                    } else if (ch == 'r' || ch == 'R') {
                        init_game();
                    }
                    }
                } else if (current_mode == MODE_MULTIPLAYER && game_state == STATE_PLAYING) {
                    // Handle input when game is over (winner declared)
                    if (mp_winner != 0) {
                        if (ch == 'r' || ch == 'R') {
                            for (int i = 0; i < num_players; i++) {
                                init_player_state(&players[i]);
                            }
                            mp_winner = 0;
                        } else if (kc == KEY_ESCAPE || ch == 0x1b) {
                            game_state = STATE_MENU;
                            mp_winner = 0;
                        }
                    } else {
                    // Multiplayer input - Player 1 (WASD + C + Space)
                    if (ch == 'a' || ch == 'A') {
                        if (!mp_check_collision(&player1, player1.current_piece, player1.current_rotation, player1.current_x - 1, player1.current_y)) {
                            player1.current_x--;
                        }
                    } else if (ch == 'd' || ch == 'D') {
                        if (!mp_check_collision(&player1, player1.current_piece, player1.current_rotation, player1.current_x + 1, player1.current_y)) {
                            player1.current_x++;
                        }
                    } else if (ch == 's' || ch == 'S') {
                        if (!mp_check_collision(&player1, player1.current_piece, player1.current_rotation, player1.current_x, player1.current_y + 1)) {
                            player1.current_y++;
                        }
                    } else if (ch == 'w' || ch == 'W') {
                        int new_rot = (player1.current_rotation + 1) % 4;
                        if (!mp_check_collision(&player1, player1.current_piece, new_rot, player1.current_x, player1.current_y)) {
                            player1.current_rotation = new_rot;
                        }
                    } else if (ch == ' ') {
                        while (!mp_check_collision(&player1, player1.current_piece, player1.current_rotation, player1.current_x, player1.current_y + 1)) {
                            player1.current_y++;
                        }
                    } else if (ch == 'c' || ch == 'C') {
                        mp_hold_piece(&player1);
                    }
                    
                    // Player 2
                    if (kc == KEY_ARROW_LEFT) {
                        if (!mp_check_collision(&player2, player2.current_piece, player2.current_rotation, player2.current_x - 1, player2.current_y)) {
                            player2.current_x--;
                        }
                    } else if (kc == KEY_ARROW_RIGHT) {
                        if (!mp_check_collision(&player2, player2.current_piece, player2.current_rotation, player2.current_x + 1, player2.current_y)) {
                            player2.current_x++;
                        }
                    } else if (kc == KEY_ARROW_DOWN) {
                        if (!mp_check_collision(&player2, player2.current_piece, player2.current_rotation, player2.current_x, player2.current_y + 1)) {
                            player2.current_y++;
                        }
                    } else if (kc == KEY_ARROW_UP) {
                        int new_rot = (player2.current_rotation + 1) % 4;
                        if (!mp_check_collision(&player2, player2.current_piece, new_rot, player2.current_x, player2.current_y)) {
                            player2.current_rotation = new_rot;
                        }
                    } else if (ch == '0' || ch == ')') {
                        while (!mp_check_collision(&player2, player2.current_piece, player2.current_rotation, player2.current_x, player2.current_y + 1)) {
                            player2.current_y++;
                        }
                    } else if (ch == 'n' || ch == 'N') {
                        mp_hold_piece(&player2);
                    }
                    
                    // Player 3 input (TFGH + V + B) - if active
                    if (num_players >= 3) {
                        if (ch == 'f' || ch == 'F') {
                            if (!mp_check_collision(&players[2], players[2].current_piece, players[2].current_rotation, players[2].current_x - 1, players[2].current_y)) {
                                players[2].current_x--;
                            }
                        } else if (ch == 'h' || ch == 'H') {
                            if (!mp_check_collision(&players[2], players[2].current_piece, players[2].current_rotation, players[2].current_x + 1, players[2].current_y)) {
                                players[2].current_x++;
                            }
                        } else if (ch == 'g' || ch == 'G') {
                            if (!mp_check_collision(&players[2], players[2].current_piece, players[2].current_rotation, players[2].current_x, players[2].current_y + 1)) {
                                players[2].current_y++;
                            }
                        } else if (ch == 't' || ch == 'T') {
                            int new_rot = (players[2].current_rotation + 1) % 4;
                            if (!mp_check_collision(&players[2], players[2].current_piece, new_rot, players[2].current_x, players[2].current_y)) {
                                players[2].current_rotation = new_rot;
                            }
                        } else if (ch == 'b' || ch == 'B') {
                            while (!mp_check_collision(&players[2], players[2].current_piece, players[2].current_rotation, players[2].current_x, players[2].current_y + 1)) {
                                players[2].current_y++;
                            }
                        } else if (ch == 'v' || ch == 'V') {
                            mp_hold_piece(&players[2]);
                        }
                    }
                    
                    // Player 4 input (IJKL + M + ,) - if active
                    if (num_players >= 4) {
                        if (ch == 'j' || ch == 'J') {
                            if (!mp_check_collision(&players[3], players[3].current_piece, players[3].current_rotation, players[3].current_x - 1, players[3].current_y)) {
                                players[3].current_x--;
                            }
                        } else if (ch == 'l' || ch == 'L') {
                            if (!mp_check_collision(&players[3], players[3].current_piece, players[3].current_rotation, players[3].current_x + 1, players[3].current_y)) {
                                players[3].current_x++;
                            }
                        } else if (ch == 'k' || ch == 'K') {
                            if (!mp_check_collision(&players[3], players[3].current_piece, players[3].current_rotation, players[3].current_x, players[3].current_y + 1)) {
                                players[3].current_y++;
                            }
                        } else if (ch == 'i' || ch == 'I') {
                            int new_rot = (players[3].current_rotation + 1) % 4;
                            if (!mp_check_collision(&players[3], players[3].current_piece, new_rot, players[3].current_x, players[3].current_y)) {
                                players[3].current_rotation = new_rot;
                            }
                        } else if (ch == ',' || ch == '<') {
                            while (!mp_check_collision(&players[3], players[3].current_piece, players[3].current_rotation, players[3].current_x, players[3].current_y + 1)) {
                                players[3].current_y++;
                            }
                        } else if (ch == 'm' || ch == 'M') {
                            mp_hold_piece(&players[3]);
                        }
                    }
                    
                    // Pause and exit in multiplayer
                    if (ch == 'p' || ch == 'P') {
                        game_state = STATE_PAUSED;
                    } else if (kc == KEY_ESCAPE || ch == 0x1b) {
                        game_state = STATE_MENU;
                        mp_winner = 0;
                    } else if (ch == 'r' || ch == 'R') {
                        init_player_state(&player1);
                        init_player_state(&player2);
                        mp_winner = 0;
                    }
                }
                } // end multiplayer input
                
        } // end while read events
        
        uint64_t now = time_ms();
        
        // Menu animation
        if (game_state == STATE_MENU) {
            if (now - last_tick >= 30) {
                update_menu_pieces(&gfx);
                last_tick = now;
            }
        }
        
        // Multiplayer game logic
        if (game_state == STATE_PLAYING && current_mode == MODE_MULTIPLAYER && mp_winner == 0) {
            // Calculate fall interval based on average level of active players
            int total_level = 0;
            for (int i = 0; i < num_players; i++) {
                total_level += players[i].level;
            }
            int avg_level = total_level / num_players;
            int fall_interval = 500 - (avg_level * 30);
            if (fall_interval < 100) fall_interval = 100;
            
            if (now - last_fall >= (uint64_t)fall_interval) {
                // Update all active players
                for (int i = 0; i < num_players; i++) {
                    if (!players[i].game_over) {
                        if (!mp_check_collision(&players[i], players[i].current_piece, players[i].current_rotation, players[i].current_x, players[i].current_y + 1)) {
                            players[i].current_y++;
                        } else {
                            mp_lock_piece(&players[i]);
                            // Find opponent for garbage (cycle to next player)
                            PlayerState *opponent = &players[(i + 1) % num_players];
                            int cleared = mp_clear_lines(&players[i], opponent);
                            if (cleared > 0) {
                                players[i].lines_cleared += cleared;
                                int line_score = cleared * cleared * 100;
                                int combo_bonus = (players[i].combo_count - 1) * 50;
                                players[i].score += line_score + combo_bonus;
                            }
                            mp_spawn_piece(&players[i]);
                        }
                    }
                }
                
                // Update particles and effects for all players
                for (int i = 0; i < num_players; i++) {
                    update_particles(&players[i]);
                    
                    // Decay screen shake
                    if (players[i].shake_x != 0) players[i].shake_x = players[i].shake_x * 8 / 10;
                    if (players[i].shake_y != 0) players[i].shake_y = players[i].shake_y * 8 / 10;
                    
                    // Decay flash effects
                    if (players[i].attack_flash > 0) players[i].attack_flash--;
                    if (players[i].receive_flash > 0) players[i].receive_flash--;
                }
                
                // Check for winner - find last player standing
                int alive_count = 0;
                int last_alive = -1;
                for (int i = 0; i < num_players; i++) {
                    if (!players[i].game_over) {
                        alive_count++;
                        last_alive = i;
                    }
                }
                
                // Declare winner if only one player left
                if (alive_count == 1) {
                    mp_winner = last_alive + 1;
                } else if (alive_count == 0) {
                    // All died - highest score wins
                    int highest_score = -1;
                    for (int i = 0; i < num_players; i++) {
                        if (players[i].score > highest_score) {
                            highest_score = players[i].score;
                            mp_winner = i + 1;
                        }
                    }
                }
                
                last_fall = now;
            }
        } // end multiplayer game logic
        
        if (game_state == STATE_PLAYING && !game_over && current_mode != MODE_MULTIPLAYER) {
            int fall_interval = 500;
            
            if (current_mode == MODE_SPEED_RUN) {
                fall_interval = 500 - (level * 50);
                if (fall_interval < 100) fall_interval = 100;
            } else if (current_mode == MODE_ZEN) {
                fall_interval = 1000;
            } else {
                fall_interval = 500 - ((level - 1) * 30);
                if (fall_interval < 100) fall_interval = 100;
            }
            
            if (now - last_fall >= (uint64_t)fall_interval && !game_over) {
                if (!check_collision(current_piece, current_rotation, current_x, current_y + 1)) {
                    current_y++;
                } else {
                    lock_piece();
                    int cleared = clear_lines();
                    if (cleared > 0) {
                        lines_cleared += cleared;
                        int line_score = cleared * cleared * 100;
                        int combo_bonus = (combo_count - 1) * 50;
                        score += line_score + combo_bonus;
                        
                        if (current_mode == MODE_SPEED_RUN && lines_cleared >= 10) {
                            level++;
                            lines_for_next_level = lines_cleared + 10;
                        } else if (current_mode == MODE_CLASSIC && lines_cleared >= lines_for_next_level) {
                            level++;
                            lines_for_next_level += 10;
                        }
                        
                        if (current_mode == MODE_MARATHON && lines_cleared >= marathon_lines_goal) {
                            game_won = 1;
                        } else if (current_mode == MODE_SPRINT && lines_cleared >= sprint_lines_goal) {
                            game_won = 1;
                        }
                        
                        if (current_mode == MODE_CHAOS) {
                            chaos_timer++;
                            if (chaos_timer >= 100) {
                                chaos_effect = (rnd_u32() % 3) + 1;
                                chaos_timer = 0;
                            }
                        }
                    }
                    spawn_piece();
                }
                last_fall = now;
            }
        } // end single-player game logic
        
        // Draw (first iteration marker)
        static int first_draw = 1;
        if (first_draw) {
            printf("First draw...\n");
            first_draw = 0;
        }
        
        if (game_state == STATE_MENU) {
            draw_menu(&gfx);
        } else if (game_state == STATE_LOBBY) {
            // Draw lobby screen
            gfx_fill_rect(&gfx, 0, 0, gfx.vi.width, gfx.vi.height, 0xFF001020);
            
            int center_x = gfx.vi.width / 2;
            int center_y = gfx.vi.height / 2;
            
            draw_text_scaled(&gfx, center_x - 150, center_y - 100, "MULTIPLAYER LOBBY", 0xFF00FFFF, 2);
            
            // Player count selector
            draw_text(&gfx, center_x - 100, center_y - 30, "Number of Players:", 0xFFFFFFFF);
            
            char count_str[4];
            itoa(num_players, count_str, 10);
            draw_text_scaled(&gfx, center_x + 20, center_y - 40, count_str, 0xFFFFDD00, 3);
            
            // Instructions
            draw_text(&gfx, center_x - 120, center_y + 30, "UP/DOWN - Change player count", 0xFF888888);
            draw_text(&gfx, center_x - 120, center_y + 50, "ENTER - Start game", 0xFF00FF00);
            draw_text(&gfx, center_x - 120, center_y + 70, "ESC - Back to menu", 0xFFFF8888);
        } else if (game_state == STATE_PAUSED) {
            gfx_fill_rect(&gfx, 0, 0, gfx.vi.width, gfx.vi.height, 0xFF000000);
            draw_board(&gfx);
            
            int pause_x = gfx.vi.width / 2 - 100;
            int pause_y = gfx.vi.height / 2 - 50;
            gfx_fill_rect(&gfx, pause_x - 15, pause_y - 15, 230, 100, 0xFF404040);
            gfx_fill_rect(&gfx, pause_x - 13, pause_y - 13, 226, 96, 0xFF000000);
            gfx_fill_rect(&gfx, pause_x - 10, pause_y - 10, 220, 90, 0xFF1A1A1A);
            
            draw_text_scaled(&gfx, pause_x, pause_y, "PAUSED", 0xFFFFFF00, 3);
            draw_text(&gfx, pause_x + 10, pause_y + 45, "Press P to Resume", 0xFF00FFFF);
            draw_text(&gfx, pause_x + 10, pause_y + 60, "Press ESC for Menu", 0xFF888888);
        } else {
            gfx_fill_rect(&gfx, 0, 0, gfx.vi.width, gfx.vi.height, 0xFF000000);
            draw_board(&gfx);
        }
        gfx_present_full(&gfx);
        
        yield();
    }
    }
    
    printf("\nExiting...\n");
    input_flush();
    close(efd);
    if (game_font) {
        fnt_free_font(game_font);
    }
    if (gfx.bb) {
        free(gfx.bb);
    }
    return 0;
    
}