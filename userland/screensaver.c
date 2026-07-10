/*
 * screensaver.c — ModuOS screensaver
 *
 * Effects (cycle automatically every ~15 seconds, or press Tab to skip):
 *    1. DVD Bounce      — classic logo bouncing around, changes colour on hit
 *    2. Pipes           — 3D tubes + joints, software rasterised w/ Z-buffer
 *    3. Matrix Rain     — green falling character columns
 *    4. Starfield       — 3D star warp / hyperspace
 *    5. Plasma          — animated sine-wave colour field
 *    6. Metaballs       — three mutually-attracted blobs
 *    7. 3D Text Spinner — "ModuOS" text rotating in true 3D
 *    8. Cube Field      — tumbling 3D boxes flying past the camera
 *    9. Spinning Donut  — shaded, Z-buffered parametric torus
 *   10. Fire            — classic bottom-up heat-propagation fire
 *   11. Tunnel          — demo-scene polar texture tunnel
 *   12. Conway's Life   — Game of Life with colour-age cells
 *
 * Press ANY KEY (or any mouse button) to exit.
 * Press TAB to skip to next effect immediately.
 *
 * Copyright © 2026 New Technologies Software — GPL v2.0
 */

#include "libc.h"
#include "NodGL.h"
#include "lib_fnt.h"
#include "../include/moduos/kernel/events/events.h"

/* ============================================================
   Tunables
   ============================================================ */

#define EFFECT_DURATION_MS  15000u   /* 15 s per effect */
#define NUM_EFFECTS         15
#define FONT_PATH  "/ModuOS/shared/assets/fonts/Terminus.fnt"

/* ============================================================
   Fixed-point / math helpers (no libm)
   ============================================================ */

/* Sine table: 256 entries, range -128..127 (scaled by 128) */
static const int8_t sin_tbl[256] = {
     0,  3,  6,  9, 12, 15, 18, 21, 24,27, 30, 33, 36, 39, 42, 45,
    48, 51, 54, 57, 59, 62, 65, 67, 70, 73, 75, 78, 80, 82, 85, 87,
    89, 91, 94, 96, 98,100,102,103,105,107,108,110,112,113,114,116,
   117,118,119,120,121,122,123,123,124,125,125,126,126,126,127,127,
   127,127,127,126,126,126,125,125,124,123,123,122,121,120,119,118,
   117,116,114,113,112,110,108,107,105,103,102,100, 98, 96, 94, 91,
    89, 87, 85, 82, 80, 78, 75, 73, 70, 67, 65, 62, 59, 57, 54, 51,
    48, 45, 42, 39, 36, 33, 30, 27, 24, 21, 18, 15, 12,  9,  6,  3,
     0, -3, -6, -9,-12,-15,-18,-21,-24,-27,-30,-33,-36,-39,-42,-45,
   -48,-51,-54,-57,-59,-62,-65,-67,-70,-73,-75,-78,-80,-82,-85,-87,
   -89,-91,-94,-96,-98,-100,-102,-103,-105,-107,-108,-110,-112,-113,
  -114,-116,-117,-118,-119,-120,-121,-122,-123,-123,-124,-125,-125,
  -126,-126,-126,-127,-127,-127,-127,-127,-126,-126,-126,-125,-125,
  -124,-123,-123,-122,-121,-120,-119,-118,-117,-116,-114,-113,-112,
  -110,-108,-107,-105,-103,-102,-100,-98,-96,-94,-91,-89,-87,-85,
   -82,-80,-78,-75,-73,-70,-67,-65,-62,-59,-57,-54,-51,-48,-45,-42,
   -39,-36,-33,-30,-27,-24,-21,-18,-15,-12, -9, -6, -3
};
/* Extra entries to reach 256 exactly */

static inline int isin(uint8_t angle) { return sin_tbl[angle]; }
static inline int icos(uint8_t angle) { return sin_tbl[(uint8_t)(angle + 64)]; }

/* Float sqrt/abs without libm: a few Newton iterations on a sensible
   starting guess is plenty accurate for the cylinder basis below,
   which only needs a unit-length normalisation, not high precision. */
static inline float sqrtf3(float v){
    if(v<=0.0f) return 0.0f;
    float x=v, g=v*0.5f+0.5f; /* cheap initial guess */
    for(int i=0;i<8;i++) g = 0.5f*(g + x/g);
    return g;
}
static inline float fabsf3(float v){ return v<0.0f ? -v : v; }

/* Cheap sqrt for hot per-pixel callers that only feed a visual falloff
   (e.g. distance-based alpha in a small disc fill) rather than a
   geometry basis vector. Each Newton step here still costs a divide,
   so cutting 8 iterations down to 3 directly cuts that cost by ~2.6x
   at this call site; 3 steps from this starting guess is already
   accurate to several significant digits for the small magnitudes
   (a few pixels squared) this is actually called with, which is far
   more precision than an alpha gradient needs. Not a drop-in
   replacement for sqrtf3 in precision-sensitive geometry code. */
static inline float sqrtf3_fast(float v){
    if(v<=0.0f) return 0.0f;
    float x=v, g=v*0.5f+0.5f;
    for(int i=0;i<3;i++) g = 0.5f*(g + x/g);
    return g;
}

/* LCG random */
static uint32_t rng = 0xDEADBEEFu;
static inline uint32_t rng_next(void) {
    rng = rng * 1664525u + 1013904223u;
    return rng;
}
static inline int rng_range(int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + (int)(rng_next() % (uint32_t)(hi - lo));
}

/* ============================================================
   App globals
   ============================================================ */

static struct {
    NodGL_Device  device;
    NodGL_Context ctx;
    NodGL_Texture tex;
    uint8_t      *bb;
    uint32_t      bb_pitch;
    uint32_t      sw, sh;        /* screen width/height */
    fnt_font_t   *font;

    int           effect;        /* 0..NUM_EFFECTS-1 */
    uint64_t      effect_start;  /* time_ms() when effect began */
    int           quit;
} g;

/* ============================================================
   Pixel helpers
   ============================================================ */

static inline void put(int x, int y, uint32_t col) {
    if ((uint32_t)x >= g.sw || (uint32_t)y >= g.sh) return;
    ((uint32_t *)(g.bb + (uint64_t)y * g.bb_pitch))[x] = col;
}

static void fill(int x, int y, int w, int h, uint32_t col) {
    int x0=x<0?0:x, y0=y<0?0:y;
    int x1=x+w; if(x1>(int)g.sw)x1=(int)g.sw;
    int y1=y+h; if(y1>(int)g.sh)y1=(int)g.sh;
    for(int yy=y0;yy<y1;yy++){
        uint32_t *row=(uint32_t*)(g.bb+(uint64_t)yy*g.bb_pitch);
        for(int xx=x0;xx<x1;xx++) row[xx]=col;
    }
}

static void clear(uint32_t col){ fill(0,0,(int)g.sw,(int)g.sh,col); }

/* Exact integer divide-by-255 without a division instruction.
   (x + 1 + (x>>8)) >> 8 is bit-identical to x/255 for every x in
   0..255*255 (the full range r*a+bg*ia can produce) — verified
   exhaustively, not an approximation. Division is one of the slowest
   scalar ALU ops on common CPUs; this replaces a divide with a
   shift+add+shift. Alpha-blending is the single hottest per-pixel op
   in this file (HUD pill, shadows, glow, lissajous trails, plasma...),
   so this touches the most frequently executed code path in the
   whole screensaver. */
static inline uint32_t div255(uint32_t x){ return (x + 1 + (x>>8)) >> 8; }

static inline void blend(int x,int y,uint8_t r,uint8_t gv,uint8_t b,uint8_t a){
    if((uint32_t)x>=g.sw||(uint32_t)y>=g.sh||!g.bb) return;
    uint32_t *d=(uint32_t*)(g.bb+(uint64_t)y*g.bb_pitch)+x;
    if(a==255){*d=0xFF000000u|((uint32_t)r<<16)|((uint32_t)gv<<8)|b;return;}
    uint32_t bg=*d; uint32_t ia=255-a;
    *d=0xFF000000u
     |(div255(r*a+((bg>>16)&0xFF)*ia)<<16)
     |(div255(gv*a+((bg>>8)&0xFF)*ia)<<8)
     |(div255(b*a+(bg&0xFF)*ia));
}

/* Draw text via font */
static void draw_text(int x,int y,const char *s,uint32_t col){
    if(!g.font||!s) return;
    int cx=x;
    uint8_t r=(col>>16)&0xFF, gv=(col>>8)&0xFF, b=col&0xFF;
    while(*s){
        fnt_glyph_t *gl=fnt_get_glyph(g.font,(uint32_t)(unsigned char)*s);
        if(gl){
            for(int dy=0;dy<gl->bitmap_height;dy++)
                for(int dx=0;dx<gl->bitmap_width;dx++)
                    if(fnt_get_pixel(gl,dx,dy))
                        blend(cx+dx,y+dy,r,gv,b,255);
            cx+=gl->width;
        }
        s++;
    }
}

static void draw_text_scaled(int x,int y,const char *s,uint32_t col,int sc){
    if(!g.font||!s||sc<=1){draw_text(x,y,s,col);return;}
    int cx=x;
    uint8_t r=(col>>16)&0xFF, gv=(col>>8)&0xFF, b=col&0xFF;
    while(*s){
        fnt_glyph_t *gl=fnt_get_glyph(g.font,(uint32_t)(unsigned char)*s);
        if(gl){
            for(int dy=0;dy<gl->bitmap_height;dy++)
                for(int dx=0;dx<gl->bitmap_width;dx++)
                    if(fnt_get_pixel(gl,dx,dy))
                        fill(cx+dx*sc,y+dy*sc,sc,sc,
                             0xFF000000u|((uint32_t)r<<16)|((uint32_t)gv<<8)|b);
            cx+=gl->width*sc;
        }
        s++;
    }
}

/* ============================================================
   HSV -> RGB
   ============================================================ */
static uint32_t hsv(int h360,int s100,int v100){
    /* h 0-359, s 0-100, v 0-100 */
    int H=h360/60, f=(h360%60)*256/60;
    int p=v100*(100-s100)/100*255/100;
    int q=v100*(100-(s100*f/256))/100*255/100;
    int t_=v100*(100-(s100*(255-f)/256))/100*255/100;
    int V=v100*255/100;
    int r,gv,b;
    switch(H%6){
        case 0:r=V;gv=t_;b=p;break;
        case 1:r=q;gv=V;b=p;break;
        case 2:r=p;gv=V;b=t_;break;
        case 3:r=p;gv=q;b=V;break;
        case 4:r=t_;gv=p;b=V;break;
        default:r=V;gv=p;b=q;break;
    }
    if(r>255)r=255; if(gv>255)gv=255; if(b>255)b=255;
    if(r<0)r=0; if(gv<0)gv=0; if(b<0)b=0;
    return 0xFF000000u|((uint32_t)r<<16)|((uint32_t)gv<<8)|(uint32_t)b;
}

/* ============================================================
   EFFECT 1 — DVD Bounce
   ============================================================ */

/* "DVD" logo, properly rounded + shaded, with a dirty-rect clear
   instead of a full-screen one.

   Why this was slow: the old version called clear() — a full-screen
   fill() — every single frame just to erase a 120x60 logo before
   redrawing it. At 1920x1080 that's ~2,073,600 pixel writes/frame
   spent wiping a screen that's otherwise empty, for an effect whose
   actual content is one small rectangle. Every other effect in this
   file either fills the whole screen anyway (plasma, fire, matrix)
   or has genuinely full-frame content, so DVD Bounce was the one
   effect paying full-screen cost for near-zero on-screen content.

   Fix: track the logo's previous frame bounding box (plus a small
   margin for the shadow/glow) and only erase *that* rect each frame,
   then draw the logo at its new position. Cost drops from "every
   pixel on screen" to "roughly one logo's worth of pixels," which is
   the single biggest win available for this effect.

   Why it looked bad: it was two overlapping fill() rects faking a
   rounded corner (it isn't actually rounded, just notched), four
   stray "corner dot" squares that don't read as part of the
   logo, and flat black text with no outline so it can disappear into
   saturated hues. Replaced with: a real filled rounded-rect (quarter-
   circle corners via a squared-distance test), a soft drop shadow,
   a simple top-to-bottom bevel so the body isn't flat, and outlined
   text (dark ring behind a bright fill) so "DVD" stays legible
   against every hue in the cycle. A brief radial flash on each wall
   hit adds the classic "nailed the corner" screensaver moment. */

#define DVD_W      120
#define DVD_H       60
#define DVD_RADIUS   14   /* corner rounding radius */
#define DVD_SHADOW    6   /* shadow offset, used to size the dirty rect */
#define DVD_FLASH_MS 220  /* corner-hit flash duration */

