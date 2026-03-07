# Blit Engine

A complete 2D game framework for ModuOS, built on NodGL.

## 📁 Directory Structure

```
EXTERNAL/Blit/
├── src/
│   ├── engine/          - Core engine source
│   │   └── Blit.c
│   └── studio/          - BlitStudio editor
│       ├── BlitStudio.c
│       └── main.c
├── include/
│   └── Blit/            - Public headers
│       ├── Blit.h
│       └── BlitStudio.h
├── examples/            - Example games
│   ├── shooter.c
│   ├── pong.c
│   ├── platformer.c
│   └── breakout.c
├── lib/                 - Built libraries (generated)
│   └── libBlit.a
├── build/               - Build outputs (generated)
│   ├── blitstudio.sqr
│   ├── shooter.sqr
│   └── ...
├── Makefile             - Build system
├── build.sh             - Build script
├── README.md            - This file
├── INSTALL.md           - Installation guide
├── INTEGRATION.md       - ModuOS integration
├── STRUCTURE.md         - Directory layout explanation
└── LICENSE              - GPL v2.0
```

**Documentation is in:** `wiki-repo/Blit/`

## 🚀 Quick Start

### Build

```bash
cd EXTERNAL/Blit
make all
```

Builds:
- `lib/libBlit.a` - Engine library
- `build/blitstudio.sqr` - Visual editor  
- `build/*.sqr` - Example games

### Install

```bash
make install
```

Copies to ModuOS system directories.

### Run

```bash
cd build
./shooter.sqr        # Space shooter
./pong.sqr           # Pong game
./blitstudio.sqr     # Visual editor
```

## 🎮 Features

- **Entity/Sprite System**
- **Collision Detection**
- **Input Handling**
- **BlitStudio Editor**
- **GPU Accelerated**

## 📖 Documentation

**All documentation is in `wiki-repo/Blit/`:**

- `wiki-repo/Blit/README.md` - Documentation index
- `wiki-repo/Blit/Overview.md` - What is Blit?
- `wiki-repo/Blit/Getting-Started.md` - First steps
- `wiki-repo/Blit/Core-Concepts.md` - How it works
- `wiki-repo/Blit/API-Reference.md` - Complete API
- `wiki-repo/Blit/BlitStudio.md` - Editor manual

## 🔧 API Example

```c
#include <Blit/Blit.h>

BlitEngine engine;
blit_init(&engine);

Sprite *sprite = blit_sprite_create_circle(&engine, 16, 0xFF00FF00);

while (blit_is_running(&engine)) {
    blit_update_input(&engine);
    
    blit_begin_frame(&engine, 0xFF000000);
    blit_sprite_draw(&engine, sprite, x, y);
    blit_end_frame(&engine);
}

blit_shutdown(&engine);
```

## 🏗️ Building Your Game

```bash
gcc -I EXTERNAL/Blit/include -I include -c mygame.c -o mygame.o
ld -T userland/user.ld mygame.o \
   EXTERNAL/Blit/lib/libBlit.a \
   userland/lib_NodGL.a userland/libc.a \
   -o mygame.sqr
```

## 📜 License

GPL v2.0 - Part of the ModuOS project.

---

**Documentation:** `wiki-repo/Blit/`  
**Ready to make games? Run `make all`!** 🚀
