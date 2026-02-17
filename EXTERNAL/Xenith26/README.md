# Xenith26 - X11-like Windowing System for ModuOS

Xenith26 is a modern X11-inspired windowing system designed specifically for ModuOS.

## Features

✨ **X11-Inspired Architecture** - Client-server model with clean separation  
🪟 **Multiple Windows** - Up to 64 simultaneous windows  
🎨 **2D Graphics** - Rectangles, lines, text, and more  
⚡ **Fast Compositing** - Direct framebuffer access via MD64API_GRP  
🔌 **UserFS IPC** - Native ModuOS inter-process communication  
📦 **Simple API** - Easy-to-use libX26 client library  

## Architecture

```
┌──────────────┐         ┌──────────────┐         ┌──────────────┐
│  X26 Clients │◄───────►│  xenith26d   │◄───────►│ Window       │
│  (Apps)      │  IPC    │  (Server)    │ Protocol│ Manager      │
└──────────────┘         └──────────────┘         └──────────────┘
                               │
                         ┌─────▼─────┐
                         │ MD64API   │
                         │ Framebuffer│
                         └───────────┘
```

### Components

- **xenith26d** - Display server (like Xorg)
  - Manages windows and compositing
  - Direct framebuffer access via `$/dev/graphics/video0`
  - Handles input events
  
- **libX26** - Client library (like Xlib)
  - Simple C API for creating windows
  - Drawing primitives (rects, lines, text)
  - Event handling
  
- **UserFS Nodes** - Communication channels
  - `$/user/xapi/gfxapi` - Graphics commands
  - `$/user/xapi/event` - Input events
  - `$/user/xapi/windows` - Window management

## Quick Start

### 1. Build Xenith26

```bash
cd EXTERNAL/Xenith26
make
make install
```

### 2. Rebuild ModuOS

```bash
cd ../..
make
```

### 3. Boot ModuOS and Start Server

```bash
./run.bat
```

In ModuOS:
```
/Apps/xenith26d &
```

### 4. Run Example Apps

```
/Apps/hello_x26
/Apps/xclock
```

## Example Application

```c
#include "lib/libX26.h"

int md_main(long argc, char **argv) {
    X26Display *dpy = X26OpenDisplay();
    X26Window win = X26CreateSimpleWindow(dpy, 100, 100, 400, 300);
    
    X26ClearWindow(dpy, win, X26_COLOR_WHITE);
    X26DrawText(dpy, win, 50, 50, "Hello Xenith26!", X26_COLOR_BLACK);
    
    X26MapWindow(dpy, win);
    X26Flush(dpy, win);
    
    while (1) {
        /* Event loop */
        sleep(1);
    }
    
    return 0;
}
```

## Directory Structure

```
EXTERNAL/Xenith26/
├── server/         Display server (xenith26d)
│   ├── startx.c           Main server implementation
│   ├── xapi_proto.h       Protocol definitions
│   └── nodes.h            UserFS node paths
├── lib/            Client library
│   ├── libX26.h           Public API header
│   └── libX26.c           Implementation
├── examples/       Example applications
│   ├── hello_x26.c        Basic window demo
│   └── xclock.c           Analog clock
├── doc/            Documentation
│   ├── GETTING_STARTED.md
│   ├── APPLICATIONS.md
│   └── PROTOCOL.md
├── Makefile        Build system
└── README.md       This file
```

## Documentation

- **[Getting Started](doc/GETTING_STARTED.md)** - Setup and first steps
- **[Writing Applications](doc/APPLICATIONS.md)** - App development guide
- **[Protocol Reference](doc/PROTOCOL.md)** - Wire protocol details
- **[libX26 API](lib/libX26.h)** - Full API documentation

## Protocol Messages

### Graphics Commands
- `XAPI_CMD_RECT` - Draw rectangle
- `XAPI_CMD_TEXT` - Draw text
- `XAPI_CMD_LINE` - Draw line
- `XAPI_CMD_BLIT` - Blit buffer
- `XAPI_CMD_COMMIT` - Present window

### Window Management
- `XAPI_WIN_CREATE` - Create window
- `XAPI_WIN_MAP` - Show window
- `XAPI_WIN_DESTROY` - Destroy window
- `XAPI_WIN_RAISE` - Bring to front
- `XAPI_WIN_MOVE` - Reposition window

### Events (Server → Client)
- `XAPI_EVENT_EXPOSE` - Redraw needed
- `XAPI_EVENT_KEY_PRESS` - Keyboard input
- `XAPI_EVENT_MOUSE_MOVE` - Mouse movement
- `XAPI_EVENT_MOUSE_PRESS` - Mouse click

## Comparison with X11

| Feature | X11 | Xenith26 |
|---------|-----|----------|
| Protocol | X11 Protocol (complex) | Simple binary protocol |
| Transport | TCP/IP, Unix sockets | UserFS nodes |
| Authentication | Xauth, cookies | Process ownership |
| Extensions | Many (RENDER, XInput, etc.) | Built into core |
| Network | Yes | Local only |
| Complexity | Very high | Low |

## Building from Source

Requires ModuOS cross-compiler toolchain.

```bash
make              # Build everything
make server       # Build server only
make lib          # Build library only
make examples     # Build examples only
make install      # Install to ISO
make clean        # Clean build files
```

## Current Status

✅ Display server with framebuffer access  
✅ Window creation and management  
✅ Basic drawing primitives (rect, line, text)  
✅ Client library (libX26)  
✅ Example applications  
⏳ Event handling (TODO)  
⏳ Window manager support (TODO)  
⏳ Shared memory buffers (TODO)  

## Future Plans

- [ ] Complete event handling system
- [ ] Window manager protocol
- [ ] Alpha blending and transparency
- [ ] Image/bitmap loading (BMP format)
- [ ] Font rendering (FNT/PF2 support)
- [ ] Copy/paste clipboard
- [ ] Drag and drop
- [ ] Network transparency (optional)

## License

Same as ModuOS core (see main LICENSE.md)

## Contributing

Contributions welcome! See the main ModuOS contributing guide.

## Credits

Designed and implemented for ModuOS by the development team.  
Inspired by X11, Wayland, and other windowing systems.
