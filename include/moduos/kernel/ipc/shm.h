// shm.h — POSIX-style shared memory (shm_open + mmap(MAP_SHARED))
//
// Deliberately NOT built on DevFS or the regular fd table. shm_open()
// returns a small kernel-local handle whose only valid use is as the `fd`
// argument to sys_mmap() when MAP_SHARED is set. There is no shm_close():
// the handle is consumed the moment mmap() successfully maps it (or leaked
// harmlessly, bounded by SHM_MAX_HANDLES, if the caller never mmaps it).
//
// Design in one paragraph: a named segment (shm_object_t) owns an array of
// physical frames allocated once at creation. Object *metadata* lifetime
// (the frames[] array + the name-table slot) is tracked with its own small
// refcount (obj->refcount) — bumped by shm_open(), dropped when a handle is
// consumed by mmap() or when shm_unlink() removes the name. Physical *page*
// lifetime is tracked completely separately by the existing phys.c
// per-frame refcounts (phys_ref_inc/phys_ref_dec), which sys_munmap(),
// process_free_user_memory() and fork() already touch unmodified — a
// shared page just has refcount > 1 instead of 1, and phys_free_frame()
// already only actually frees at refcount 0. Two independent, already-
// correct refcounts, composed instead of reinvented.
//
// This is a deliberate simplification vs. real POSIX shm_open(): size is
// given directly to shm_open() (no separate ftruncate()), and mmap() of a
// shared handle must request the *entire* object (no partial/offset
// mappings) — matching the no-offset limitation sys_mmap() already has for
// regular file-backed mappings.

#ifndef MODUOS_KERNEL_IPC_SHM_H
#define MODUOS_KERNEL_IPC_SHM_H

#include <stdint.h>
#include <stddef.h>

#define SHM_NAME_MAX     64
#define SHM_MAX_OBJECTS  64
#define SHM_MAX_HANDLES  128

/* fd.h's O_* flags don't include O_EXCL (nothing else in ModuOS needed it
 * yet). Defined here, one bit past fd.h's highest (O_NONBLOCK = 0x0800),
 * so it's safe to OR into the same oflags word shm_open() takes. */
#ifndef O_EXCL
#define O_EXCL 0x1000
#endif

/* Call once during kernel init, after the heap/physical allocator are up
 * (anywhere near fd_init() is fine — shm has no filesystem dependency). */
void shm_init(void);

/* shm_open(name, oflags, mode, size)
 *
 * oflags: O_CREAT, O_EXCL, O_RDONLY, O_RDWR (from fd.h). O_EXCL without
 *   O_CREAT is ignored, like POSIX.
 * mode:   stored for informational purposes only; not enforced (ModuOS's
 *   uid/gid model doesn't reach this subsystem yet).
 * size:   required (>0) when creating a new object; rounded up to a page.
 *   When opening an *existing* object, pass 0 to accept its current size,
 *   or the exact existing size — a mismatched non-zero size is EINVAL.
 *
 * Returns a handle (>=0) valid for exactly one mmap(MAP_SHARED, handle)
 * call, or a negative -errno.
 */
int shm_open(const char *name, int oflags, uint32_t mode, uint64_t size);

/* Removes `name` from the namespace so future shm_open() calls can't find
 * it. Existing mappings (and any handle already issued but not yet
 * mmap()'d) keep working; the underlying pages are only actually freed once
 * every mapping using them has gone away (via the ordinary munmap/exit
 * path, unmodified). Returns 0 or -errno (ENOENT). */
int shm_unlink(const char *name);

/* --- Internal API used only by sys_mmap(); not a syscall. --- */

/* Resolve and CONSUME a handle: on success, writes the object's physical
 * frame list into *out_frames (owned by the object, do not free) and its
 * page count into *out_frame_count, and reports whether the handle was
 * opened with write access. The handle slot is freed either way (matching
 * "one mmap per shm_open()"). Returns 0 on success, -errno on failure
 * (EBADF for an unknown/already-consumed handle). */
int shm_handle_take(int handle, uint64_t **out_frames, size_t *out_frame_count,
                    int *out_can_write);

#endif /* MODUOS_KERNEL_IPC_SHM_H */
