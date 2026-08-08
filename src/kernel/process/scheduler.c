// scheduler.c - HTDS (Hybrid-Tree Decay Scheduler)
//
// vruntime-keyed red-black tree scheduler. Pick/insert/remove are all
// O(log N). The leftmost node (minimum vruntime) is found by walking left
// from the root on each pick rather than maintaining a stale cache pointer;
// this costs nothing extra because pick already pays O(log N) for removal.
//
// REFACTOR — embedded scheduler node
// -----------------------------------
// rbtree_node_t is now defined in process_new.h and embedded inside
// process_t as the field 'sched_node'.  This removes two classes of bug
// that were corrupting PID 2's page_table field:
//
//   1. kzalloc() inside rbtree_insert() could return a recycled heap block
//      that overlapped a live process_t, letting tree-pointer writes corrupt
//      arbitrary kernel data structures.
//
//   2. The sched_nodes[MAX_PROCESSES] index array could hold a stale pointer
//      after a rotation called sched_node_set() for only one of the affected
//      PIDs, making subsequent remove/insert see the wrong node address.
//
// With embedded nodes:
//   - No allocation or free ever touches the scheduling hot path.
//   - container_of() recovers the process_t* from a node pointer in O(1)
//     without any external array.
//   - Rotations never change a node's address, so the container_of result
//     is always correct regardless of how many times the tree rebalances.

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/spinlock.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/kheap.h"
#include "moduos/kernel/debug.h"
#include <stdint.h>
#include <stdbool.h>

extern process_t *process_table[MAX_PROCESSES];
extern process_t *process_find(uint32_t pid);
extern volatile process_t *current;
extern void set_curproc(process_t *p);
extern void switch_to(process_t *prev, process_t *next);

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define NICE_0_WEIGHT         1024
#define MIN_GRANULARITY_NS    750000ULL
#define SCHED_WAKEUP_BONUS_NS 1000000ULL
#define TICK_NS               1000000ULL

// Linux prio_to_weight[], indexed by (nice + 20).
static const uint32_t sched_weight_table[40] = {
    /* -20 */ 88761, 71755, 56483, 46273, 36291,
    /* -15 */ 29154, 23254, 18705, 14949, 11916,
    /* -10 */  9548,  7620,  6100,  4904,  3906,
    /*  -5 */  3121,  2501,  1991,  1586,  1277,
    /*   0 */  1024,   820,   655,   526,   423,
    /*   5 */   335,   272,   215,   172,   137,
    /*  10 */   110,    87,    70,    56,    45,
    /*  15 */    36,    29,    23,    18,    15,
};

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

// rbtree_node_t is declared in process_new.h (embedded in process_t).
// We only need the run-queue state here.

typedef struct {
    rbtree_node_t *root;
    uint64_t       min_vruntime;
    uint64_t       clock_ticks;
    uint32_t       nr_running;
} sched_state_t;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static spinlock_t    sched_lock __attribute__((aligned(64)));
static sched_state_t sched_state;
static int           sched_enabled = 0;

// ---------------------------------------------------------------------------
// Node <-> process helpers
// ---------------------------------------------------------------------------

// Get the embedded scheduler node from a process pointer.
static inline rbtree_node_t *sched_node_of(process_t *p) {
    return &p->sched_node;
}

// Get the owning process from a node pointer using container_of.
// This replaces every old use of n->process.
static inline process_t *sched_proc_of(rbtree_node_t *n) {
    return container_of(n, process_t, sched_node);
}

// Is this process currently linked into the tree?
// A freshly removed node has all link fields NULLed, so this is O(1).
static inline bool sched_node_in_tree(process_t *p) {
    rbtree_node_t *n = sched_node_of(p);
    return (n->parent != NULL) || (sched_state.root == n);
}

// ---------------------------------------------------------------------------
// Weight
// ---------------------------------------------------------------------------

static inline uint32_t sched_process_weight(process_t *p) {
    if (p->weight == 0) {
        int nice = p->nice;
        if (nice < -20) nice = -20;
        if (nice >  19) nice =  19;
        p->weight = sched_weight_table[nice + 20];
    }
    return p->weight;
}

uint32_t scheduler_nice_to_weight(int nice) {
    if (nice < -20) nice = -20;
    if (nice >  19) nice =  19;
    return sched_weight_table[nice + 20];
}

// ---------------------------------------------------------------------------
// Red-black tree — rotations
// (Identical to the original; rotations only touch node pointers so they
//  are correct with both heap-allocated and embedded nodes.)
// ---------------------------------------------------------------------------