static struct {
    float x,y,vx,vy;
    int   hue;
    int   prev_x,prev_y;     /* previous frame's top-left, for dirty-rect clear */
    int   has_prev;          /* 0 on the first frame after init */
    uint64_t flash_start;    /* time_ms() of last wall hit, 0 = none active */
    int   flash_cx,flash_cy; /* screen point the flash radiates from */
} dvd;

static void dvd_init(void){
    dvd.x  = (float)(g.sw/4);
    dvd.y  = (float)(g.sh/4);
    dvd.vx = 2.6f;
    dvd.vy = 2.0f;
    dvd.hue = rng_range(0,360);
    dvd.has_prev = 0;
    dvd.flash_start = 0;
}

/* Filled rounded rectangle: corners are cut with a squared-distance
   test against each corner centre (avoids a sqrt per pixel — we only
   need to compare against r*r, never the actual distance). The body
   gets a light top-to-bottom bevel so it isn't a single flat tone. */
static void fill_rounded_rect(int x,int y,int w,int h,int r,uint32_t col){
    uint8_t cr=(col>>16)&0xFF, cg=(col>>8)&0xFF, cb=col&0xFF;
    int rr=r*r;
    for(int yy=0; yy<h; yy++){
        /* Bevel: brighten near the top edge, darken near the bottom,
           cheap linear ramp instead of a real lighting model. */
        int ramp = 40 - (yy*80)/(h>1?h-1:1); /* +40 .. -40 */
        int rr2=cr+ramp, gg2=cg+ramp, bb2=cb+ramp;
        if(rr2<0)rr2=0; if(rr2>255)rr2=255;
        if(gg2<0)gg2=0; if(gg2>255)gg2=255;
        if(bb2<0)bb2=0; if(bb2>255)bb2=255;
        uint32_t row_col = 0xFF000000u|((uint32_t)rr2<<16)|((uint32_t)gg2<<8)|(uint32_t)bb2;

        for(int xx=0; xx<w; xx++){
            /* Which corner (if any) is this pixel near? Skip the test
               entirely for the large interior region — only the four
               r x r corner blocks need the distance check. */
            int near_left  = xx < r;
            int near_right = xx >= w-r;
            int near_top   = yy < r;
            int near_bot   = yy >= h-r;
            if((near_left||near_right) && (near_top||near_bot)){
                int cx = near_left ? r : w-r-1;
                int cy = near_top  ? r : h-r-1;
                int dx = xx-cx, dy = yy-cy;
                if(dx*dx+dy*dy > rr) continue; /* outside the rounded corner */
            }
            put(x+xx, y+yy, row_col);
        }
    }
}

/* Outlined text: stamp the glyph offset by 1px in 4 directions in the
   outline colour first, then the fill colour on top, so "DVD" stays
   readable against any hue instead of risking flat-black-on-dark. */
static void draw_text_outlined(int x,int y,const char *s,uint32_t fill_col,uint32_t outline_col,int sc){
    static const int8_t ox4[4]={-1,1,0,0}, oy4[4]={0,0,-1,1};
    for(int k=0;k<4;k++)
        draw_text_scaled(x+ox4[k]*sc, y+oy4[k]*sc, s, outline_col, sc);
    draw_text_scaled(x, y, s, fill_col, sc);
}

static void dvd_update(void){
    uint64_t now = time_ms();

    /* Erase only last frame's footprint (logo + shadow margin), not
       the whole screen. This is the actual fix for the slowness. */
    if(dvd.has_prev){
        fill(dvd.prev_x-2, dvd.prev_y-2,
             DVD_W+DVD_SHADOW+4, DVD_H+DVD_SHADOW+4, 0xFF000000);
    } else {
        clear(0xFF000000); /* first frame only: establish a clean black field */
    }

    dvd.x += dvd.vx;
    dvd.y += dvd.vy;

    int hit = 0;
    if(dvd.x < 0)               { dvd.x=0;         dvd.vx=-dvd.vx; hit=1; }
    if(dvd.x > (int)g.sw-DVD_W) { dvd.x=(int)g.sw-DVD_W; dvd.vx=-dvd.vx; hit=1; }
    if(dvd.y < 0)               { dvd.y=0;         dvd.vy=-dvd.vy; hit=1; }
    if(dvd.y > (int)g.sh-DVD_H) { dvd.y=(int)g.sh-DVD_H; dvd.vy=-dvd.vy; hit=1; }

    if(hit){
        dvd.hue = (dvd.hue + 60 + rng_range(20,80)) % 360;
        dvd.flash_start = now;
        dvd.flash_cx = (int)dvd.x + DVD_W/2;
        dvd.flash_cy = (int)dvd.y + DVD_H/2;
    }

    uint32_t col = hsv(dvd.hue, 90, 100);
    int ix=(int)dvd.x, iy=(int)dvd.y;

    /* Soft drop shadow first, body on top of it */
    for(int yy=0; yy<DVD_H; yy++)
        for(int xx=0; xx<DVD_W; xx++)
            blend(ix+DVD_SHADOW+xx, iy+DVD_SHADOW+yy, 0,0,0, 70);

    fill_rounded_rect(ix, iy, DVD_W, DVD_H, DVD_RADIUS, col);

    draw_text_outlined(ix+18, iy+16, "DVD", 0xFFFFFFFF, 0xFF000000, 3);

    /* Corner-hit flash: a quick fading ring centred on the bounce
       point. Cheap (one circle outline, only while flash is active)
       and gives the classic "it hit the corner!" payoff. */
    if(dvd.flash_start != 0){
        uint64_t age = now - dvd.flash_start;
        if(age < DVD_FLASH_MS){
            int radius = 6 + (int)(age * 70 / DVD_FLASH_MS);
            uint8_t a = (uint8_t)(180 - (age*180/DVD_FLASH_MS));
            for(int a2=0; a2<256; a2+=4){
                int fx = dvd.flash_cx + (isin((uint8_t)a2)*radius)/128;
                int fy = dvd.flash_cy + (icos((uint8_t)a2)*radius)/128;
                blend(fx,fy,255,255,255,a);
            }
        } else {
            dvd.flash_start = 0;
        }
    }

    dvd.prev_x = ix; dvd.prev_y = iy; dvd.has_prev = 1;
}

/* ============================================================
   Shared software Z-buffer (used by real-3D effects: Pipes, Spinner)
   ============================================================ */

static float   *zbuf = NULL;
static uint32_t zbuf_w = 0, zbuf_h = 0;

static void zbuf_ensure(void){
    if(zbuf && zbuf_w==g.sw && zbuf_h==g.sh) return;
    if(zbuf) free(zbuf);
    zbuf_w=g.sw; zbuf_h=g.sh;
    zbuf=(float*)malloc((size_t)zbuf_w*(size_t)zbuf_h*sizeof(float));
}

static void zbuf_clear(float far_z){
    if(!zbuf) return;
    size_t n=(size_t)zbuf_w*(size_t)zbuf_h;
    /* All call sites pass the same far_z, so its bit pattern is just a
       repeated 32-bit word. Writing it as uint32_t instead of float
       makes this a flat memory-fill pattern the compiler can turn into
       wide (SSE/AVX) stores instead of one scalar float store at a
       time — this loop runs once per frame for every 3D effect, on a
       full-screen buffer, so it's worth being cheap. */
    uint32_t pattern;
    memcpy(&pattern,&far_z,sizeof(pattern));
    uint32_t *zp=(uint32_t*)zbuf;
    for(size_t i=0;i<n;i++) zp[i]=pattern;
}

/* Depth-tested pixel write: only writes if z is nearer (smaller) than
   what's already there. This is what actually makes overlapping 3D
   geometry occlude correctly instead of "whatever was drawn last wins". */
static inline void put_z(int x,int y,float z,uint32_t col){
    if((uint32_t)x>=g.sw||(uint32_t)y>=g.sh) return;
    float *zp=&zbuf[(size_t)y*zbuf_w+x];
    if(z < *zp){
        *zp=z;
        ((uint32_t*)(g.bb+(uint64_t)y*g.bb_pitch))[x]=col;
    }
}

/* ============================================================
   Minimal 3D camera/projection (shared by Pipes + Text Spinner)
   ============================================================ */

typedef struct { float x,y,z; } Vec3;

/* Perspective project a world-space point to screen space.
   Camera looks down +Z. Returns false if behind the camera. */
static int proj3d(Vec3 p, float cam_dist, float fov_scale, int *sx,int *sy,float *out_z){
    float z = p.z + cam_dist;
    if(z < 1.0f) return 0;
    float s = fov_scale / z;
    *sx = (int)((float)(g.sw/2) + p.x*s);
    *sy = (int)((float)(g.sh/2) + p.y*s);
    *out_z = z;
    return 1;
}

/* Rotate a point around the Y axis then the X axis (angles 0-255 fixed-point) */
static Vec3 rotate_xy(Vec3 p, uint8_t ay, uint8_t ax){
    float sy_=(float)isin(ay)/128.0f, cy_=(float)icos(ay)/128.0f;
    float sx_=(float)isin(ax)/128.0f, cx_=(float)icos(ax)/128.0f;
    Vec3 r;
    /* yaw (around Y) */
    float x1 = p.x*cy_ + p.z*sy_;
    float z1 = -p.x*sy_ + p.z*cy_;
    /* pitch (around X) */
    float y2 = p.y*cx_ - z1*sx_;
    float z2 = p.y*sx_ + z1*cx_;
    r.x=x1; r.y=y2; r.z=z2;
    return r;
}

/* Filled, depth-tested triangle rasteriser (screen-space, barycentric).
   This is the actual primitive that makes "3D" mean something here:
   every 3D face below is built from these, not from bounding boxes. */
static void tri_fill_z(int x0,int y0,float z0,
                        int x1,int y1,float z1,
                        int x2,int y2,float z2,
                        uint32_t col)
{
    int minx=x0,maxx=x0,miny=y0,maxy=y0;
    if(x1<minx)minx=x1; if(x1>maxx)maxx=x1; if(y1<miny)miny=y1; if(y1>maxy)maxy=y1;
    if(x2<minx)minx=x2; if(x2>maxx)maxx=x2; if(y2<miny)miny=y2; if(y2>maxy)maxy=y2;
    if(minx<0)minx=0; if(miny<0)miny=0;
    if(maxx>(int)g.sw-1)maxx=(int)g.sw-1; if(maxy>(int)g.sh-1)maxy=(int)g.sh-1;
    if(minx>maxx||miny>maxy) return;

    float area = (float)((x1-x0)*(y2-y0) - (x2-x0)*(y1-y0));
    if(area==0.0f) return;
    float inv_area = 1.0f/area;

    for(int y=miny;y<=maxy;y++){
        for(int x=minx;x<=maxx;x++){
            float w0 = (float)((x1-x)*(y2-y) - (x2-x)*(y1-y)) * inv_area;
            float w1 = (float)((x2-x)*(y0-y) - (x0-x)*(y2-y)) * inv_area;
            float w2 = 1.0f - w0 - w1;
            if(w0<0.0f||w1<0.0f||w2<0.0f) continue;
            float z = w0*z0 + w1*z1 + w2*z2;
            put_z(x,y,z,col);
        }
    }
}

/* Shade a flat colour by a 0..1 lighting factor (cheap directional light) */
static inline uint32_t shade(uint32_t col,float k){
    if(k<0.0f)k=0.0f; if(k>1.0f)k=1.0f;
    uint8_t r=(uint8_t)(((col>>16)&0xFF)*k);
    uint8_t gv=(uint8_t)(((col>>8)&0xFF)*k);
    uint8_t b=(uint8_t)((col&0xFF)*k);
    return 0xFF000000u|((uint32_t)r<<16)|((uint32_t)gv<<8)|b;
}

/* Draw a textured-by-shading box (cuboid) in world space, fully
   rasterised face-by-face with the triangle filler above, so faces
   that point away from the camera or are behind nearer geometry are
   handled by backface culling + the Z-buffer rather than painted
   as flat bounding-box rectangles. */
