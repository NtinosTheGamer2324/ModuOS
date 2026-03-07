#!/bin/bash
# Blit Engine - Build Script
# Copyright © 2026 ModuOS Project Contributors

set -e

echo "================================"
echo "  Blit Engine Build Script"
echo "================================"
echo ""

# Detect platform
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "win32" ]]; then
    MAKE=mingw32-make
else
    MAKE=make
fi

# Parse arguments
TARGET="${1:-all}"

case "$TARGET" in
    all)
        echo "Building Blit Engine, BlitStudio, and examples..."
        $MAKE all
        ;;
    blit)
        echo "Building Blit Engine library..."
        $MAKE blit
        ;;
    blitstudio|studio|editor)
        echo "Building BlitStudio editor..."
        $MAKE blitstudio
        ;;
    examples)
        echo "Building example games..."
        $MAKE examples
        ;;
    install)
        echo "Installing to ModuOS..."
        $MAKE install
        ;;
    clean)
        echo "Cleaning build artifacts..."
        $MAKE clean
        ;;
    help|--help|-h)
        $MAKE help
        ;;
    *)
        echo "Unknown target: $TARGET"
        echo "Run './build.sh help' for available targets"
        exit 1
        ;;
esac

echo ""
echo "Build complete!"
echo ""
echo "Outputs in build/ directory:"
echo "  - libBlit.a         (Blit Engine library)"
echo "  - blitstudio.sqr    (BlitStudio editor)"
echo "  - shooter.sqr       (Example game)"
echo "  - pong.sqr          (Example game)"
echo "  - platformer.sqr    (Example game)"
echo "  - breakout.sqr      (Example game)"
echo ""
echo "To install: ./build.sh install"
echo "To clean:   ./build.sh clean"
