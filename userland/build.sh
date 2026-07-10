#!/bin/sh
# build.sh - organized build for ModuOS userland (POSIX sh)

# Avoid strict-mode flags here because some minimal /bin/sh builds behave differently.
set -e

# Default number of jobs/threads
JOBS=1

# Parse arguments for -j flag
while [ $# -gt 0 ]; do
    case "$1" in
        -j)
            if [ -n "$2" ] && [ "${2#-}" = "$2" ]; then
                JOBS="$2"
                shift 2
            else
                echo "Error: -j requires an argument" >&2
                exit 1
            fi
            ;;
        -j*)
            JOBS="${1#-j}"
            shift 1
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

# Compiler/Linker
CC=${CC:-x86_64-elf-gcc}
LD=${LD:-x86_64-elf-ld}

# Common flags
GCC_FLAGS_COMMON="-ffreestanding -c -mno-red-zone -O2 -fomit-frame-pointer -nostdlib -I."

# Linker scripts
LD_SCRIPT_APP="user.ld"      # fixed 0x400000 (ET_EXEC)
LD_SCRIPT_LD="ld_user.ld"    # interpreter (ET_EXEC)
LD_SCRIPT_LIB="lib_user.ld"  # shared libs (ET_DYN)

BUILD_DIR="build"
DIST_DIR="dist"

rm -rf "$BUILD_DIR" "$DIST_DIR"
mkdir -p "$BUILD_DIR" "$DIST_DIR"

# --- Phase 0: Compile userland objects (Parallelized) ---
echo "[BUILD] Compiling source files using $JOBS thread(s)..."

current_jobs=0

for src in *.c; do
    [ -f "$src" ] || continue

    # Stop shipping legacy apps
    case "$src" in
        bmpview.c)
            echo "[BUILD] SKIP legacy $src"
            continue
            ;;
    esac

    obj="$BUILD_DIR/${src%.c}.o"
    echo "[BUILD] CC $src -> $obj"

    case "$src" in
        lib_demo_gfx.c)
            # Only lib_demo_gfx needs -fPIC because it's truly a shared library
            "$CC" $GCC_FLAGS_COMMON -fPIC -DLIBC_NO_START "$src" -o "$obj" &
            ;;
        lib_*.c)
            # Other lib_* are used for static linking - NO -fPIC
            "$CC" $GCC_FLAGS_COMMON -DLIBC_NO_START "$src" -o "$obj" &
            ;;
        emu6502.c|tia_pf.c)
            # Emulator/core objects are compiled as support objects (no entry point)
            "$CC" $GCC_FLAGS_COMMON -DLIBC_NO_START "$src" -o "$obj" &
            ;;
        *)
            "$CC" $GCC_FLAGS_COMMON "$src" -o "$obj" &
            ;;
    esac

    # Manage thread pool
    current_jobs=$((current_jobs + 1))
    if [ "$current_jobs" -ge "$JOBS" ]; then
        wait
        current_jobs=0
    fi
done

# Wait for any remaining compilation background jobs to finish
wait

# --- Phase 0b: Compile coreutils objects (Parallelized) ---
echo "[BUILD] Compiling coreutils source files using $JOBS thread(s)..."

current_jobs=0