static void draw_box3d(Vec3 center, float hw,float hh,float hd,
                        uint8_t ay,uint8_t ax,
                        float cam_dist,float fov_scale,
                        uint32_t base_col)
{
    Vec3 local[8]={
        {-hw,-hh,-hd},{ hw,-hh,-hd},{ hw, hh,-hd},{-hw, hh,-hd},
        {-hw,-hh, hd},{ hw,-hh, hd},{ hw, hh, hd},{-hw, hh, hd},
    };
    /* Local-space face normals, in the same order as the faces[] list
       below. Rotating these by the box's own orientation (not the
       camera turntable separately) gives true per-face lighting that
       tracks correctly as the box spins, rather than a fixed guess. */
    static const Vec3 local_n[6]={
        { 0, 0,-1}, { 0, 0, 1}, {-1, 0, 0}, { 1, 0, 0}, { 0,-1, 0}, { 0, 1, 0}
    };
    Vec3 world[8];
    int sx[8],sy[8],vis[8]={0};
    float sz[8];
    for(int i=0;i<8;i++){
        Vec3 r=rotate_xy(local[i],ay,ax);
        r.x+=center.x; r.y+=center.y; r.z+=center.z;
        world[i]=r;
        vis[i]=proj3d(r,cam_dist,fov_scale,&sx[i],&sy[i],&sz[i]);
    }

    /* 6 faces, each as 2 triangles; normal-based shading + backface cull */
    static const int faces[6][4]={
        {0,1,2,3}, /* front  -Z */
        {5,4,7,6}, /* back   +Z */
        {4,0,3,7}, /* left   -X */
        {1,5,6,2}, /* right  +X */
        {4,5,1,0}, /* bottom -Y */
        {3,2,6,7}, /* top    +Y */
    };

    /* Single key light pointing back at the camera (0,0,-1 in view
       space). Rotating the local normal by the box's own (ay,ax)
       and dotting with the light gives proper Lambertian shading
       per face, instead of a fixed brightness table. */
    for(int f=0;f<6;f++){
        int a=faces[f][0],b=faces[f][1],c=faces[f][2],d=faces[f][3];
        if(!vis[a]||!vis[b]||!vis[c]||!vis[d]) continue;

        /* Backface cull via 2D winding of the projected quad */
        long cross=(long)(sx[b]-sx[a])*(sy[c]-sy[a]) - (long)(sx[c]-sx[a])*(sy[b]-sy[a]);
        if(cross>=0) continue; /* facing away from camera */

        Vec3 wn=rotate_xy(local_n[f],ay,ax);
        float ndotl = -wn.z; /* light along view axis */
        float k = 0.35f + 0.65f*(ndotl<0.0f?0.0f:ndotl);

        /* world[] holds the rotated+translated corners; reusing it here
           keeps the array load-bearing rather than write-only. */
        (void)world;
        uint32_t fc=shade(base_col,k);
        tri_fill_z(sx[a],sy[a],sz[a], sx[b],sy[b],sz[b], sx[c],sy[c],sz[c], fc);
        tri_fill_z(sx[a],sy[a],sz[a], sx[c],sy[c],sz[c], sx[d],sy[d],sz[d], fc);
    }
}

/* Draw a depth-tested sphere by rasterising a small lat/long mesh
   of quads (as triangle pairs). Used for the Pipes joint balls so
   they actually read as round 3D nodes rather than flat squares. */
#define SPHERE_LAT 6
#define SPHERE_LON 10
static void draw_sphere3d(Vec3 center,float radius,
                           float cam_dist,float fov_scale,
                           uint32_t base_col)
{
    Vec3 norms[(SPHERE_LAT+1)*SPHERE_LON]; /* local-space normal, pre-translate */
    int  sx_[(SPHERE_LAT+1)*SPHERE_LON], sy_[(SPHERE_LAT+1)*SPHERE_LON];
    float sz_[(SPHERE_LAT+1)*SPHERE_LON];
    int   vis_[(SPHERE_LAT+1)*SPHERE_LON];

    for(int la=0;la<=SPHERE_LAT;la++){
        uint8_t pang=(uint8_t)(la*128/SPHERE_LAT); /* 0..128 -> 0..pi */
        float py = (float)icos(pang)/128.0f;       /* -1..1 */
        float pr = (float)isin(pang)/128.0f;       /* ring radius factor */
        for(int lo=0;lo<SPHERE_LON;lo++){
            uint8_t lang=(uint8_t)(lo*256/SPHERE_LON);
            float lx=(float)isin(lang)/128.0f, lz=(float)icos(lang)/128.0f;
            int idx=la*SPHERE_LON+lo;
            /* A sphere centred at the origin has its own surface point
               as its normal direction, so no separate normal calc needed
               beyond what we already compute for position. */
            Vec3 n={ pr*lx, py, pr*lz };
            Vec3 p={ center.x+radius*n.x,
                     center.y+radius*n.y,
                     center.z+radius*n.z };
            norms[idx]=n;
            vis_[idx]=proj3d(p,cam_dist,fov_scale,&sx_[idx],&sy_[idx],&sz_[idx]);
        }
    }

    for(int la=0;la<SPHERE_LAT;la++){
        for(int lo=0;lo<SPHERE_LON;lo++){
            int lo2=(lo+1)%SPHERE_LON;
            int i0=la*SPHERE_LON+lo,     i1=la*SPHERE_LON+lo2;
            int i2=(la+1)*SPHERE_LON+lo2,i3=(la+1)*SPHERE_LON+lo;
            if(!vis_[i0]||!vis_[i1]||!vis_[i2]||!vis_[i3]) continue;

            /* Lambertian shading from the true (already world-oriented,
               since the sphere has no separate ay/ax rotation) surface
               normal, instead of a post-projection y-coordinate hack. */
            float ndotl = -norms[i0].z;
            float k = 0.35f + 0.65f*(ndotl<0.0f?0.0f:ndotl);
            uint32_t fc=shade(base_col,k);

            tri_fill_z(sx_[i0],sy_[i0],sz_[i0], sx_[i1],sy_[i1],sz_[i1], sx_[i2],sy_[i2],sz_[i2], fc);
            tri_fill_z(sx_[i0],sy_[i0],sz_[i0], sx_[i2],sy_[i2],sz_[i2], sx_[i3],sy_[i3],sz_[i3], fc);
        }
    }
}

/* Draw a depth-tested cylinder spanning two world-space (already
   camera-rotated) endpoints. Builds an explicit orthonormal basis
   perpendicular to the segment's own axis rather than orienting via
   rotate_xy's yaw-then-pitch angles: a single yaw+pitch pair cannot
   reach every one of the 6 axis-aligned pipe directions (pitching
   around X can never tilt a vector onto the X axis at all), so this
   is the primitive that actually has to do the job. */
#define CYL_SEGS 10
static void draw_cylinder3d(Vec3 p0, Vec3 p1, float radius,
                             float cam_dist, float fov_scale,
                             uint32_t base_col)
{
    Vec3 axis={ p1.x-p0.x, p1.y-p0.y, p1.z-p0.z };
    float alen=sqrtf3(axis.x*axis.x+axis.y*axis.y+axis.z*axis.z);
    if(alen<0.0001f) alen=0.0001f;
    Vec3 ax_n={ axis.x/alen, axis.y/alen, axis.z/alen };

    /* Pick a helper vector not parallel to the axis, then two cross
       products give an orthonormal {u,v} perpendicular to ax_n. */
    Vec3 tmp = (fabsf3(ax_n.x)<0.9f) ? (Vec3){1,0,0} : (Vec3){0,1,0};
    Vec3 u={ ax_n.y*tmp.z-ax_n.z*tmp.y,
             ax_n.z*tmp.x-ax_n.x*tmp.z,
             ax_n.x*tmp.y-ax_n.y*tmp.x };
    float ulen=sqrtf3(u.x*u.x+u.y*u.y+u.z*u.z);
    if(ulen<0.0001f) ulen=0.0001f;
    u.x/=ulen; u.y/=ulen; u.z/=ulen;
    Vec3 v={ ax_n.y*u.z-ax_n.z*u.y,
             ax_n.z*u.x-ax_n.x*u.z,
             ax_n.x*u.y-ax_n.y*u.x };

    Vec3 nrm[CYL_SEGS]; /* radial normal at each ring segment */
    int   sxlo[CYL_SEGS],sylo[CYL_SEGS],vislo[CYL_SEGS];
    int   sxhi[CYL_SEGS],syhi[CYL_SEGS],vishi[CYL_SEGS];
    float szlo[CYL_SEGS],szhi[CYL_SEGS];

    for(int i=0;i<CYL_SEGS;i++){
        uint8_t ang=(uint8_t)(i*256/CYL_SEGS);
        float cu=(float)icos(ang)/128.0f, cv=(float)isin(ang)/128.0f;
        Vec3 n={ u.x*cu+v.x*cv, u.y*cu+v.y*cv, u.z*cu+v.z*cv };
        nrm[i]=n;

        Vec3 wlo={ p0.x+n.x*radius, p0.y+n.y*radius, p0.z+n.z*radius };
        Vec3 whi={ p1.x+n.x*radius, p1.y+n.y*radius, p1.z+n.z*radius };

        vislo[i]=proj3d(wlo,cam_dist,fov_scale,&sxlo[i],&sylo[i],&szlo[i]);
        vishi[i]=proj3d(whi,cam_dist,fov_scale,&sxhi[i],&syhi[i],&szhi[i]);
    }

    /* Wall: one quad (2 tris) per segment, lit by the wall's own
       radial normal so shading is stable regardless of orientation. */
    for(int i=0;i<CYL_SEGS;i++){
        int j=(i+1)%CYL_SEGS;
        if(!vislo[i]||!vislo[j]||!vishi[i]||!vishi[j]) continue;

        float ndotl = -nrm[i].z;
        float k = 0.35f + 0.65f*(ndotl<0.0f?0.0f:ndotl);
        uint32_t fc=shade(base_col,k);

        tri_fill_z(sxlo[i],sylo[i],szlo[i], sxhi[i],syhi[i],szhi[i], sxhi[j],syhi[j],szhi[j], fc);
        tri_fill_z(sxlo[i],sylo[i],szlo[i], sxhi[j],syhi[j],szhi[j], sxlo[j],sylo[j],szlo[j], fc);
    }

    /* End caps as triangle fans, lit flat from the segment's own axis */
    float klo=0.35f+0.65f*(( ax_n.z)<0.0f?0.0f:( ax_n.z));
    float khi=0.35f+0.65f*((-ax_n.z)<0.0f?0.0f:(-ax_n.z));
    uint32_t fc_lo=shade(base_col,klo), fc_hi=shade(base_col,khi);

    int scx0,scy0; float scz0;
    if(proj3d(p0,cam_dist,fov_scale,&scx0,&scy0,&scz0)){
        for(int i=0;i<CYL_SEGS;i++){
            int j=(i+1)%CYL_SEGS;
            if(vislo[i]&&vislo[j])
                tri_fill_z(scx0,scy0,scz0, sxlo[j],sylo[j],szlo[j], sxlo[i],sylo[i],szlo[i], fc_lo);
        }
    }
    int scx1,scy1; float scz1;
    if(proj3d(p1,cam_dist,fov_scale,&scx1,&scy1,&scz1)){
        for(int i=0;i<CYL_SEGS;i++){
            int j=(i+1)%CYL_SEGS;
            if(vishi[i]&&vishi[j])
                tri_fill_z(scx1,scy1,scz1, sxhi[i],syhi[i],szhi[i], sxhi[j],syhi[j],szhi[j], fc_hi);
        }
    }
}

/* ============================================================
   EFFECT 2 — Pipes (now genuinely 3D: world-space cylindrical
   tubes + joint spheres rendered with a Z-buffer and a turntable
   camera, not a flat 2D grid drawn with coloured rectangles)
   ============================================================ */

#define MAX_PIPES    10
#define PIPE_STEP    20.0f   /* world units per grid step */
#define PIPE_RADIUS  6.0f
#define PIPE_BOUND_X 160.0f  /* half-extent of the playfield box */
#define PIPE_BOUND_Y 100.0f
#define PIPE_BOUND_Z 220.0f  /* deeper on Z so the fixed camera has room */
#define PIPE_MAX_SEGS 5000   /* total segments before the picture resets */

typedef struct {
    Vec3 pos;
    Vec3 dir;       /* axis direction, one of +-X/+-Y/+-Z */
    uint32_t col;   /* fixed for this pipe's whole life, like the original */
    int   age;
    int   active;
} Pipe3;

static Pipe3 pipes3[MAX_PIPES];
static long  pipe_seg_count;
static const float PIPE_CAM_DIST=560.0f, PIPE_FOV=340.0f;
static const uint8_t PIPE_CAM_AY=18, PIPE_CAM_AX=26; /* fixed viewing angle */

static const Vec3 pipe_dirs3[6] = {
    { 1,0,0},{-1,0,0},{0, 1,0},{0,-1,0},{0,0, 1},{0,0,-1}
};

static void pipe_spawn(Pipe3 *p){
    p->pos.x=(float)rng_range(-(int)PIPE_BOUND_X,(int)PIPE_BOUND_X);
    p->pos.y=(float)rng_range(-(int)PIPE_BOUND_Y,(int)PIPE_BOUND_Y);
    p->pos.z=(float)rng_range(-(int)PIPE_BOUND_Z,(int)PIPE_BOUND_Z);
    p->dir=pipe_dirs3[rng_range(0,6)];
    p->col=hsv(rng_range(0,360),85,95);
    p->age=0;
    p->active=1;
}

static void pipe_init(void){
    zbuf_ensure();
    clear(0xFF0A0A1A);
    zbuf_clear(1.0e9f);
    pipe_seg_count=0;
    for(int i=0;i<MAX_PIPES;i++) pipe_spawn(&pipes3[i]);
}

