#!/bin/sh
# build.sh - organized build for ModuOS userland (POSIX sh)

# Avoid strict-mode flags here because some minimal /bin/sh builds behave differently.
set -e

# Compiler/Linker
CC=${CC:-x86_64-elf-gcc}
LD=${LD:-x86_64-elf-ld}

# Common flags
GCC_FLAGS_COMMON="-ffreestanding -c -mno-red-zone -O2 -nostdlib"

# Linker scripts
LD_SCRIPT_APP="user.ld"        # fixed 0x400000 (ET_EXEC)
LD_SCRIPT_LD="ld_user.ld"      # interpreter (ET_EXEC)
LD_SCRIPT_LIB="lib_user.ld"    # shared libs (ET_DYN)

BUILD_DIR="build"
DIST_DIR="dist"

rm -rf "$BUILD_DIR" "$DIST_DIR"
mkdir -p "$BUILD_DIR" "$DIST_DIR"

# Compile
for src in *.c; do
    [ -f "$src" ] || continue
    obj="$BUILD_DIR/${src%.c}.o"
    echo "[BUILD] CC $src -> $obj"

    case "$src" in
        lib_*.c)
            "$CC" $GCC_FLAGS_COMMON -fPIC "$src" -o "$obj"
            ;;
        *)
            "$CC" $GCC_FLAGS_COMMON "$src" -o "$obj"
            ;;
    esac

done

# Link in two phases so libraries exist before apps that depend on them.

# Phase 1: shared libs
for obj in "$BUILD_DIR"/lib_*.o; do
    [ -f "$obj" ] || continue
    base=$(basename "${obj%.o}")
    outname="${base#lib_}.sqrl"
    bin="$DIST_DIR/$outname"
    echo "[BUILD] LD(shared) $obj -> $bin"
    "$LD" "$obj" -shared -T "$LD_SCRIPT_LIB" -o "$bin" --hash-style=sysv

done

# Phase 2: interpreter + apps
for obj in "$BUILD_DIR"/*.o; do
    [ -f "$obj" ] || continue
    base=$(basename "${obj%.o}")

    case "$base" in
        lib_*)
            # already linked in phase 1
            ;;
        ld_moduos)
            bin="$DIST_DIR/ld-moduos.sqr"
            echo "[BUILD] LD(interp) $obj -> $bin"
            "$LD" "$obj" -T "$LD_SCRIPT_LD" -o "$bin" --hash-style=sysv
            ;;
        demo_dyn)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app dyn-demo) $obj -> $bin"
            "$LD" "$obj" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv \
                --dynamic-linker /ModuOS/shared/usr/lib/ld-moduos.sqr \
                --no-as-needed \
                -L"$DIST_DIR" -l:demo_gfx.sqrl
            ;;
        *)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app) $obj -> $bin"
            "$LD" "$obj" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv \
                --dynamic-linker /ModuOS/shared/usr/lib/ld-moduos.sqr
            ;;
    esac

done

echo "[BUILD] Done. Outputs in $DIST_DIR"