for src in coreutils/*.c; do
    [ -f "$src" ] || continue

    base=$(basename "$src")
    obj="$BUILD_DIR/coreutils_${base%.c}.o"
    echo "[BUILD] CC $src -> $obj"

    "$CC" $GCC_FLAGS_COMMON -Icoreutils "$src" -o "$obj" &

    current_jobs=$((current_jobs + 1))
    if [ "$current_jobs" -ge "$JOBS" ]; then
        wait
        current_jobs=0
    fi
done

wait

# --- Phase 1: shared libs ---
# (Kept sequential because it's fast and avoids dependency race conditions)
for obj in "$BUILD_DIR"/lib_*.o; do
    [ -f "$obj" ] || continue
    base=$(basename "${obj%.o}")
    
    # Skip libraries that are only used for static linking
    case "$base" in
        lib_NodGL|lib_NodGL_shader|lib_sw_shader|lib_gfx2d|lib_fnt|lib_8bit|lib_a2600|lib_json|lib_pakzip)
            # These are static-only libraries, skip creating .sqrl
            continue
            ;;
    esac
    
    outname="${base#lib_}.sqrl"
    bin="$DIST_DIR/$outname"
    echo "[BUILD] LD(shared) $obj -> $bin"
    "$LD" "$obj" -shared -T "$LD_SCRIPT_LIB" -o "$bin" --hash-style=sysv

done

# --- Phase 2: interpreter + apps ---
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
                --dynamic-linker /ModuOS/shared/lib/ld-moduos.sqr \
                --no-as-needed \
                -L"$DIST_DIR" -l:demo_gfx.sqrl
            ;;
        paintgfx)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static gfx2d) $obj + lib_nodgl.o + lib_gfx2d.o -> $bin"
            "$LD" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        ilib_viewer)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static gfx2d) $obj + lib_nodgl.o + lib_gfx2d.o + lib_fnt.o -> $bin"
            "$LD" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        imgview)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static gfx2d) $obj + lib_nodgl.o + lib_gfx2d.o + lib_fnt.o -> $bin"
            "$LD" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        tvd_player)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static gfx2d) $obj + lib_nodgl.o + lib_gfx2d.o + lib_fnt.o -> $bin"
            "$LD" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        pakman)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static pakman) $obj + libs -> $bin"
            "$LD" "$obj" \
                "$BUILD_DIR/lib_json.o" \
                "$BUILD_DIR/lib_pakzip.o" \
                -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        pacmangfx)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static $base) $obj + lib_fnt.o -> $bin"
            "$LD" "$obj" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        teseraris)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static NodGL+fnt) $obj + lib_NodGL.o + lib_gfx2d.o + lib_fnt.o -> $bin"
            "$LD" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        minesgfx)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static NodGL+fnt) $obj + lib_NodGL.o + lib_gfx2d.o + lib_fnt.o -> $bin"
            "$LD" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        nodds)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static NodGL) $obj + lib_NodGL.o + lib_gfx2d.o + lib_fnt.o -> $bin"
            "$LD" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        nodds_demo)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static NodGL) $obj + nodds_client.o + lib_fnt.o -> $bin"
            "$LD" "$obj" "$BUILD_DIR/nodds_client.o" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
            
        calcgfx|snakegfx|raygfx|froggergfx|gfxclock|mousedemo|sysmon|miniwm|neontank|gfxtest)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static NodGL) $obj + lib_NodGL.o + lib_gfx2d.o -> $bin"
            "$LD" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;

        screensaver)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static NodGL) $obj + lib_NodGL.o + lib_gfx2d.o + lib_fnt.o -> $bin"
            "$LD" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        maze2d)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static NodGL) $obj + lib_NodGL.o + lib_gfx2d.o + lib_fnt.o -> $bin"
            "$LD" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        NodGL_demo|NodGL_benchmark|NodGL_triangle|NodGL_stress_test|NodGLDiag)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static NodGL) $obj + lib_NodGL.o + lib_gfx2d.o -> $bin"
            "$LD" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        shader_demo)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static NodGL+shader) $obj + lib_NodGL.o + lib_NodGL_shader.o + lib_sw_shader.o + lib_gfx2d.o -> $bin"
            "$LD" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_NodGL_shader.o" "$BUILD_DIR/lib_sw_shader.o" "$BUILD_DIR/lib_gfx2d.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        ntosiux_ttyman)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static ttyman) $obj + lib_NodGL.o + lib_fnt.o + lib_gfx2d.o -> $bin"
            "$LD" "$obj" \
                "$BUILD_DIR/lib_NodGL.o" \
                "$BUILD_DIR/lib_fnt.o" \
                "$BUILD_DIR/lib_gfx2d.o" \
                -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        *)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static) $obj -> $bin"
            "$LD" "$obj" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
    esac

done

# --- Phase 2b: Link coreutils apps ---
echo "[BUILD] Linking coreutils apps..."

for obj in "$BUILD_DIR"/coreutils_*.o; do
    [ -f "$obj" ] || continue
    # Strip the "coreutils_" prefix to recover the original tool name
    base=$(basename "${obj%.o}")
    toolname="${base#coreutils_}"
    bin="$DIST_DIR/${toolname}.sqr"
    echo "[BUILD] LD(coreutil) $obj -> $bin"
    "$LD" "$obj" -T "$LD_SCRIPT_APP" -o "$bin" --hash-style=sysv
done

echo "[BUILD] Done. Outputs in $DIST_DIR"

# Build Blit Engine (after lib_NodGL.o is available)
echo "[BUILD] Building Blit Engine..."
if [ -d "../EXTERNAL/Blit" ]; then
    (cd "../EXTERNAL/Blit" && make -j"$JOBS" all && make install)
    echo "[BUILD] Blit Engine built and installed"
    if [ -d "../EXTERNAL/Blit/build" ]; then
        echo "[BUILD] Copying Blit executables to $DIST_DIR..."
        cp ../EXTERNAL/Blit/build/*.sqr "$DIST_DIR/" 2>/dev/null || true
    fi
else
    echo "[BUILD] WARNING: Blit Engine not found at ../EXTERNAL/Blit"
fi