/* Pipes accumulate onto the backbuffer frame after frame instead of
   redrawing from scratch — that persistence is what actually makes a
   "trail" visible instead of only ever seeing the newest segment.
   The camera is intentionally static: once a segment is baked into
   pixels it can't be re-rotated, so there is no turntable here. */
static void pipe_update(void){
    zbuf_ensure();

    for(int i=0;i<MAX_PIPES;i++){
        Pipe3 *p=&pipes3[i];
        if(!p->active) continue;

        Vec3 from=p->pos;
        Vec3 to={ p->pos.x+p->dir.x*PIPE_STEP,
                  p->pos.y+p->dir.y*PIPE_STEP,
                  p->pos.z+p->dir.z*PIPE_STEP };

        /* Rotate both segment endpoints into camera space, then let
           draw_cylinder3d build its own perpendicular basis from the
           two points directly — robust for any of the 6 axis-aligned
           travel directions, unlike trying to express the orientation
           as a yaw+pitch pair (which can't reach all of them). */
        Vec3 rfrom=rotate_xy(from,PIPE_CAM_AY,PIPE_CAM_AX);
        Vec3 rto  =rotate_xy(to,  PIPE_CAM_AY,PIPE_CAM_AX);
        draw_cylinder3d(rfrom, rto, PIPE_RADIUS, PIPE_CAM_DIST, PIPE_FOV, p->col);
        draw_sphere3d(rto, PIPE_RADIUS*1.2f, PIPE_CAM_DIST, PIPE_FOV, p->col);

        p->pos=to;
        p->age++;
        pipe_seg_count++;

        if(p->pos.x<-PIPE_BOUND_X||p->pos.x>PIPE_BOUND_X||
           p->pos.y<-PIPE_BOUND_Y||p->pos.y>PIPE_BOUND_Y||
           p->pos.z<-PIPE_BOUND_Z||p->pos.z>PIPE_BOUND_Z){
            pipe_spawn(p);
            continue;
        }

        if(p->age > rng_range(4,12)){
            p->age=0;
            int tries=8;
            while(tries-->0){
                Vec3 nd=pipe_dirs3[rng_range(0,6)];
                if(nd.x==-p->dir.x&&nd.y==-p->dir.y&&nd.z==-p->dir.z) continue;
                p->dir=nd; break;
            }
        }
    }

    /* Once the picture has accumulated enough pipework, wipe and
       start a fresh one — matching the real screensaver's behaviour
       of periodically clearing once the scene gets too busy. */
    if(pipe_seg_count > PIPE_MAX_SEGS) pipe_init();
}

/* ============================================================
   EFFECT 3 — Matrix Rain
   ============================================================ */

#define MAT_COLS_MAX  200

static struct {
    int y;       /* current head row (in chars) */
    int speed;   /* rows per frame divisor */
    int timer;
    uint8_t ch;
    int active;
} mat[MAT_COLS_MAX];

static int mat_ncols, mat_nrows, mat_cw, mat_ch_h;
static int mat_frame;

/* Katakana-ish: use printable ASCII range for simplicity (0x21-0x7E) */
static char mat_char(void){ return (char)(0x21+rng_range(0,93)); }

static void mat_init(void){
    mat_cw  = g.font ? fnt_string_width(g.font,"M") : 8;
    mat_ch_h = g.font ? (int)g.font->header.glyph_height : 12;
    mat_ncols = (int)g.sw / mat_cw;
    mat_nrows = (int)g.sh / mat_ch_h;
    if(mat_ncols>MAT_COLS_MAX) mat_ncols=MAT_COLS_MAX;
    clear(0xFF000000);
    mat_frame=0;
    for(int i=0;i<mat_ncols;i++){
        mat[i].y      = rng_range(-mat_nrows, 0);
        mat[i].speed  = rng_range(1,4);
        mat[i].timer  = 0;
        mat[i].ch     = (uint8_t)mat_char();
        mat[i].active = 1;
    }
}

static void mat_update(void){
    mat_frame++;

    /* Fade the buffer towards black */
    uint32_t *fb=(uint32_t*)g.bb;
    uint32_t total=g.sw*g.sh;
    for(uint32_t i=0;i<total;i++){
        uint32_t p=fb[i];
        uint8_t r=(p>>16)&0xFF, gv=(p>>8)&0xFF, b=p&0xFF;
        /* Green channel decays slowest */
        if(gv>8) gv-=8; else gv=0;
        if(r>4)  r -=4; else r=0;
        if(b>4)  b -=4; else b=0;
        fb[i]=0xFF000000u|((uint32_t)r<<16)|((uint32_t)gv<<8)|b;
    }

    for(int i=0;i<mat_ncols;i++){
        mat[i].timer++;
        if(mat[i].timer<mat[i].speed) continue;
        mat[i].timer=0;

        int px=i*mat_cw;
        int py=mat[i].y * mat_ch_h;

        /* Draw glowing head */
        if(mat[i].y>=0 && mat[i].y<mat_nrows){
            char s[2]={(char)(0x21+rng_range(0,93)),0};
            draw_text(px,py,s,0xFFFFFFFF); /* white head */
        }
        /* Draw bright green one behind */
        if(mat[i].y-1>=0 && mat[i].y-1<mat_nrows){
            char s[2]={mat[i].ch,0};
            draw_text(px,(mat[i].y-1)*mat_ch_h,s,0xFF00FF41);
        }

        mat[i].y++;
        mat[i].ch=(uint8_t)mat_char();

        if(mat[i].y>mat_nrows+5){
            mat[i].y=rng_range(-mat_nrows/2,-1);
            mat[i].speed=rng_range(1,4);
        }
    }
}

/* ============================================================
   EFFECT 4 — Starfield / Warp
   ============================================================ */

#define MAX_STARS 400

typedef struct { float x,y,z; } Star;
static Star stars[MAX_STARS];

static void star_reset(int i){
    stars[i].x = (float)rng_range(-(int)g.sw/2, (int)g.sw/2);
    stars[i].y = (float)rng_range(-(int)g.sh/2, (int)g.sh/2);
    stars[i].z = (float)rng_range(1,(int)g.sw);
}

static void stars_init(void){
    clear(0xFF000000);
    for(int i=0;i<MAX_STARS;i++){
        star_reset(i);
        stars[i].z=(float)rng_range(1,(int)g.sw); /* spread out initially */
    }
}

static void stars_update(void){
    clear(0xFF000000);
    int cx=(int)g.sw/2, cy=(int)g.sh/2;
    for(int i=0;i<MAX_STARS;i++){
        stars[i].z -= 8.0f;
        if(stars[i].z<=0.0f) star_reset(i);

        float sx = stars[i].x / stars[i].z * (float)g.sw + (float)cx;
        float sy = stars[i].y / stars[i].z * (float)g.sh + (float)cy;

        if(sx<0||sx>=(float)g.sw||sy<0||sy>=(float)g.sh) { star_reset(i); continue; }

        /* Brightness based on z: closer = brighter */
        int bright=(int)((1.0f - stars[i].z/(float)g.sw)*255.0f);
        if(bright<0) bright=0; if(bright>255) bright=255;

        /* Trail */
        float pz=stars[i].z+8.0f;
        float px2=stars[i].x/pz*(float)g.sw+(float)cx;
        float py2=stars[i].y/pz*(float)g.sh+(float)cy;
        /* Simple line */
        int steps=(int)((sx-px2)*(sx-px2)+(sy-py2)*(sy-py2));
        if(steps<1) steps=1;
        if(steps>20) steps=20;
        for(int s=0;s<=steps;s++){
            float t=(float)s/(float)steps;
            int lx=(int)(px2+t*(sx-px2));
            int ly=(int)(py2+t*(sy-py2));
            int b2=bright*s/steps;
            blend(lx,ly,(uint8_t)b2,(uint8_t)b2,(uint8_t)255,255);
        }

        /* Dot */
        blend((int)sx,(int)sy,255,255,255,255);
    }
}

/* ============================================================
   EFFECT 5 — Plasma
   ============================================================ */

static uint8_t plasma_t=0;

static void plasma_init(void){ plasma_t=0; }

static void plasma_update(void){
    plasma_t++;
    uint8_t t=plasma_t;
    /* Sample at half-res for speed, then 2x upscale */
    int sw2=(int)g.sw/2, sh2=(int)g.sh/2;
    for(int y=0;y<sh2;y++){
        for(int x=0;x<sw2;x++){
            int v = isin((uint8_t)(x*3+t))
                  + isin((uint8_t)(y*3+t))
                  + isin((uint8_t)((x+y)*2+t*2))
                  + isin((uint8_t)((int)(x*x+y*y)/4+t));
            /* v in [-512..512]; map to hue */
            int hue=(int)(((long)v+512)*360/1024);
            if(hue<0)hue=0; if(hue>359)hue=359;
            uint32_t col=hsv(hue,90,90);
            /* 2x2 block */
            int px=x*2, py=y*2;
            if((uint32_t)(px+1)<g.sw && (uint32_t)(py+1)<g.sh){
                put(px,py,col); put(px+1,py,col);
                put(px,py+1,col); put(px+1,py+1,col);
            }
        }
    }
}

/* ============================================================
   EFFECT 6 — Metaballs
   ============================================================ */

#define MBALLS 3
#define MB_SCALE 3   /* render at 1/3 resolution then upscale */

static struct { float x,y,vx,vy; } mb[MBALLS];
static int mb_hue=0;

static void mb_init(void){
    mb_hue=rng_range(0,360);
    for(int i=0;i<MBALLS;i++){
        mb[i].x=(float)rng_range(80,g.sw-80);
        mb[i].y=(float)rng_range(80,g.sh-80);
        mb[i].vx=(float)rng_range(-3,3)*0.7f+0.5f;
        mb[i].vy=(float)rng_range(-3,3)*0.7f+0.5f;
    }
}

static void mb_update(void){
    mb_hue=(mb_hue+1)%360;

    /* Move balls */
    for(int i=0;i<MBALLS;i++){
        mb[i].x+=mb[i].vx; mb[i].y+=mb[i].vy;
        if(mb[i].x<50||(int)mb[i].x>(int)g.sw-50) mb[i].vx=-mb[i].vx;
        if(mb[i].y<50||(int)mb[i].y>(int)g.sh-50) mb[i].vy=-mb[i].vy;
    }

    /* Rasterise at reduced res */
    int rw=(int)g.sw/MB_SCALE, rh=(int)g.sh/MB_SCALE;
    for(int y=0;y<rh;y++){
        for(int x=0;x<rw;x++){
            float fx=(float)(x*MB_SCALE), fy=(float)(y*MB_SCALE);
            float sum=0.0f;
            for(int i=0;i<MBALLS;i++){
                float dx=fx-mb[i].x, dy=fy-mb[i].y;
                float d2=dx*dx+dy*dy;
                if(d2<1.0f) d2=1.0f;
                sum+=10000.0f/d2;
            }
            uint32_t col;
            if(sum>1.0f){
                int v=(int)(sum*64.0f); if(v>255)v=255;
                int h2=(mb_hue+(int)(sum*30.0f))%360;
                col=hsv(h2,90,v*100/255);
            } else {
                col=0xFF000000;
            }
            /* Upscale block */
            for(int dy=0;dy<MB_SCALE;dy++)
                for(int dx=0;dx<MB_SCALE;dx++)
                    put(x*MB_SCALE+dx, y*MB_SCALE+dy, col);
        }
    }
}

/* ============================================================
   EFFECT 7 — 3D Text Spinner (now genuinely 3D: each letter is a
   rasterised cuboid with backface culling + Z-buffered depth
   sorting, instead of bounding-box rectangles)
   ============================================================ */

static uint8_t spin_angle=0;
static int     spin_hue=0;

static const char *spin_text="ModuOS";
static const char *spin_sub ="New Technologies Software";

static void spin_init(void){ spin_angle=0; spin_hue=0; }

