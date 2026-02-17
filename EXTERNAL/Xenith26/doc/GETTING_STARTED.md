# Getting Started with Xenith26

## What is Xenith26?

Xenith26 is an X11-inspired windowing system for ModuOS. It provides:

- **Display Server** (`xenith26d`) - Manages windows and graphics
- **Client Library** (`libX26`) - API for applications to create windows
- **Simple Protocol** - Clean message-based communication
- **UserFS IPC** - Uses ModuOS's UserFS for inter-process communication

## Quick Start

### 1. Build Xenith26

```bash
cd EXTERNAL/Xenith26
make
```

This builds:
- `build/xenith26d.sqr` - Display server
- `build/libX26.a` - Client library
- `build/hello_x26.sqr` - Example application
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
/Apps/xenith26d
```

You should see:
```
=== Xenith26 Display Server ===
[xenith26d] Opening graphics device...
[xenith26d] Display initialized: 1024x768 @ 32 bpp
[xenith26d] Server ready and waiting for clients...
```

### 6. Run Example Applications

Open another terminal/shell and run:
```
/Apps/hello_x26
```

or

```
/Apps/xclock
```

## Architecture Overview

```
┌──────────────────┐
│   Application    │  (hello_x26, xclock, etc.)
│  (uses libX26)   │
└────────┬─────────┘
         │
         │ libX26 API (X26CreateWindow, X26DrawRect, etc.)
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
│   xenith26d      │  Display Server
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
- [libX26 API](LIBX26_API.md) - Full API reference

## Troubleshooting

### "Could not connect to display server"

Make sure `xenith26d` is running first.

### "Could not open $/dev/graphics/video0"

Make sure ModuOS is booted in graphics mode (not text mode).

### Window doesn't appear

Check that you called:
1. `X26MapWindow()` to show the window
2. `X26Flush()` to commit drawing commands

## Examples

See the `examples/` directory for:
- `hello_x26.c` - Basic window with shapes and text
- `xclock.c` - Animated analog clock
