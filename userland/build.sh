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
# -Ilibc: the ISO headers (stdio.h, string.h, ...) live in libc/.
GCC_FLAGS_COMMON="-ffreestanding -c -mno-red-zone -O2 -fomit-frame-pointer -nostdlib -I. -Ilibc"

# Linker scripts
LD_SCRIPT_APP="user.ld"      # fixed 0x400000 (ET_EXEC)
LD_SCRIPT_DYN="user_dyn.ld"  # fixed 0x400000 (ET_EXEC) + PT_INTERP/PT_DYNAMIC
LD_SCRIPT_LD="ld_user.ld"    # interpreter (ET_EXEC at DL_INTERP_BASE)
LD_SCRIPT_LIB="lib_user.ld"  # shared libs (ET_DYN)

# Libraries shipped as .sqrl (lib_<name>.c, compiled -fPIC). Anything not
# listed here is still linked statically into the apps that use it.
# Order matters: a library must come after the libraries it depends on.
#
# libc (libc/libc.c -> dist/libc.sqrl) is built from libc/, not from lib_libc.c,
# and every other library and every app is linked against it, so it comes first.
SHARED_LIBS="libc demo_gfx fnt crypto tls"

# Libraries that need other .sqrl libraries, as lib:dep1,dep2. The deps become
# DT_NEEDED entries so ld-moduos loads them when the library is loaded.
LIB_DEPS="tls:crypto"

# Apps linked dynamically, as app:lib1,lib2 (names without lib_/.sqrl).
DYNAMIC_APPS="demo_dyn:demo_gfx
    mcurl:tls,crypto
    ilib_viewer:fnt
    imgview:fnt
    tvd_player:fnt
    teseraris:fnt
    screensaver:fnt
    gfxclock:fnt
    mdw:fnt
    demo_welcome:fnt
    demo_notes:fnt
    mdw_test:fnt
    mdterm:fnt
    mdparted_gui:fnt
    mdrun:fnt
    mdinfo:fnt
    mdfm:fnt
    mdedit:fnt
    mdtaskbar:fnt
    mddesktop:fnt
    ntosiux_ttyman:fnt"

list_has() {
    for item in $2; do
        [ "$item" = "$1" ] && return 0
    done
    return 1
}

# Every app links libc.sqrl; DYNAMIC_APPS only lists the *extra* libraries.
dyn_app_libs() {
    extra=""
    for entry in $DYNAMIC_APPS; do
        if [ "${entry%%:*}" = "$1" ]; then
            extra=$(echo "${entry#*:}" | tr ',' ' ')
            break
        fi
    done
    echo "libc $extra"
}

# Links an app. Every app is dynamic: it drops the static objects of its shared
# libraries (always including libc), uses the dynamic-app script, gets crt0
# (the _start that calls md_main) and DT_NEEDED entries plus PT_INTERP.
ld_app() {
    app="$1"
    shift
    app_libs=$(dyn_app_libs "$app")

    count=$#
    while [ "$count" -gt 0 ]; do
        arg="$1"
        shift
        count=$((count - 1))
        keep=1
        for dl in $app_libs; do
            [ "$arg" = "$BUILD_DIR/lib_${dl}.o" ] && keep=0
        done
        [ "$arg" = "$LD_SCRIPT_APP" ] && arg="$LD_SCRIPT_DYN"
        [ "$keep" -eq 1 ] && set -- "$@" "$arg"
    done
    set -- "$@" "$BUILD_DIR/lib_crt0.o"

    link_libs=""
    for dl in $app_libs; do
        link_libs="$link_libs -l:${dl}.sqrl"
    done
    echo "[BUILD] dynamic: $app -> $app_libs"
    "$LD" "$@" \
        --dynamic-linker /ModuOS/shared/lib/ld-moduos.sqrl \
        --no-as-needed \
        -L"$DIST_DIR" $link_libs
}

BUILD_DIR="build"
DIST_DIR="dist"

rm -rf "$BUILD_DIR" "$DIST_DIR"
mkdir -p "$BUILD_DIR" "$DIST_DIR"