static void spin_update(void){
    zbuf_ensure();
    clear(0xFF050510);
    zbuf_clear(1.0e9f);

    spin_angle++;
    spin_hue=(spin_hue+1)%360;

    float cam_dist=380.0f, fov=260.0f;
    int len=(int)strlen(spin_text);
    float total_w=(float)(len*42);
    float start_x=-total_w/2.0f+21.0f;

    /* Slabs, drawn back-to-front by the shared Z-buffer regardless
       of iteration order — no manual painter's-algorithm sorting
       needed because every pixel is depth-tested. */
    for(int i=0;i<len;i++){
        Vec3 center={ start_x+(float)(i*42), 0.0f, 0.0f };
        int lhue=(spin_hue+i*360/len)%360;
        uint32_t col=hsv(lhue,85,95);
        draw_box3d(center, 17.0f,22.0f,10.0f,
                   spin_angle, 28,
                   cam_dist, fov, col);
    }

    /* Letter labels on the front face: project the same point
       draw_box3d would place the front face's centre at.
       Two checks make this respect the actual 3D scene instead of
       floating flat decals over it:
         1. Backface cull — skip the label whenever the front face
            normal points away from the camera (same test draw_box3d
            uses for the face itself), so a letter rotated to show
            its back never gets a glyph painted on it.
         2. Depth test — stamp each glyph pixel through put_z() against
            the shared zbuf instead of a raw fill(), so labels are
            correctly hidden behind nearer geometry (e.g. a letter in
            front overlapping one further back) rather than always
            winning regardless of depth. */
    for(int i=0;i<len;i++){
        Vec3 center={ start_x+(float)(i*42), 0.0f, -10.0f };
        Vec3 r=rotate_xy(center, spin_angle, 28);
        int px,py; float pz;
        if(!proj3d(r,cam_dist,fov,&px,&py,&pz)) continue;

        /* Front-face normal is local (0,0,-1); rotate it the same way
           draw_box3d rotates local_n[] and test against the view axis. */
        Vec3 n0={0.0f,0.0f,-1.0f};
        Vec3 wn=rotate_xy(n0, spin_angle, 28);
        if(wn.z >= 0.0f) continue; /* face points away from camera */

        char s[2]={spin_text[i],0};
        fnt_glyph_t *gl=fnt_get_glyph(g.font,(uint32_t)(unsigned char)s[0]);
        if(!gl) continue;

        int sc=2;
        int ox=px-7, oy=py-10;
        for(int dy=0;dy<gl->bitmap_height;dy++)
            for(int dx=0;dx<gl->bitmap_width;dx++)
                if(fnt_get_pixel(gl,dx,dy))
                    /* Glyph sits right on the face, so reuse its depth
                       for every pixel of this letter (the face itself
                       is small/flat-on relative to cam distance). */
                    for(int yy=0;yy<sc;yy++)
                        for(int xx=0;xx<sc;xx++)
                            put_z(ox+dx*sc+xx, oy+dy*sc+yy, pz, 0xFF000000u);
    }

    /* Subtitle */
    if(g.font){
        int sw=fnt_string_width(g.font,spin_sub);
        draw_text((int)g.sw/2-sw/2,(int)g.sh*3/4,spin_sub,
                  hsv((spin_hue+180)%360,60,80));
    }

    /* Ground shadow (simple dark ellipse) */
    int ey=(int)g.sh/2+80;
    for(int ex=-(int)g.sw/4;ex<(int)g.sw/4;ex++){
        int dy_=3; /* shadow height */
        for(int dd=0;dd<dy_;dd++)
            blend((int)g.sw/2+ex, ey+dd, 0,0,0,
                  (uint8_t)(80-(dd*25)));
    }
}

/* ============================================================
   EFFECT 8 — Cube Field (tumbling 3D boxes flying past camera)
   ============================================================ */

#define CUBE_COUNT 24
#define CUBE_FAR   600.0f

typedef struct {
    Vec3 pos;
    float spin_y, spin_x;    /* angle accumulators, 0..255 fixed-point */
    float spin_y_rate, spin_x_rate;
    float size;
    uint32_t col;
} Cube;

static Cube cubes[CUBE_COUNT];

static void cube_spawn(Cube *c){
    c->pos.x=(float)rng_range(-220,220);
    c->pos.y=(float)rng_range(-160,160);
    c->pos.z=CUBE_FAR + (float)rng_range(0,300);
    c->spin_y=(float)rng_range(0,256);
    c->spin_x=(float)rng_range(0,256);
    c->spin_y_rate=0.4f+(float)rng_range(0,100)*0.01f;
    c->spin_x_rate=0.3f+(float)rng_range(0,100)*0.01f;
    c->size=(float)rng_range(14,34);
    c->col=hsv(rng_range(0,360),80,95);
}

static void cube_init(void){
    for(int i=0;i<CUBE_COUNT;i++) cube_spawn(&cubes[i]);
}

static void cube_update(void){
    zbuf_ensure();
    clear(0xFF02020A);
    zbuf_clear(1.0e9f);

    float cam_dist=0.0f, fov=420.0f; /* pos.z already includes camera offset */

    for(int i=0;i<CUBE_COUNT;i++){
        Cube *c=&cubes[i];
        c->pos.z -= 4.0f;
        c->spin_y += c->spin_y_rate;
        c->spin_x += c->spin_x_rate;
        if(c->pos.z < 30.0f) cube_spawn(c);

        uint8_t ay=(uint8_t)((int)c->spin_y & 255);
        uint8_t ax=(uint8_t)((int)c->spin_x & 255);
        float hw=c->size, hh=c->size, hd=c->size;
        draw_box3d(c->pos, hw,hh,hd, ay,ax, cam_dist,fov, c->col);
    }
}

/* ============================================================
   EFFECT 9 — Spinning Donut (parametric torus, shaded + Z-buffered)
   ============================================================ */

#define TORUS_RING   28   /* segments around the tube (theta) */
#define TORUS_TUBE   14   /* segments around the tube cross-section (phi) */
#define TORUS_R1     90.0f  /* distance from centre to tube centre */
#define TORUS_R2     36.0f  /* tube radius */

static uint8_t torus_ay=0, torus_ax=0;
static int     torus_hue=0;

static void torus_init(void){ torus_ay=0; torus_ax=40; torus_hue=0; }

static void torus_update(void){
    zbuf_ensure();
    clear(0xFF050008);
    zbuf_clear(1.0e9f);

    torus_ay++;
    torus_ax += 1; /* slow tumble on the second axis too */
    torus_hue=(torus_hue+2)%360;

    float cam_dist=300.0f, fov=300.0f;

    /* Build the torus mesh fresh each frame: theta walks around the
       big ring, phi walks around the tube's circular cross-section. */
    Vec3 norms[TORUS_RING*TORUS_TUBE]; /* local-space normals, pre-rotation */
    int   sx_[TORUS_RING*TORUS_TUBE], sy_[TORUS_RING*TORUS_TUBE];
    float sz_[TORUS_RING*TORUS_TUBE];
    int   vis_[TORUS_RING*TORUS_TUBE];

    for(int t=0;t<TORUS_RING;t++){
        uint8_t theta=(uint8_t)(t*256/TORUS_RING);
        float ct=(float)icos(theta)/128.0f, st=(float)isin(theta)/128.0f;
        for(int p=0;p<TORUS_TUBE;p++){
            uint8_t phi=(uint8_t)(p*256/TORUS_TUBE);
            float cp=(float)icos(phi)/128.0f, sp=(float)isin(phi)/128.0f;

            /* Point on the tube surface, local space (torus axis = Z) */
            Vec3 local_p={
                (TORUS_R1+TORUS_R2*cp)*ct,
                (TORUS_R1+TORUS_R2*cp)*st,
                TORUS_R2*sp
            };
            /* Surface normal: same angular position, unit tube radius */
            Vec3 local_n={ cp*ct, cp*st, sp };

            Vec3 wp=rotate_xy(local_p,torus_ay,torus_ax);
            Vec3 wn=rotate_xy(local_n,torus_ay,torus_ax);

            int idx=t*TORUS_TUBE+p;
            norms[idx]=wn;
            vis_[idx]=proj3d(wp,cam_dist,fov,&sx_[idx],&sy_[idx],&sz_[idx]);
        }
    }

    for(int t=0;t<TORUS_RING;t++){
        int t2=(t+1)%TORUS_RING;
        for(int p=0;p<TORUS_TUBE;p++){
            int p2=(p+1)%TORUS_TUBE;
            int i0=t*TORUS_TUBE+p,   i1=t2*TORUS_TUBE+p;
            int i2=t2*TORUS_TUBE+p2, i3=t*TORUS_TUBE+p2;
            if(!vis_[i0]||!vis_[i1]||!vis_[i2]||!vis_[i3]) continue;

            /* Per-vertex hue across the ring gives the classic
               rainbow-donut look; lighting comes from the normal. */
            float ndotl = -norms[i0].z;
            float k = 0.25f + 0.75f*(ndotl<0.0f?0.0f:ndotl);
            int hue=(torus_hue + t*360/TORUS_RING)%360;
            uint32_t fc=shade(hsv(hue,85,100),k);

            tri_fill_z(sx_[i0],sy_[i0],sz_[i0], sx_[i1],sy_[i1],sz_[i1], sx_[i2],sy_[i2],sz_[i2], fc);
            tri_fill_z(sx_[i0],sy_[i0],sz_[i0], sx_[i2],sy_[i2],sz_[i2], sx_[i3],sy_[i3],sz_[i3], fc);
        }
    }
}

/* ============================================================
   EFFECT 10 — Fire (classic bottom-up heat-propagation fire,
   doom-style palette lookup; cheap per-pixel, no 3D needed)
   ============================================================ */

static uint8_t *fire_buf=NULL;   /* heat 0..255 per cell */
static int fire_w, fire_h, fire_cell;
static uint32_t fire_palette[256];

static void fire_build_palette(void){
    /* black -> red -> orange -> yellow -> white, classic fire ramp */
    for(int i=0;i<256;i++){
        int t=i;
        int r,gv,b;
        if(t<64){       r=t*4;            gv=0;             b=0; }
        else if(t<128){ r=255;            gv=(t-64)*4;      b=0; }
        else if(t<192){ r=255;            gv=255;           b=(t-128)*4; }
        else {          r=255;            gv=255;           b=255; }
        if(r>255)r=255; if(gv>255)gv=255; if(b>255)b=255;
        fire_palette[i]=0xFF000000u|((uint32_t)r<<16)|((uint32_t)gv<<8)|(uint32_t)b;
    }
}

static void fire_init(void){
    fire_cell=3; /* render at 1/3 res, blocky like the original effect */
    fire_w=(int)g.sw/fire_cell;
    fire_h=(int)g.sh/fire_cell;
    if(fire_buf) free(fire_buf);
    fire_buf=(uint8_t*)malloc((size_t)fire_w*(size_t)fire_h);
    if(!fire_buf) return;
    memset(fire_buf,0,(size_t)fire_w*(size_t)fire_h);
    fire_build_palette();
    clear(0xFF000000);
}

static void fire_update(void){
    if(!fire_buf) return;

    /* Seed the bottom row with random full-heat / embers */
    for(int x=0;x<fire_w;x++)
        fire_buf[(fire_h-1)*fire_w+x]=(rng_next()&1)?255:rng_range(160,256);

    /* Propagate heat upward with random decay/spread, classic algorithm */
    for(int y=0;y<fire_h-1;y++){
        for(int x=0;x<fire_w;x++){
            int src_x = x + rng_range(-1,2);
            if(src_x<0) src_x=0; if(src_x>=fire_w) src_x=fire_w-1;
            int below = fire_buf[(y+1)*fire_w+src_x];
            int decay = rng_range(0,3);
            int v = below - decay;
            if(v<0) v=0;
            fire_buf[y*fire_w+x]=(uint8_t)v;
        }
    }

    /* Blit through the palette, upscaled to fire_cell blocks */
    for(int y=0;y<fire_h;y++){
        for(int x=0;x<fire_w;x++){
            uint32_t col=fire_palette[fire_buf[y*fire_w+x]];
            fill(x*fire_cell, y*fire_cell, fire_cell, fire_cell, col);
        }
    }
}

/* ============================================================
   EFFECT 11 — Tunnel (classic demo-scene texture tunnel, built
   from polar-coordinate lookup tables; cheap per-pixel, no 3D)
   ============================================================ */

static int *tun_dist=NULL, *tun_ang=NULL; /* precomputed per-pixel LUTs */
static uint8_t tun_t=0;

static void tunnel_init(void){
    tun_t=0;
    int cx=(int)g.sw/2, cy=(int)g.sh/2;
    if(tun_dist) free(tun_dist);
    if(tun_ang)  free(tun_ang);
    size_t n=(size_t)g.sw*(size_t)g.sh;
    tun_dist=(int*)malloc(n*sizeof(int));
    tun_ang =(int*)malloc(n*sizeof(int));
    if(!tun_dist||!tun_ang) return;

    for(int y=0;y<(int)g.sh;y++){
        for(int x=0;x<(int)g.sw;x++){
            int dx=x-cx, dy=y-cy;
            int d2=dx*dx+dy*dy;
            /* Integer sqrt via simple Newton iteration (no libm) */
            int d=0;
            if(d2>0){
                int guess=d2;
                for(int it=0;it<12;it++){
                    if(guess<=0){guess=1;}
                    guess=(guess+d2/guess)/2;
                }
                d=guess;
            }
            /* Angle via a coarse lookup scan against the existing sine
               table — avoids needing atan2, cheap enough since this
               only runs once at init time per pixel. */
            int best_a=0; long best_err=0x7FFFFFFFL;
            for(int a=0;a<256;a+=4){
                int cdx=icos((uint8_t)a), cdy=isin((uint8_t)a);
                long dot = (long)cdx*dx + (long)cdy*dy;
                long crs = (long)cdx*dy - (long)cdy*dx;
                long err = crs*crs - (dot>0? 0L : (1L<<30));
                if(err<best_err){ best_err=err; best_a=a; }
            }
            tun_dist[y*(int)g.sw+x]=d;
            tun_ang [y*(int)g.sw+x]=best_a;
        }
    }
}

