# Getting Started with FlareX

## What is FlareX?

FlareX is an X11-inspired windowing system for ModuOS. It provides:

- **Display Server** (`FlareXd`) - Manages windows and graphics
- **Client Library** (`libFlareX`) - API for applications to create windows
- **Simple Protocol** - Clean message-based communication
- **UserFS IPC** - Uses ModuOS's UserFS for inter-process communication

## Quick Start

### 1. Build FlareX

```bash
cd EXTERNAL/FlareX
make
```

This builds:
- `build/FlareXd.sqr` - Display server
- `build/libFlareX.a` - Client library
- `build/hello_FlareX.sqr` - Example application
- `build/xclock.sqr` - Clock demo

### 2. Install to ISO

```bash
make install
```

This copies the executables to `targets/AMD64/iso/Apps/`

### 3. Rebuild ModuOS

From the root directory:
```bash
make
```

### 4. Run ModuOS

Boot ModuOS using your preferred method:
```bash
./run.bat
```

### 5. Start the Display Server

Once logged in to ModuOS:
```
/Apps/FlareXd
```

You should see:
```
=== FlareX Display Server ===
[FlareXd] Opening graphics device...
[FlareXd] Display initialized: 1024x768 @ 32 bpp
[FlareXd] Server ready and waiting for clients...
```

### 6. Run Example Applications

Open another terminal/shell and run:
```
/Apps/hello_FlareX
```

or

```
/Apps/xclock
```

## Architecture Overview

```
┌──────────────────┐
│   Application    │  (hello_FlareX, xclock, etc.)
│  (uses libFlareX)   │
└────────┬─────────┘
         │
         │ libFlareX API (FlareXCreateWindow, FlareXDrawRect, etc.)
         │
         ▼
┌──────────────────┐
│  UserFS Nodes    │  $/user/xapi/gfxapi
│                  │  $/user/xapi/event
│                  │  $/user/xapi/windows
└────────┬─────────┘
         │
         │ Binary Protocol Messages
         │
         ▼
┌──────────────────┐
│   FlareXd      │  Display Server
│  (Server)        │  - Window management
└────────┬─────────┘  - Compositing
         │            - Input handling
         │
         ▼
┌──────────────────┐
│ $/dev/graphics/  │  Framebuffer (MD64API_GRP)
│     video0       │
└──────────────────┘
```

## Next Steps

- [Writing Applications](APPLICATIONS.md) - Create your own GUI apps
- [Protocol Reference](PROTOCOL.md) - Protocol details
- [Window Manager Guide](WINDOW_MANAGERS.md) - Build a WM
- [libFlareX API](LIBFlareX_API.md) - Full API reference

## Troubleshooting

### "Could not connect to display server"

Make sure `FlareXd` is running first.

### "Could not open $/dev/graphics/video0"

Make sure ModuOS is booted in graphics mode (not text mode).

### Window doesn't appear

Check that you called:
1. `FlareXMapWindow()` to show the window
2. `FlareXFlush()` to commit drawing commands

## Examples

See the `examples/` directory for:
- `hello_FlareX.c` - Basic window with shapes and text
- `xclock.c` - Animated analog clock