# --- Phase -1: stale pre-libc/ headers ---
# A quoted #include finds a header next to the source file before it looks at
# -Ilibc, so the old copies would silently shadow the new ones. Delete them.
stale=0
for h in assert.h errno.h libc.h stdlib.h string.h time.h coreutils/libc.h coreutils/string.h; do
    if [ -f "$h" ]; then
        echo "[BUILD] ERROR: stale $h shadows libc/$(basename "$h"); delete it" >&2
        stale=1
    fi
done
[ "$stale" -eq 0 ] || exit 1

# --- Phase 0a: libc ---
# lib_libc.o       -> libc.sqrl (PIC; functions only, no exported data)
# lib_crt0.o       -> _start, linked into every program
# (lib_ prefix keeps Phase 2 from treating them as apps)
echo "[BUILD] CC libc/libc.c -> $BUILD_DIR/lib_libc.o (shared)"
"$CC" $GCC_FLAGS_COMMON -fPIC -fno-semantic-interposition -fno-stack-protector \
    libc/libc.c -o "$BUILD_DIR/lib_libc.o" &
echo "[BUILD] CC libc/crt0.c -> $BUILD_DIR/lib_crt0.o"
"$CC" $GCC_FLAGS_COMMON libc/crt0.c -o "$BUILD_DIR/lib_crt0.o" &
wait

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

    lib_name="${src#lib_}"
    lib_name="${lib_name%.c}"
    case "$src" in
        ld_moduos.c)
            # Runs before anything else in the process: no stack protector, no PIC.
            "$CC" $GCC_FLAGS_COMMON -fno-pic -fno-stack-protector "$src" -o "$obj" &
            ;;
        lib_*.c)
            if list_has "$lib_name" "$SHARED_LIBS"; then
                # Shared libraries must be position independent
                "$CC" $GCC_FLAGS_COMMON -fPIC -DLIBC_NO_START "$src" -o "$obj" &
            else
                # Others are used for static linking - NO -fPIC
                "$CC" $GCC_FLAGS_COMMON -DLIBC_NO_START "$src" -o "$obj" &
            fi
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
# Kept sequential and in SHARED_LIBS order so a library's dependencies are
# already in $DIST_DIR when it is linked.
for lib in $SHARED_LIBS; do
    obj="$BUILD_DIR/lib_${lib}.o"
    [ -f "$obj" ] || continue
    bin="$DIST_DIR/${lib}.sqrl"

    dep_flags=""
    extra_ld=""
    if [ "$lib" = "libc" ]; then
        # libc binds its own symbols locally and depends on nothing.
        extra_ld="-Bsymbolic"
    else
        dep_flags="-l:libc.sqrl"
    fi
    for entry in $LIB_DEPS; do
        if [ "${entry%%:*}" = "$lib" ]; then
            for dep in $(echo "${entry#*:}" | tr ',' ' '); do
                dep_flags="$dep_flags -l:${dep}.sqrl"
            done
        fi
    done

    echo "[BUILD] LD(shared) $obj -> $bin"
    "$LD" "$obj" -shared -T "$LD_SCRIPT_LIB" -o "$bin" --hash-style=sysv \
        --no-as-needed -L"$DIST_DIR" $extra_ld $dep_flags
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
            bin="$DIST_DIR/ld-moduos.sqrl"
            echo "[BUILD] LD(interp) $obj -> $bin"
            "$LD" "$obj" -T "$LD_SCRIPT_LD" -o "$bin" --hash-style=sysv
            ;;
        paintgfx)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static gfx2d) $obj + lib_nodgl.o + lib_gfx2d.o -> $bin"
            ld_app "$base" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        ilib_viewer)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static gfx2d) $obj + lib_nodgl.o + lib_gfx2d.o + lib_fnt.o -> $bin"
            ld_app "$base" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        imgview)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static gfx2d) $obj + lib_nodgl.o + lib_gfx2d.o + lib_fnt.o -> $bin"
            ld_app "$base" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        tvd_player)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static gfx2d) $obj + lib_nodgl.o + lib_gfx2d.o + lib_fnt.o -> $bin"
            ld_app "$base" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        pakman)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static pakman) $obj + libs -> $bin"
            ld_app "$base" "$obj" \
                "$BUILD_DIR/lib_json.o" \
                "$BUILD_DIR/lib_pakzip.o" \
                -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        mcurl)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app tls) $obj + lib_tls.o + lib_crypto.o -> $bin"
            ld_app "$base" "$obj" \
                "$BUILD_DIR/lib_tls.o" \
                "$BUILD_DIR/lib_crypto.o" \
                -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        pacmangfx)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static $base) $obj + lib_fnt.o -> $bin"
            ld_app "$base" "$obj" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        teseraris)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static NodGL+fnt) $obj + lib_NodGL.o + lib_gfx2d.o + lib_fnt.o -> $bin"
            ld_app "$base" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        minesgfx)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static NodGL+fnt) $obj + lib_NodGL.o + lib_gfx2d.o + lib_fnt.o -> $bin"
            ld_app "$base" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        nodds)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static NodGL) $obj + lib_NodGL.o + lib_gfx2d.o + lib_fnt.o -> $bin"
            ld_app "$base" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        nodds_demo)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static NodGL) $obj + nodds_client.o + lib_fnt.o -> $bin"
            ld_app "$base" "$obj" "$BUILD_DIR/nodds_client.o" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
            
        calcgfx|snakegfx|raygfx|froggergfx|mousedemo|sysmon|miniwm|neontank|gfxtest)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static NodGL) $obj + lib_NodGL.o + lib_gfx2d.o -> $bin"
            ld_app "$base" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;

        screensaver|gfxclock)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static NodGL) $obj + lib_NodGL.o + lib_gfx2d.o + lib_fnt.o -> $bin"
            ld_app "$base" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        maze2d)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static NodGL) $obj + lib_NodGL.o + lib_gfx2d.o + lib_fnt.o -> $bin"
            ld_app "$base" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        NodGL_demo|NodGL_benchmark|NodGL_triangle|NodGL_stress_test|NodGLDiag|malwar)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static NodGL) $obj + lib_NodGL.o + lib_gfx2d.o -> $bin"
            ld_app "$base" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        mdw)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static NodGL+fnt) $obj + lib_NodGL.o + lib_gfx2d.o + lib_fnt.o -> $bin"
            ld_app "$base" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_gfx2d.o" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        demo_welcome|demo_notes|mdw_test|mdterm|mdparted_gui|mdrun|mdbindkeys|mdinfo|mdfm|mdedit|mdtaskbar|mddesktop)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static fnt) $obj + lib_fnt.o -> $bin"
            ld_app "$base" "$obj" "$BUILD_DIR/lib_fnt.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        shader_demo)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static NodGL+shader) $obj + lib_NodGL.o + lib_NodGL_shader.o + lib_sw_shader.o + lib_gfx2d.o -> $bin"
            ld_app "$base" "$obj" "$BUILD_DIR/lib_NodGL.o" "$BUILD_DIR/lib_NodGL_shader.o" "$BUILD_DIR/lib_sw_shader.o" "$BUILD_DIR/lib_gfx2d.o" -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        ntosiux_ttyman)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static ttyman) $obj + lib_NodGL.o + lib_fnt.o + lib_gfx2d.o -> $bin"
            ld_app "$base" "$obj" \
                "$BUILD_DIR/lib_NodGL.o" \
                "$BUILD_DIR/lib_fnt.o" \
                "$BUILD_DIR/lib_gfx2d.o" \
                -T "$LD_SCRIPT_APP" -o "$bin" \
                --hash-style=sysv
            ;;
        *)
            bin="$DIST_DIR/${base}.sqr"
            echo "[BUILD] LD(app static) $obj -> $bin"
            ld_app "$base" "$obj" -T "$LD_SCRIPT_APP" -o "$bin" \
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
    ld_app "coreutils/$toolname" "$obj" -T "$LD_SCRIPT_APP" -o "$bin" --hash-style=sysv
done

echo "[BUILD] Done. Outputs in $DIST_DIR"