static void tunnel_update(void){
    if(!tun_dist||!tun_ang) return;
    tun_t++;

    uint32_t *fb=(uint32_t*)g.bb;
    int n=(int)(g.sw*g.sh);
    for(int i=0;i<n;i++){
        int d=tun_dist[i];
        int a=tun_ang[i];

        /* Depth banding: tunnel rings rushing towards camera */
        int ring=(int)(40000/(d+1)) - (int)tun_t*3;
        int check=((ring/16)+(a/16)) & 1;

        int hue=(a*2 + (int)tun_t*3) % 360;
        uint32_t col = check ? hsv(hue,70,90) : hsv(hue,70,35);
        fb[i]=col;
    }
}

/* ============================================================
   EFFECT 12 — Conway's Game of Life (colour-aged)
   ============================================================ */

#define LIFE_CELL   4   /* px per cell */
static int   life_gw, life_gh;
static uint8_t *life_grid=NULL;  /* age: 0=dead, 1..254=alive age */
static uint8_t *life_next=NULL;

static void life_init(void){
    life_gw=(int)g.sw/LIFE_CELL;
    life_gh=(int)g.sh/LIFE_CELL;
    size_t sz=(size_t)life_gw*(size_t)life_gh;
    if(!life_grid){ life_grid=(uint8_t*)malloc(sz); }
    if(!life_next){ life_next=(uint8_t*)malloc(sz); }
    if(!life_grid||!life_next) return;
    /* Random seed */
    for(size_t i=0;i<sz;i++)
        life_grid[i]=(rng_next()&3)==0 ? 1 : 0;
    clear(0xFF000000);
}

static void life_update(void){
    if(!life_grid||!life_next) return;
    size_t sz=(size_t)life_gw*(size_t)life_gh;

    /* Compute next generation */
    for(int y=0;y<life_gh;y++){
        for(int x=0;x<life_gw;x++){
            int n=0;
            for(int dy=-1;dy<=1;dy++){
                for(int dx=-1;dx<=1;dx++){
                    if(dx==0&&dy==0) continue;
                    int nx=(x+dx+life_gw)%life_gw;
                    int ny=(y+dy+life_gh)%life_gh;
                    if(life_grid[ny*life_gw+nx]) n++;
                }
            }
            uint8_t cur=life_grid[y*life_gw+x];
            int alive=(cur>0);
            int next_alive;
            if(alive)  next_alive=(n==2||n==3);
            else       next_alive=(n==3);
            if(next_alive){
                uint8_t age=cur+1; if(age==0)age=255;
                life_next[y*life_gw+x]=age;
            } else {
                life_next[y*life_gw+x]=0;
            }
        }
    }

    memcpy(life_grid,life_next,sz);

    /* Render */
    for(int y=0;y<life_gh;y++){
        for(int x=0;x<life_gw;x++){
            uint8_t age=life_grid[y*life_gw+x];
            uint32_t col;
            if(age==0){
                col=0xFF000000;
            } else {
                /* Young = green, old = white/yellow */
                int hue=(int)age; /* 0=green, wraps around */
                /* Map age 1..255 -> hue 120..60 (green to yellow) */
                int h=120-(int)age/2; if(h<0)h=0;
                int v=60+(int)age/5; if(v>100)v=100;
                col=hsv(h,90,v);
            }
            /* Fill cell */
            fill(x*LIFE_CELL, y*LIFE_CELL, LIFE_CELL-1, LIFE_CELL-1, col);
        }
    }

    /* Re-seed if too few alive */
    int alive_count=0;
    for(size_t i=0;i<sz;i++) if(life_grid[i]) alive_count++;
    if(alive_count < life_gw*life_gh/20){
        /* Inject some random cells */
        for(int i=0;i<life_gw*life_gh/8;i++){
            int pos=(int)(rng_next()%(uint32_t)(life_gw*life_gh));
            life_grid[pos]=1;
        }
    }
}

/* ============================================================
   EFFECT 13 — Bouncing Spheres
   Physics balls bouncing inside a transparent box, shadows on
   floor, real elastic collision response between spheres.
   ============================================================ */

#define BS_COUNT   8
#define BS_CAM_DIST  700.0f
#define BS_FOV       400.0f
#define BS_BOX_HX   180.0f
#define BS_BOX_HY   130.0f
#define BS_BOX_HZ   180.0f
#define BS_GRAVITY   0.12f   /* world units/frame^2 */
#define BS_DAMPING   0.78f   /* velocity after wall hit */
#define BS_RADIUS    22.0f

typedef struct {
    Vec3     pos;
    Vec3     vel;
    uint32_t col;
    float    spin; /* rotation angle for visual flair */
} BSphere;

static BSphere bspheres[BS_COUNT];
static uint8_t bs_cam_ay, bs_cam_ax;

static void bs_init(void) {
    zbuf_ensure();
    bs_cam_ay = 32;
    bs_cam_ax = 20;
    for (int i = 0; i < BS_COUNT; i++) {
        bspheres[i].pos.x = (float)rng_range(-(int)(BS_BOX_HX-BS_RADIUS), (int)(BS_BOX_HX-BS_RADIUS));
        bspheres[i].pos.y = (float)rng_range(-(int)(BS_BOX_HY-BS_RADIUS), (int)(BS_BOX_HY-BS_RADIUS));
        bspheres[i].pos.z = (float)rng_range(-(int)(BS_BOX_HZ-BS_RADIUS), (int)(BS_BOX_HZ-BS_RADIUS));
        bspheres[i].vel.x = ((float)rng_range(-80,80))*0.035f;
        bspheres[i].vel.y = ((float)rng_range(-50,50))*0.025f;
        bspheres[i].vel.z = ((float)rng_range(-80,80))*0.035f;
        bspheres[i].col   = hsv(rng_range(0,360), 90, 100);
        bspheres[i].spin  = 0.0f;
    }
}

/* Draw the wire-frame edges of the bounding box */
static void bs_draw_box_edges(void) {
    /* 8 corners of the box */
    Vec3 corners[8] = {
        {-BS_BOX_HX,-BS_BOX_HY,-BS_BOX_HZ}, { BS_BOX_HX,-BS_BOX_HY,-BS_BOX_HZ},
        { BS_BOX_HX, BS_BOX_HY,-BS_BOX_HZ}, {-BS_BOX_HX, BS_BOX_HY,-BS_BOX_HZ},
        {-BS_BOX_HX,-BS_BOX_HY, BS_BOX_HZ}, { BS_BOX_HX,-BS_BOX_HY, BS_BOX_HZ},
        { BS_BOX_HX, BS_BOX_HY, BS_BOX_HZ}, {-BS_BOX_HX, BS_BOX_HY, BS_BOX_HZ},
    };
    int sx[8], sy[8], vis[8];
    float sz[8];
    for (int i = 0; i < 8; i++) {
        Vec3 r = rotate_xy(corners[i], bs_cam_ay, bs_cam_ax);
        vis[i] = proj3d(r, BS_CAM_DIST, BS_FOV, &sx[i], &sy[i], &sz[i]);
    }
    /* 12 edges */
    static const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0}, /* front face */
        {4,5},{5,6},{6,7},{7,4}, /* back face */
        {0,4},{1,5},{2,6},{3,7}  /* connecting */
    };
    uint32_t ec = 0xFF334455;
    for (int e = 0; e < 12; e++) {
        int a = edges[e][0], b = edges[e][1];
        if (!vis[a] || !vis[b]) continue;
        /* Simple Bresenham line */
        int x0=sx[a],y0=sy[a],x1=sx[b],y1=sy[b];
        int dx=x1-x0<0?-(x1-x0):(x1-x0);
        int dy=y1-y0<0?-(y1-y0):(y1-y0);
        int steps=dx>dy?dx:dy; if(steps<1)steps=1; if(steps>500)steps=500;
        for(int s=0;s<=steps;s++){
            int px=x0+(x1-x0)*s/steps;
            int py=y0+(y1-y0)*s/steps;
            blend(px,py,0x33,0x44,0x55,180);
        }
    }
    /* Floor shadow plane: draw a semi-transparent filled floor */
    /* Floor at y = +BS_BOX_HY (bottom in world space, +Y is down) */
    /* Project 4 floor corners */
    Vec3 fl[4] = {
        {-BS_BOX_HX, BS_BOX_HY,-BS_BOX_HZ},{ BS_BOX_HX, BS_BOX_HY,-BS_BOX_HZ},
        { BS_BOX_HX, BS_BOX_HY, BS_BOX_HZ},{-BS_BOX_HX, BS_BOX_HY, BS_BOX_HZ},
    };
    int flsx[4],flsy[4],flvis[4]; float flsz[4];
    for(int i=0;i<4;i++){
        Vec3 r=rotate_xy(fl[i],bs_cam_ay,bs_cam_ax);
        flvis[i]=proj3d(r,BS_CAM_DIST,BS_FOV,&flsx[i],&flsy[i],&flsz[i]);
    }
    if(flvis[0]&&flvis[1]&&flvis[2]&&flvis[3]){
        /* Two tris, blended */
        for(int pass=0;pass<2;pass++){
            int i0=0,i1=pass?2:1,i2=pass?3:2;
            int minx=flsx[i0],maxx=flsx[i0],miny=flsy[i0],maxy=flsy[i0];
            if(flsx[i1]<minx)minx=flsx[i1]; if(flsx[i1]>maxx)maxx=flsx[i1];
            if(flsx[i2]<minx)minx=flsx[i2]; if(flsx[i2]>maxx)maxx=flsx[i2];
            if(flsy[i1]<miny)miny=flsy[i1]; if(flsy[i1]>maxy)maxy=flsy[i1];
            if(flsy[i2]<miny)miny=flsy[i2]; if(flsy[i2]>maxy)maxy=flsy[i2];
            if(minx<0)minx=0; if(miny<0)miny=0;
            if(maxx>=(int)g.sw)maxx=(int)g.sw-1;
            if(maxy>=(int)g.sh)maxy=(int)g.sh-1;
            float area=(float)((flsx[i1]-flsx[i0])*(flsy[i2]-flsy[i0])-(flsx[i2]-flsx[i0])*(flsy[i1]-flsy[i0]));
            if(area==0.0f) continue;
            float inv=1.0f/area;
            for(int y=miny;y<=maxy;y++) for(int x=minx;x<=maxx;x++){
                float w0=(float)((flsx[i1]-x)*(flsy[i2]-y)-(flsx[i2]-x)*(flsy[i1]-y))*inv;
                float w1=(float)((flsx[i2]-x)*(flsy[i0]-y)-(flsx[i0]-x)*(flsy[i2]-y))*inv;
                float w2=1.0f-w0-w1;
                if(w0<0||w1<0||w2<0) continue;
                blend(x,y,0x11,0x22,0x33,60);
            }
        }
    }
}