static void rbtree_rotate_left(rbtree_node_t *n) {
    rbtree_node_t *r = n->right;

    n->right = r->left;
    if (r->left)
        r->left->parent = n;

    r->parent = n->parent;
    if      (!n->parent)            sched_state.root  = r;
    else if (n->parent->left == n)  n->parent->left   = r;
    else                            n->parent->right  = r;

    r->left   = n;
    n->parent = r;
}

static void rbtree_rotate_right(rbtree_node_t *n) {
    rbtree_node_t *l = n->left;

    n->left = l->right;
    if (l->right)
        l->right->parent = n;

    l->parent = n->parent;
    if      (!n->parent)            sched_state.root  = l;
    else if (n->parent->left == n)  n->parent->left   = l;
    else                            n->parent->right  = l;

    l->right  = n;
    n->parent = l;
}

// ---------------------------------------------------------------------------
// Red-black tree — insert fixup (CLRS)
// ---------------------------------------------------------------------------

static void rbtree_insert_fixup(rbtree_node_t *n) {
    while (n->parent && n->parent->is_red) {
        rbtree_node_t *p  = n->parent;
        rbtree_node_t *gp = p->parent;

        if (p == gp->left) {
            rbtree_node_t *uncle = gp->right;
            if (uncle && uncle->is_red) {
                p->is_red     = false;
                uncle->is_red = false;
                gp->is_red    = true;
                n = gp;
            } else {
                if (n == p->right) {
                    n = p;
                    rbtree_rotate_left(n);
                    p  = n->parent;
                    gp = p->parent;
                }
                p->is_red  = false;
                gp->is_red = true;
                rbtree_rotate_right(gp);
            }
        } else {
            rbtree_node_t *uncle = gp->left;
            if (uncle && uncle->is_red) {
                p->is_red     = false;
                uncle->is_red = false;
                gp->is_red    = true;
                n = gp;
            } else {
                if (n == p->left) {
                    n = p;
                    rbtree_rotate_right(n);
                    p  = n->parent;
                    gp = p->parent;
                }
                p->is_red  = false;
                gp->is_red = true;
                rbtree_rotate_left(gp);
            }
        }
    }
    sched_state.root->is_red = false;
}

// ---------------------------------------------------------------------------
// Red-black tree — delete fixup (CLRS)
//
// x_parent is passed separately because x may be NULL (removed black leaf).
// ---------------------------------------------------------------------------

static void rbtree_remove_fixup(rbtree_node_t *x, rbtree_node_t *x_parent) {
    while (x != sched_state.root && (!x || !x->is_red)) {
        if (!x_parent)
            break;

        if (x == x_parent->left) {
            rbtree_node_t *w = x_parent->right;

            if (w && w->is_red) {
                w->is_red        = false;
                x_parent->is_red = true;
                rbtree_rotate_left(x_parent);
                w = x_parent->right;
            }

            if (!w) {
                x        = x_parent;
                x_parent = x->parent;
            } else if ((!w->left  || !w->left->is_red) &&
                       (!w->right || !w->right->is_red)) {
                w->is_red = true;
                x         = x_parent;
                x_parent  = x->parent;
            } else {
                if (!w->right || !w->right->is_red) {
                    if (w->left) w->left->is_red = false;
                    w->is_red = true;
                    rbtree_rotate_right(w);
                    w = x_parent->right;
                }
                w->is_red        = x_parent->is_red;
                x_parent->is_red = false;
                if (w->right) w->right->is_red = false;
                rbtree_rotate_left(x_parent);
                x = sched_state.root;
                break;
            }
        } else {
            rbtree_node_t *w = x_parent->left;

            if (w && w->is_red) {
                w->is_red        = false;
                x_parent->is_red = true;
                rbtree_rotate_right(x_parent);
                w = x_parent->left;
            }

            if (!w) {
                x        = x_parent;
                x_parent = x->parent;
            } else if ((!w->right || !w->right->is_red) &&
                       (!w->left  || !w->left->is_red)) {
                w->is_red = true;
                x         = x_parent;
                x_parent  = x->parent;
            } else {
                if (!w->left || !w->left->is_red) {
                    if (w->right) w->right->is_red = false;
                    w->is_red = true;
                    rbtree_rotate_left(w);
                    w = x_parent->left;
                }
                w->is_red        = x_parent->is_red;
                x_parent->is_red = false;
                if (w->left) w->left->is_red = false;
                rbtree_rotate_right(x_parent);
                x = sched_state.root;
                break;
            }
        }
    }
    if (x) x->is_red = false;
}

// ---------------------------------------------------------------------------
// Red-black tree — transplant
// ---------------------------------------------------------------------------

static void rbtree_transplant(rbtree_node_t *u, rbtree_node_t *v) {
    if      (!u->parent)            sched_state.root  = v;
    else if (u == u->parent->left)  u->parent->left   = v;
    else                            u->parent->right  = v;
    if (v) v->parent = u->parent;
}

// ---------------------------------------------------------------------------
// Red-black tree — insert
//
// Caller must hold sched_lock.
//
// No kzalloc: we use the node embedded in the process_t directly.
// All link fields are reset so a re-enqueued process starts clean.
// ---------------------------------------------------------------------------

static void rbtree_insert(process_t *p) {
    if (sched_node_in_tree(p))
        return;

    rbtree_node_t *n = sched_node_of(p);

    // Reset every link field — the process may have been in the tree before.
    n->left     = NULL;
    n->right    = NULL;
    n->parent   = NULL;
    n->is_red   = true;
    n->vruntime = p->vruntime;

    rbtree_node_t *parent = NULL;
    rbtree_node_t *cur    = sched_state.root;
    while (cur) {
        parent = cur;
        cur = (n->vruntime < cur->vruntime) ? cur->left : cur->right;
    }

    n->parent = parent;
    if (!parent) {
        sched_state.root = n;
        n->is_red = false;
    } else if (n->vruntime < parent->vruntime) {
        parent->left = n;
        rbtree_insert_fixup(n);
    } else {
        parent->right = n;
        rbtree_insert_fixup(n);
    }

    sched_state.nr_running++;
}

// ---------------------------------------------------------------------------
// Red-black tree — remove (CLRS)
//
// Caller must hold sched_lock.
//
// Key change from the old implementation:
//   The old two-child case called sched_node_set(y->process, y) because the
//   node was a separately allocated struct and "moving" the successor meant
//   copying it to the deleted node's address, changing its heap address.
//   The external sched_nodes[pid] array then had to be updated to point at
//   the new address — and that update was the source of the corruption: if
//   the index was wrong, the next insert/remove wrote tree pointers over an
//   unrelated process_t field (PID 2's page_table in the observed crash).
//
//   With embedded nodes the node's address is always &process->sched_node.
//   The CLRS two-child deletion copies only the *key* (vruntime) and the
//   color bit into the successor slot; it does NOT move the struct itself.
//   container_of() therefore always recovers the correct process_t* and
//   sched_node_set() is simply gone.
//
// After the CLRS deletion we NULL all link fields on z so that
// sched_node_in_tree() correctly returns false for this process until the
// next enqueue.
// ---------------------------------------------------------------------------

static void rbtree_remove(process_t *p) {
    if (!sched_node_in_tree(p))
        return;

    rbtree_node_t *z = sched_node_of(p);
    sched_state.nr_running--;

    rbtree_node_t *y              = z;
    rbtree_node_t *x              = NULL;
    rbtree_node_t *x_parent       = NULL;
    bool           y_original_red = z->is_red;

    if (!z->left) {
        x        = z->right;
        x_parent = z->parent;
        rbtree_transplant(z, z->right);
    } else if (!z->right) {
        x        = z->left;
        x_parent = z->parent;
        rbtree_transplant(z, z->left);
    } else {
        // Find in-order successor (leftmost node in right subtree).
        y = z->right;
        while (y->left) y = y->left;

        y_original_red = y->is_red;
        x              = y->right;

        if (y->parent == z) {
            x_parent = y;
        } else {
            x_parent         = y->parent;
            rbtree_transplant(y, y->right);
            y->right         = z->right;
            y->right->parent = y;
        }

        rbtree_transplant(z, y);
        y->left         = z->left;
        y->left->parent = y;
        y->is_red       = z->is_red;

        // No sched_node_set() here — see block comment above.
        // container_of() always resolves to the correct process_t*
        // because the node is embedded at a fixed offset inside it.
    }

    if (!y_original_red)
        rbtree_remove_fixup(x, x_parent);

    // Mark node as "not in tree" by clearing all link fields.
    // sched_node_in_tree() tests these and will return false until
    // the process is re-inserted.
    z->left = z->right = z->parent = NULL;
    z->is_red = false;
    // No kfree — the node is part of the process_t heap allocation.
}

// ---------------------------------------------------------------------------
// min_vruntime — kept monotonically non-decreasing
// ---------------------------------------------------------------------------

static void sched_update_min_vruntime(void) {
    rbtree_node_t *n = sched_state.root;
    if (!n)
        return;
    while (n->left) n = n->left;
    if (n->vruntime > sched_state.min_vruntime)
        sched_state.min_vruntime = n->vruntime;
}

// ---------------------------------------------------------------------------
// vruntime accounting
// ---------------------------------------------------------------------------