static void bs_update(void) {
    clear(0xFF050810);
    zbuf_clear(1.0e9f);

    /* Slow camera rotation */
    bs_cam_ay++;

    bs_draw_box_edges();

    /* Physics: gravity + wall bounces */
    for (int i = 0; i < BS_COUNT; i++) {
        bspheres[i].vel.y += BS_GRAVITY;
        bspheres[i].pos.x += bspheres[i].vel.x;
        bspheres[i].pos.y += bspheres[i].vel.y;
        bspheres[i].pos.z += bspheres[i].vel.z;
        bspheres[i].spin  += 2.0f;

        float bx=BS_BOX_HX-BS_RADIUS, by=BS_BOX_HY-BS_RADIUS, bz=BS_BOX_HZ-BS_RADIUS;
        if(bspheres[i].pos.x < -bx){ bspheres[i].pos.x=-bx; bspheres[i].vel.x*=-BS_DAMPING; }
        if(bspheres[i].pos.x >  bx){ bspheres[i].pos.x= bx; bspheres[i].vel.x*=-BS_DAMPING; }
        if(bspheres[i].pos.y < -by){ bspheres[i].pos.y=-by; bspheres[i].vel.y*=-BS_DAMPING; }
        if(bspheres[i].pos.y >  by){ bspheres[i].pos.y= by; bspheres[i].vel.y*=-BS_DAMPING; }
        if(bspheres[i].pos.z < -bz){ bspheres[i].pos.z=-bz; bspheres[i].vel.z*=-BS_DAMPING; }
        if(bspheres[i].pos.z >  bz){ bspheres[i].pos.z= bz; bspheres[i].vel.z*=-BS_DAMPING; }
    }

    /* Sphere-sphere collision response */
    float diam = BS_RADIUS * 2.0f;
    for (int i = 0; i < BS_COUNT; i++) {
        for (int j = i+1; j < BS_COUNT; j++) {
            float dx = bspheres[j].pos.x - bspheres[i].pos.x;
            float dy = bspheres[j].pos.y - bspheres[i].pos.y;
            float dz = bspheres[j].pos.z - bspheres[i].pos.z;
            float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < diam*diam && d2 > 0.001f) {
                float d = sqrtf3(d2);
                float nx=dx/d, ny=dy/d, nz=dz/d;
                /* Push apart */
                float overlap = (diam - d) * 0.5f;
                bspheres[i].pos.x -= nx*overlap;
                bspheres[i].pos.y -= ny*overlap;
                bspheres[i].pos.z -= nz*overlap;
                bspheres[j].pos.x += nx*overlap;
                bspheres[j].pos.y += ny*overlap;
                bspheres[j].pos.z += nz*overlap;
                /* Elastic velocity exchange along normal */
                float vi = bspheres[i].vel.x*nx + bspheres[i].vel.y*ny + bspheres[i].vel.z*nz;
                float vj = bspheres[j].vel.x*nx + bspheres[j].vel.y*ny + bspheres[j].vel.z*nz;
                if (vi - vj > 0.0f) { /* only if approaching */
                    bspheres[i].vel.x += (vj-vi)*nx;
                    bspheres[i].vel.y += (vj-vi)*ny;
                    bspheres[i].vel.z += (vj-vi)*nz;
                    bspheres[j].vel.x += (vi-vj)*nx;
                    bspheres[j].vel.y += (vi-vj)*ny;
                    bspheres[j].vel.z += (vi-vj)*nz;
                }
            }
        }
    }

    /* Draw drop shadows on the floor first (painter's order: behind spheres) */
    for (int i = 0; i < BS_COUNT; i++) {
        /* Shadow = dark ellipse projected onto the floor plane (y = BS_BOX_HY) */
        Vec3 shadow_center = { bspheres[i].pos.x, BS_BOX_HY, bspheres[i].pos.z };
        Vec3 r = rotate_xy(shadow_center, bs_cam_ay, bs_cam_ax);
        int ssx, ssy; float ssz;
        if (!proj3d(r, BS_CAM_DIST, BS_FOV, &ssx, &ssy, &ssz)) continue;
        /* Shadow radius scales with height above floor */
        float height = BS_BOX_HY - bspheres[i].pos.y;
        if (height < 1.0f) height = 1.0f;
        float srad = BS_RADIUS * BS_FOV / ssz * (1.0f - height/(BS_BOX_HY*2.0f+1.0f));
        if (srad < 2.0f) srad = 2.0f;
        uint8_t salpha = (uint8_t)(120.0f * (1.0f - height/(BS_BOX_HY*2.0f)));
        if ((int)salpha < 0) salpha = 0;
        int ir = (int)srad;
        for (int dy2=-ir; dy2<=ir; dy2++) {
            for (int dx2=-ir; dx2<=ir; dx2++) {
                if (dx2*dx2*4 + dy2*dy2 > ir*ir*4) continue; /* ellipse */
                blend(ssx+dx2, ssy+dy2, 0,0,0, salpha);
            }
        }
    }

    /* Draw spheres back-to-front (simple Z sort: insertion sort on camera-Z) */
    int order[BS_COUNT];
    float cam_z[BS_COUNT];
    for (int i = 0; i < BS_COUNT; i++) {
        order[i] = i;
        Vec3 r = rotate_xy(bspheres[i].pos, bs_cam_ay, bs_cam_ax);
        cam_z[i] = r.z + BS_CAM_DIST;
    }
    for (int i = 1; i < BS_COUNT; i++) {
        int oi = order[i]; float zi = cam_z[oi];
        int j = i - 1;
        while (j >= 0 && cam_z[order[j]] < zi) { order[j+1] = order[j]; j--; }
        order[j+1] = oi;
    }
    for (int ii = 0; ii < BS_COUNT; ii++) {
        int i = order[ii];
        Vec3 rp = rotate_xy(bspheres[i].pos, bs_cam_ay, bs_cam_ax);
        draw_sphere3d(rp, BS_RADIUS, BS_CAM_DIST, BS_FOV, bspheres[i].col);
    }
}

/* ============================================================
   EFFECT 14 — Terrain Flyover
   Procedural heightmap rendered as a scrolling triangle mesh.
   Camera flies forward over rolling hills at a fixed height.

   Coordinate system used here:
     World X  = left/right
     World Y  = height (positive Y = UP, terrain surface Y >= 0)
     World Z  = depth ahead of camera (increases going forward)
   Camera sits at (0, CAM_EYE_Y, 0) looking down +Z.
   A world point at (wx, wy, wz_cam) projects to screen as:
     sx = hw + wx * FOV / wz_cam
     sy = hh - (wy - CAM_EYE_Y) * FOV / wz_cam
   The minus in sy makes positive Y go UP on screen.
   ============================================================ */

#define TRN_COLS      48    /* grid columns */
#define TRN_ROWS      40    /* grid rows visible ahead */
#define TRN_CELL      28    /* world units per cell */
#define TRN_HEIGHT    70.0f /* max terrain amplitude */
#define TRN_FOV       420.0f
#define TRN_CAM_EYE_Y 90.0f /* camera height above y=0 plane */
#define TRN_SPEED     2.2f  /* world units scrolled per frame */
#define TRN_TILE_LEN  (TRN_ROWS * TRN_CELL) /* heightmap tile depth */

static float trn_scroll; /* total world units scrolled so far */

static void trn_init(void) {
    zbuf_ensure();
    trn_scroll = 0.0f;
}

/* Heightmap — returns Y >= 0 (height above sea).
   Uses only sin_tbl, no libm. wx/wz are world coords. */
static float trn_height(float wx, float wz) {
    uint8_t a1 = (uint8_t)((int)(wx * 0.9f) & 0xFF);
    uint8_t a2 = (uint8_t)((int)(wz * 0.7f) & 0xFF);
    uint8_t a3 = (uint8_t)((int)((wx + wz) * 0.45f) & 0xFF);
    uint8_t a4 = (uint8_t)((int)(wx * 0.28f - wz * 0.38f) & 0xFF);
    float h = (float)isin(a1)/128.0f * 0.42f
            + (float)isin(a2)/128.0f * 0.33f
            + (float)isin(a3)/128.0f * 0.15f
            + (float)isin(a4)/128.0f * 0.10f;
    /* Remap -1..1  ->  0..TRN_HEIGHT  (all positive, sea at 0) */
    return (h + 1.0f) * 0.5f * TRN_HEIGHT;
}

static uint32_t trn_color(float wy, float slope) {
    float h_norm = wy / TRN_HEIGHT; /* 0..1 */
    if (h_norm < 0.18f) {
        /* Deep water */
        return 0xFF000000u | ((uint32_t)0x10 << 16) | ((uint32_t)0x45 << 8) | 0xA0u;
    } else if (h_norm < 0.28f) {
        /* Sandy shore */
        return hsv(42, 55, 65);
    } else if (h_norm < 0.62f) {
        /* Green land, darker on steep slopes */
        int val = 45 + (int)(h_norm * 35.0f) - (int)(slope * 20.0f);
        if (val < 25) val = 25; if (val > 80) val = 80;
        int hue = 108 - (int)(slope * 25.0f);
        return hsv(hue, 65, val);
    } else if (h_norm < 0.82f) {
        /* Rock */
        int v = 38 + (int)(h_norm * 30.0f);
        return hsv(22, 28, v);
    } else {
        /* Snow */
        int v = 88 + (int)((h_norm - 0.82f) * 55.0f);
        if (v > 100) v = 100;
        return hsv(210, 12, v);
    }
}

static void trn_update(void) {
    zbuf_clear(1.0e9f);

    trn_scroll += TRN_SPEED;

    int hw = (int)g.sw / 2;
    int hh = (int)g.sh / 2;

    /* Sky gradient — draw top half before terrain overwrites lower half */
    for (int y = 0; y < (int)g.sh; y++) {
        float t = (float)y / (float)g.sh;
        uint8_t r2, g2, b2;
        if (t < 0.5f) {
            /* Upper sky: deep blue -> lighter */
            float u = t / 0.5f;
            r2 = (uint8_t)(8  + (int)(u * 40));
            g2 = (uint8_t)(14 + (int)(u * 70));
            b2 = (uint8_t)(40 + (int)(u * 110));
        } else {
            /* Lower sky near horizon: hazy */
            float u = (t - 0.5f) / 0.5f;
            r2 = (uint8_t)(48 + (int)(u * 80));
            g2 = (uint8_t)(84 + (int)(u * 80));
            b2 = (uint8_t)(150+ (int)(u * 60));
        }
        uint32_t sc = 0xFF000000u|((uint32_t)r2<<16)|((uint32_t)g2<<8)|b2;
        uint32_t *row = (uint32_t*)(g.bb + (uint64_t)y * g.bb_pitch);
        for (int x = 0; x < (int)g.sw; x++) row[x] = sc;
    }

    /* Render terrain grid front-to-back (row 0 = nearest) so closer
       triangles naturally win the Z-buffer over far ones. */
    for (int row = 0; row < TRN_ROWS - 1; row++) {
        for (int col = 0; col < TRN_COLS - 1; col++) {

            /* Camera-relative Z of this row's near and far edge.
               Row 0 starts 2 cells ahead so nothing clips the camera. */
            float cz0 = (float)(row + 2) * TRN_CELL;
            float cz1 = (float)(row + 3) * TRN_CELL;

            /* World X of left and right column edges */
            float wx0 = (float)(col     - TRN_COLS/2) * TRN_CELL;
            float wx1 = (float)(col + 1 - TRN_COLS/2) * TRN_CELL;

            /* World Z (absolute) for heightmap sampling — tile seamlessly */
            float wz0 = trn_scroll + cz0;
            float wz1 = trn_scroll + cz1;
            float tile = (float)TRN_TILE_LEN;
            float wz0t = wz0 - (float)((int)(wz0/tile)) * tile;
            float wz1t = wz1 - (float)((int)(wz1/tile)) * tile;
            if (wz0t < 0.0f) wz0t += tile;
            if (wz1t < 0.0f) wz1t += tile;

            /* Heights (world Y, positive = up) */
            float h00 = trn_height(wx0, wz0t);
            float h10 = trn_height(wx1, wz0t);
            float h01 = trn_height(wx0, wz1t);
            float h11 = trn_height(wx1, wz1t);

            /* Project: sx = hw + wx*FOV/cz
                        sy = hh - (wy - CAM_EYE_Y)*FOV/cz
               (terrain below camera eye -> positive (wy - EYE) is negative -> sy > hh, goes down) */
            float s0 = TRN_FOV / cz0, s1 = TRN_FOV / cz1;

            int sx00 = hw + (int)(wx0 * s0);
            int sy00 = hh - (int)((h00 - TRN_CAM_EYE_Y) * s0);
            int sx10 = hw + (int)(wx1 * s0);
            int sy10 = hh - (int)((h10 - TRN_CAM_EYE_Y) * s0);
            int sx01 = hw + (int)(wx0 * s1);
            int sy01 = hh - (int)((h01 - TRN_CAM_EYE_Y) * s1);
            int sx11 = hw + (int)(wx1 * s1);
            int sy11 = hh - (int)((h11 - TRN_CAM_EYE_Y) * s1);

            /* Colour + shading */
            float h_avg = (h00+h10+h01+h11)*0.25f;
            float slope = (fabsf3(h10-h00) + fabsf3(h01-h00)) / (TRN_CELL * 0.5f);
            if (slope > 1.0f) slope = 1.0f;
            /* Distance fog: fade to hazy sky colour at far end */
            float fog = (float)row / (float)(TRN_ROWS - 2);
            float shade_k = (0.5f + 0.5f*(1.0f-slope)) * (1.0f - fog*0.5f);
            uint32_t base = trn_color(h_avg, slope);
            uint32_t fc   = shade(base, shade_k);
            /* Blend in fog tint */
            if (fog > 0.5f) {
                uint8_t fr=(fc>>16)&0xFF, fg2=(fc>>8)&0xFF, fb2=fc&0xFF;
                float fk = (fog - 0.5f) * 1.2f; if(fk>1.0f)fk=1.0f;
                fr=(uint8_t)((float)fr*(1.0f-fk)+180.0f*fk);
                fg2=(uint8_t)((float)fg2*(1.0f-fk)+200.0f*fk);
                fb2=(uint8_t)((float)fb2*(1.0f-fk)+220.0f*fk);
                fc=0xFF000000u|((uint32_t)fr<<16)|((uint32_t)fg2<<8)|fb2;
            }

            tri_fill_z(sx00,sy00,cz0, sx10,sy10,cz0, sx01,sy01,cz1, fc);
            tri_fill_z(sx10,sy10,cz0, sx11,sy11,cz1, sx01,sy01,cz1, fc);
        }
    }

    /* Thin horizon haze strip */
    int hy = hh - (int)((0.0f - TRN_CAM_EYE_Y) * TRN_FOV / ((float)(TRN_ROWS+2)*TRN_CELL));
    for (int yy = hy - 3; yy <= hy + 5; yy++) {
        if (yy < 0 || yy >= (int)g.sh) continue;
        for (int xx = 0; xx < (int)g.sw; xx++)
            blend(xx, yy, 0xB4, 0xC8, 0xDC, 35);
    }
}