static void sched_update_curr(process_t *p, uint64_t delta_ns) {
    p->vruntime += (delta_ns * NICE_0_WEIGHT) / sched_process_weight(p);
    sched_update_min_vruntime();
}

// ---------------------------------------------------------------------------
// Enqueue / dequeue
// ---------------------------------------------------------------------------

static void enqueue_process(process_t *p, bool is_wakeup) {
    spinlock_lock(&sched_lock);

    if (p->state == PROCESS_STATE_ZOMBIE ||
        p->state == PROCESS_STATE_TERMINATED) {
        spinlock_unlock(&sched_lock);
        return;
    }

    sched_process_weight(p);

    uint64_t floor = sched_state.min_vruntime;
    if (is_wakeup && floor >= SCHED_WAKEUP_BONUS_NS)
        floor -= SCHED_WAKEUP_BONUS_NS;

    if (p->vruntime < floor)
        p->vruntime = floor;

    rbtree_insert(p);
    p->state = PROCESS_STATE_READY;

    spinlock_unlock(&sched_lock);
}

static void dequeue_process(process_t *p) {
    spinlock_lock(&sched_lock);
    rbtree_remove(p);
    spinlock_unlock(&sched_lock);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void scheduler_init(void) {
    spinlock_init(&sched_lock);
    sched_state.root         = NULL;
    sched_state.min_vruntime = 0;
    sched_state.clock_ticks  = 0;
    sched_state.nr_running   = 0;
    sched_enabled            = 1;
    com_write_string(COM1_PORT,
        "[SCHED] HTDS (Red-Black Tree Decay Scheduler) initialized\n");
}

void scheduler_add_process(process_t *p) {
    if (p) enqueue_process(p, false);
}

void scheduler_remove_process(process_t *p) {
    if (p) dequeue_process(p);
}

// Legacy names referenced by exit_new.c and signals_new.c.
void scheduler_add(process_t *p)    { scheduler_add_process(p); }
void scheduler_remove(process_t *p) { scheduler_remove_process(p); }

uint64_t scheduler_get_min_vruntime(void) { return sched_state.min_vruntime; }
uint64_t scheduler_get_clock_ticks(void)  { return sched_state.clock_ticks;  }
uint32_t scheduler_get_nr_running(void)   { return sched_state.nr_running;   }

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------

// Minimal decimal formatter — avoids a printf dependency in the kernel COM path.
static void com_write_u64(uint64_t v) {
    char buf[21];
    int  i = 20;
    buf[i] = '\0';
    if (v == 0) {
        buf[--i] = '0';
    } else {
        while (v) {
            buf[--i] = '0' + (v % 10);
            v /= 10;
        }
    }
    com_write_string(COM1_PORT, buf + i);
}

static void rbtree_print_inorder(const rbtree_node_t *n) {
    if (!n) return;
    rbtree_print_inorder(n->left);
    process_t *p = sched_proc_of((rbtree_node_t *)n);
    com_write_string(COM1_PORT, "  pid=");
    com_write_u64(p->pid);
    com_write_string(COM1_PORT, " vrt=");
    com_write_u64(n->vruntime);
    com_write_string(COM1_PORT, " state=");
    com_write_u64(p->state);
    com_write_string(COM1_PORT, " name=");
    com_write_string(COM1_PORT, p->name ? p->name : "?");
    com_write_string(COM1_PORT, n->is_red ? " RED\n" : " BLK\n");
    rbtree_print_inorder(n->right);
}

void debug_print_ready_queue(void) {
    com_write_string(COM1_PORT, "[SCHED] Queue (vruntime order):\n");
    spinlock_lock(&sched_lock);
    rbtree_print_inorder(sched_state.root);
    spinlock_unlock(&sched_lock);
    com_write_string(COM1_PORT, "[SCHED] ---\n");
}

// ---------------------------------------------------------------------------
// schedule()
//
// The re-enqueue of prev and the pick of next happen under one lock
// acquisition so there is never a window where prev is both in the tree
// and selected as next.
// ---------------------------------------------------------------------------

static uint64_t last_sched_dump_ms = 0;
static uint64_t last_sched_dump_tick = 0;  /* add with other globals */

void schedule(void) {
    if (!sched_enabled)
        return;

    process_t *prev = (process_t *)current;
    process_t *next = NULL;

    if (prev)
        prev->need_resched = 0;

    spinlock_lock(&sched_lock);

    if (prev && prev->pid != 0 && prev->state == PROCESS_STATE_RUNNING) {
        if (prev->vruntime < sched_state.min_vruntime)
            prev->vruntime = sched_state.min_vruntime;
        rbtree_insert(prev);
        prev->state = PROCESS_STATE_READY;
    }

    /* ── QUEUE DUMP every 2 seconds (2000 ticks at 1kHz) ─────────────── */
    /* Only spam COM when debug is at max (KDBG_ON) — med/off stay quiet. */
    if (kernel_debug_is_on() &&
        sched_state.clock_ticks - last_sched_dump_tick >= 2000) {
        last_sched_dump_tick = sched_state.clock_ticks;
        com_write_string(COM1_PORT, "[SCHED] queue (nr=");
        char nbuf[16]; itoa((int)sched_state.nr_running, nbuf, 10);
        com_write_string(COM1_PORT, nbuf);
        com_write_string(COM1_PORT, ") min_vrt=");
        com_write_u64(sched_state.min_vruntime);
        com_write_string(COM1_PORT, ":\n");
        rbtree_print_inorder(sched_state.root);
        com_write_string(COM1_PORT, "[SCHED] ---\n");
    }
    /* ───────────────────────────────────────────────────────────────────── */

    rbtree_node_t *lm = sched_state.root;
    if (lm) {
        while (lm->left) lm = lm->left;
        next = sched_proc_of(lm);
        rbtree_remove(next);
    }

    spinlock_unlock(&sched_lock);

    if (!next)
        next = process_find(0);

    if (next) {
        next->state        = PROCESS_STATE_RUNNING;
        next->need_resched = 0;
    }

    if (prev != next && next) {
        set_curproc(next);
        switch_to(prev, next);
    }
}

// ---------------------------------------------------------------------------
// scheduler_tick() — timer IRQ, ~1 kHz
// ---------------------------------------------------------------------------

void scheduler_tick(void) {
    if (!sched_enabled)
        return;

    process_t *p = (process_t *)current;
    sched_state.clock_ticks++;

    if (!p)
        return;

    sched_update_curr(p, TICK_NS);

    /* The running process is not in the tree, so nr_running counts only
     * processes that are *waiting* to run.  With a single user process,
     * nr_running == 0 but we still want to mark need_resched so the IRQ
     * return path can invoke schedule() and let the idle process or any
     * newly-woken processes run.  Drop the nr_running guard. */
    if (p->vruntime > sched_state.min_vruntime + MIN_GRANULARITY_NS)
        p->need_resched = 1;

    int woke_someone = 0;

        for (int i = 1; i < MAX_PROCESSES; i++) {   /* skip PID 0 (idle) */
        process_t *sp = process_table[i];
        if (!sp)
            continue;
        if (sp->state != PROCESS_STATE_SLEEPING)
            continue;
        if (sp->sleep_ticks == 0)
            continue;

        sp->sleep_ticks--;
        if (sp->sleep_ticks == 0) {
            /*
             * Timer expired.  process_sleep() always uses the process's
             * own PID cast to a pointer as the wait channel.  Use
             * wait_channel directly to cover any future variants.
             */
            void *ch = sp->wait_channel ? sp->wait_channel
                                        : (void *)(uintptr_t)sp->pid;
            sp->wait_channel = NULL;
            wakeup(ch);
            woke_someone = 1;
        }
    }

    /*
     * If the CPU is currently idle (PID 0) and we just woke a process up,
     * don't wait for the IRQ-return preemption path to notice — it skips
     * rescheduling when interrupting kernel-mode code (idle's hlt loop),
     * so a newly-woken process would otherwise sit READY in the tree
     * forever while idle keeps halting. Calling schedule() here is safe:
     * idle holds no locks across hlt, and schedule() will pick the
     * newly-woken (lowest-vruntime) process as next.
     */
    if (woke_someone && p->pid == 0) {
        p->need_resched = 0;
        schedule();
    }
}


// ---------------------------------------------------------------------------
// sleep / wakeup
// ---------------------------------------------------------------------------

void sleep_on(void *channel) {
    if (!current)
        return;

    process_t *p    = (process_t *)current;
    p->wait_channel = channel;
    p->state        = PROCESS_STATE_SLEEPING;
    dequeue_process(p);
    schedule();
}

void wakeup(void *channel) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = process_table[i];
        if (p && p->state == PROCESS_STATE_SLEEPING &&
            p->wait_channel == channel) {
            p->wait_channel = NULL;
            enqueue_process(p, true);
        }
    }
}

// ---------------------------------------------------------------------------
// Preemption
// ---------------------------------------------------------------------------

int should_reschedule(void) {
    if (!sched_enabled || !current)
        return 0;
    return current->need_resched != 0;
}

void clear_need_resched(void) {
    if (current)
        current->need_resched = 0;
}