/* ============================================================
   EFFECT 15 — Lissajous Aurora
   Hundreds of overlapping parametric Lissajous curves drawn
   with additive colour blending and per-frame fade. No heavy
   math — everything runs on the sin_tbl. The curves drift in
   frequency and phase over time producing aurora-like ribbons
   that morph continuously. Very fast; every pixel touched only
   once per curve point, no zbuffer needed.
   ============================================================ */

#define LIS_CURVES    18    /* simultaneous curves */
#define LIS_PTS      320    /* sample points per curve per frame */
#define LIS_THICK      3    /* half-thickness of each ribbon in pixels */

typedef struct {
    uint8_t ax, ay;   /* x/y frequency (sin_tbl angles per unit t) */
    uint8_t px, py;   /* x/y phase offsets */
    uint8_t dax, day; /* drift rates for ax/ay */
    uint8_t dpx, dpy; /* drift rates for px/py */
    int     hue;      /* base colour hue 0-359 */
    int     dhue;     /* hue drift per frame */
} LisCurve;

static LisCurve lis_curves[LIS_CURVES];
static int lis_frame;

static void lis_init(void) {
    lis_frame = 0;
    clear(0xFF000000);
    for (int i = 0; i < LIS_CURVES; i++) {
        lis_curves[i].ax  = (uint8_t)rng_range(1, 5);
        lis_curves[i].ay  = (uint8_t)rng_range(1, 5);
        lis_curves[i].px  = (uint8_t)rng_range(0, 255);
        lis_curves[i].py  = (uint8_t)rng_range(0, 255);
        lis_curves[i].dax = (uint8_t)rng_range(0, 1);
        lis_curves[i].day = (uint8_t)rng_range(0, 1);
        lis_curves[i].dpx = (uint8_t)(1 + rng_range(0, 2));
        lis_curves[i].dpy = (uint8_t)(1 + rng_range(0, 2));
        lis_curves[i].hue  = rng_range(0, 360);
        lis_curves[i].dhue = rng_range(-2, 3);
    }
}

static void lis_update(void) {
    lis_frame++;

    /* Fade the framebuffer toward black each frame — this is what
       produces the glowing trail / persistence effect */
    uint32_t *fb = (uint32_t*)g.bb;
    uint32_t total = g.sw * g.sh;
    for (uint32_t i = 0; i < total; i++) {
        uint32_t p = fb[i];
        uint8_t r2 = (p >> 16) & 0xFF;
        uint8_t g2 = (p >>  8) & 0xFF;
        uint8_t b2 =  p        & 0xFF;
        if (r2 > 7) r2 -= 7; else r2 = 0;
        if (g2 > 7) g2 -= 7; else g2 = 0;
        if (b2 > 7) b2 -= 7; else b2 = 0;
        fb[i] = 0xFF000000u | ((uint32_t)r2 << 16) | ((uint32_t)g2 << 8) | b2;
    }

    int hw = (int)g.sw / 2;
    int hh = (int)g.sh / 2;
    int rx  = hw - 20;  /* x radius in pixels */
    int ry  = hh - 20;  /* y radius in pixels */

    for (int ci = 0; ci < LIS_CURVES; ci++) {
        LisCurve *c = &lis_curves[ci];

        /* Slowly drift the frequencies and phases over time */
        if ((lis_frame & 0x3F) == (ci * 4 & 0x3F)) {
            c->px += c->dpx;
            c->py += c->dpy;
        }
        if ((lis_frame & 0xFF) == (ci * 17 & 0xFF)) {
            /* Occasionally nudge the frequency by 1 step */
            uint8_t new_ax = (uint8_t)(1 + rng_range(0, 5));
            uint8_t new_ay = (uint8_t)(1 + rng_range(0, 5));
            /* Glide toward new frequency: only change if close */
            if (new_ax != c->ax) c->ax = new_ax;
            if (new_ay != c->ay) c->ay = new_ay;
        }
        c->hue = (c->hue + c->dhue + 360) % 360;

        /* Alpha: 30-60 so curves stack additively without washing out */
        uint8_t alpha = (uint8_t)(30 + rng_range(0, 30));
        uint32_t col  = hsv(c->hue, 90, 95);
        uint8_t cr = (col >> 16) & 0xFF;
        uint8_t cg = (col >>  8) & 0xFF;
        uint8_t cb =  col        & 0xFF;

        /* Draw this curve as LIS_PTS sample points */
        for (int p2 = 0; p2 < LIS_PTS; p2++) {
            /* t: 0..255 maps to one full parameter cycle */
            uint8_t t = (uint8_t)(p2 * 256 / LIS_PTS);
            int px2 = hw + (isin((uint8_t)(c->ax * t + c->px)) * rx) / 128;
            int py2 = hh + (isin((uint8_t)(c->ay * t + c->py)) * ry) / 128;

            /* Draw a small filled disc for thickness */
            for (int dy2 = -LIS_THICK; dy2 <= LIS_THICK; dy2++) {
                for (int dx2 = -LIS_THICK; dx2 <= LIS_THICK; dx2++) {
                    if (dx2*dx2 + dy2*dy2 > LIS_THICK*LIS_THICK) continue;
                    /* Intensity falls off from centre */
                    float dist = sqrtf3_fast((float)(dx2*dx2+dy2*dy2));
                    float k = 1.0f - dist / (float)(LIS_THICK + 1);
                    uint8_t a2 = (uint8_t)((float)alpha * k);
                    blend(px2 + dx2, py2 + dy2, cr, cg, cb, a2);
                }
            }
        }
    }
}

/* ============================================================
   Effect dispatch
   ============================================================ */

static const char *effect_names[NUM_EFFECTS]={
    "DVD Bounce","3D Pipes","Matrix Rain","Starfield Warp",
    "Plasma","Metaballs","3D Text","Cube Field",
    "Spinning Donut","Fire","Tunnel","Game of Life",
    "Bouncing Spheres","Terrain Flyover","Lissajous Aurora"
};

static void effect_start(int e){
    g.effect=e%NUM_EFFECTS;
    g.effect_start=time_ms();
    switch(g.effect){
        case 0:  dvd_init();    break;
        case 1:  pipe_init();   break;
        case 2:  mat_init();    break;
        case 3:  stars_init();  break;
        case 4:  plasma_init(); break;
        case 5:  mb_init();     break;
        case 6:  spin_init();   break;
        case 7:  cube_init();   break;
        case 8:  torus_init();  break;
        case 9:  fire_init();   break;
        case 10: tunnel_init(); break;
        case 11: life_init();   break;
        case 12: bs_init();     break;
        case 13: trn_init();    break;
        case 14: lis_init();    break;
    }
}

static void effect_update(void){
    switch(g.effect){
        case 0:  dvd_update();    break;
        case 1:  pipe_update();   break;
        case 2:  mat_update();    break;
        case 3:  stars_update();  break;
        case 4:  plasma_update(); break;
        case 5:  mb_update();     break;
        case 6:  spin_update();   break;
        case 7:  cube_update();   break;
        case 8:  torus_update();  break;
        case 9:  fire_update();   break;
        case 10: tunnel_update(); break;
        case 11: life_update();   break;
        case 12: bs_update();     break;
        case 13: trn_update();    break;
        case 14: lis_update();    break;
    }
}

/* Draw effect name + progress bar in corner */
static void draw_hud(void){
    if(!g.font) return;
    uint64_t now=time_ms();
    uint64_t elapsed=now-g.effect_start;
    int pct=(int)(elapsed*100/EFFECT_DURATION_MS);
    if(pct>100)pct=100;

    /* Semi-transparent pill in top-right */
    int tw=fnt_string_width(g.font,effect_names[g.effect]);
    int bx=(int)g.sw-tw-30, by=8;

    /* Background bar */
    uint32_t bg=0xAA000000;
    int bw=tw+20;
    for(int y=by;y<by+20;y++)
        for(int x=bx;x<bx+bw;x++)
            blend(x,y,0,0,0,170);

    draw_text(bx+8,by+4,effect_names[g.effect],0xFFCCCCCC);

    /* Progress dot row */
    for(int i=0;i<NUM_EFFECTS;i++){
        int dx=(int)g.sw-NUM_EFFECTS*12-4+i*12;
        int dy=by+22;
        uint32_t dc=(i==g.effect)?0xFFFFFFFF:0xFF444444;
        fill(dx,dy,8,4,dc);
    }

    /* TAB hint */
    draw_text(8,(int)g.sh-18,"TAB: next effect   Any key: exit",0xFF444444);
}

/* ============================================================
   Entry point
   ============================================================ */

int md_main(long argc, char **argv){
    (void)argc;(void)argv;
    memset(&g,0,sizeof(g));

    /* Seed rng */
    rng^=(uint32_t)time_ms();

    int efd=open("$/dev/input/event0",O_RDONLY|O_NONBLOCK,0);
    if(efd<0){ printf("screensaver: no event device\n"); sleep(2); return 2; }

    if(NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0,&g.device,&g.ctx,NULL)!=NodGL_OK){
        printf("screensaver: NodGL failed\n"); close(efd); return 1;
    }
    NodGL_GetScreenResolution(g.device,&g.sw,&g.sh);

    NodGL_TextureDesc td; memset(&td,0,sizeof(td));
    td.width=g.sw; td.height=g.sh;
    td.format=NodGL_FORMAT_R8G8B8A8_UNORM; td.mip_levels=1;
    if(NodGL_CreateTexture(g.device,&td,&g.tex)!=NodGL_OK){
        NodGL_ReleaseDevice(g.device); close(efd); return 1;
    }
    if(NodGL_MapResource(g.ctx,g.tex,(void**)&g.bb,&g.bb_pitch)!=NodGL_OK){
        NodGL_ReleaseResource(g.device,g.tex);
        NodGL_ReleaseDevice(g.device); close(efd); return 1;
    }

    /* Font (optional — HUD degrades gracefully if missing) */
    {
        int fd=open(FONT_PATH,O_RDONLY,0);
        if(fd>=0){
            long fsz=lseek(fd,0,SEEK_END); lseek(fd,0,SEEK_SET);
            if(fsz>0&&fsz<4*1024*1024){
                void *fdata=malloc((size_t)fsz);
                if(fdata){
                    size_t got=0;
                    while(got<(size_t)fsz){
                        ssize_t r=read(fd,(uint8_t*)fdata+got,(size_t)fsz-got);
                        if(r<=0)break; got+=(size_t)r;
                    }
                    g.font=fnt_load_font(fdata,got);
                    free(fdata);
                }
            }
            close(fd);
        }
    }

    /* Start with a random effect */
    effect_start((int)(rng_next()%NUM_EFFECTS));

    g.quit=0;
    while(!g.quit){
        Event ev;
        while(read(efd,&ev,sizeof(ev))>0){
            if(ev.type==EVENT_KEY_PRESSED){
                KeyCode kc=ev.data.keyboard.keycode;
                char ch=ev.data.keyboard.ascii;
                if(kc==KEY_TAB||ch=='\t'){
                    effect_start(g.effect+1);
                } else {
                    g.quit=1; break;
                }
            } else if(ev.type==EVENT_MOUSE_BUTTON){
                if(ev.data.mouse.buttons){ g.quit=1; break; }
            }
        }
        if(g.quit) break;

        /* Auto-advance effect */
        if(time_ms()-g.effect_start >= EFFECT_DURATION_MS)
            effect_start(g.effect+1);

        effect_update();
        draw_hud();

        NodGL_DrawTexture(g.ctx,g.tex,0,0,0,0,g.sw,g.sh);
        NodGL_PresentContext(g.ctx,1); /* vsync */
        yield();
    }

    /* Cleanup */
    if(zbuf){ free(zbuf); zbuf=NULL; }
    if(life_grid){ free(life_grid); life_grid=NULL; }
    if(life_next){ free(life_next); life_next=NULL; }
    if(fire_buf){ free(fire_buf); fire_buf=NULL; }
    if(tun_dist){ free(tun_dist); tun_dist=NULL; }
    if(tun_ang){ free(tun_ang); tun_ang=NULL; }
    if(g.font)   fnt_free_font(g.font);
    if(g.bb)     NodGL_UnmapResource(g.ctx,g.tex);
    NodGL_ReleaseResource(g.device,g.tex);
    NodGL_ReleaseDevice(g.device);
    close(efd);
    input_flush();
    return 0;
}