// scheduler.c - HTDS (Hybrid-Tree Decay Scheduler) with Red-Black Tree
//
// Adapted from anyway™_internal_NTOSIUX scheduler to use global scheduler state
// while maintaining compatibility with the legacy CFS API.
//
// Red-Black Tree implementation provides O(log N) operations vs O(N) for
// the previous linked-list CFS scheduler.

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/spinlock.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/kheap.h"
#include "moduos/kernel/debug.h"
#include <stdint.h>
#include <stdbool.h>

// External declarations
extern process_t *process_table[MAX_PROCESSES];
extern process_t *process_find(uint32_t pid);
extern volatile process_t *current;

// ---------------------------------------------------------------------------
// Config & globals
// ---------------------------------------------------------------------------

#define NICE_0_WEIGHT        1024
#define MIN_GRANULARITY_NS   750000ULL         // 0.75 ms
#define SCHED_WAKEUP_BONUS_NS 1000000ULL       // 1 ms I/O boost

// Weight table (Linux prio_to_weight[], nice -20..19)
static const uint32_t nice_to_weight_table[40] = {
    /* -20 */ 88761, 71755, 56483, 46273, 36291,
    /* -15 */ 29154, 23254, 18705, 14949, 11916,
    /* -10 */  9548,  7620,  6100,  4904,  3906,
    /*  -5 */  3121,  2501,  1991,  1586,  1277,
    /*   0 */  1024,   820,   655,   526,   423,
    /*   5 */   335,   272,   215,   172,   137,
    /*  10 */   110,    87,    70,    56,    45,
    /*  15 */    36,    29,    23,    18,    15,
};

// Red-Black Tree node for scheduling tree
typedef struct rbtree_node {
    struct rbtree_node *left;
    struct rbtree_node *right;
    struct rbtree_node *parent;
    bool is_red;
    process_t *process;
    uint64_t vruntime;
} rbtree_node_t;

// Scheduler state
typedef struct {
    rbtree_node_t *root;
    rbtree_node_t *leftmost;
    uint64_t min_vruntime;
    uint64_t clock_ticks;
} sched_state_t;

static spinlock_t sched_lock __attribute__((aligned(64)));
static sched_state_t g_sched;
static int sched_enabled = 0;

// Node tracking: indexed by PID to map process -> rbtree_node
static rbtree_node_t *g_sched_nodes[MAX_PROCESSES];

static inline void set_sched_node(process_t *p, rbtree_node_t *node) {
    if (p && p->pid < MAX_PROCESSES) g_sched_nodes[p->pid] = node;
}

static inline rbtree_node_t *get_sched_node(process_t *p) {
    if (!p || p->pid >= MAX_PROCESSES) return NULL;
    return g_sched_nodes[p->pid];
}

// ---------------------------------------------------------------------------
// Weight helper
// ---------------------------------------------------------------------------

static uint32_t nice_to_weight(int nice) {
    if (nice < -20) nice = -20;
    if (nice >  19) nice =  19;
    return nice_to_weight_table[nice + 20];
}

uint32_t scheduler_nice_to_weight(int nice) { return nice_to_weight(nice); }

// ---------------------------------------------------------------------------
// Red-Black Tree operations
// ---------------------------------------------------------------------------

static inline rbtree_node_t *rbtree_successor(rbtree_node_t *node) {
    if (node->right != NULL) {
        node = node->right;
        while (node->left != NULL) node = node->left;
        return node;
    }
    return NULL;
}

static inline void rbtree_rotate_left(rbtree_node_t *node) {
    rbtree_node_t *right_child = node->right;
    if (!right_child) return;

    node->right = right_child->left;
    if (right_child->left) right_child->left->parent = node;

    right_child->parent = node->parent;
    if (!node->parent) {
        g_sched.root = right_child;
    } else if (node->parent->left == node) {
        node->parent->left = right_child;
    } else {
        node->parent->right = right_child;
    }

    right_child->left = node;
    node->parent = right_child;
}

static inline void rbtree_rotate_right(rbtree_node_t *node) {
    rbtree_node_t *left_child = node->left;
    if (!left_child) return;

    node->left = left_child->right;
    if (left_child->right) left_child->right->parent = node;

    left_child->parent = node->parent;
    if (!node->parent) {
        g_sched.root = left_child;
    } else if (node->parent->left == node) {
        node->parent->left = left_child;
    } else {
        node->parent->right = left_child;
    }

    left_child->right = node;
    node->parent = left_child;
}

static inline void rbtree_insert_fixup(rbtree_node_t *node) {
    while (node->parent && node->parent->is_red) {
        if (node->parent == node->parent->parent->left) {
            rbtree_node_t *uncle = node->parent->parent->right;

            if (uncle && uncle->is_red) {
                node->parent->is_red = false;
                uncle->is_red = false;
                node->parent->parent->is_red = true;
                node = node->parent->parent;
            } else {
                if (node == node->parent->right) {
                    node = node->parent;
                    rbtree_rotate_left(node);
                }
                node->parent->is_red = false;
                node->parent->parent->is_red = true;
                rbtree_rotate_right(node->parent->parent);
            }
        } else {
            rbtree_node_t *uncle = node->parent->parent->left;

            if (uncle && uncle->is_red) {
                node->parent->is_red = false;
                uncle->is_red = false;
                node->parent->parent->is_red = true;
                node = node->parent->parent;
            } else {
                if (node == node->parent->left) {
                    node = node->parent;
                    rbtree_rotate_right(node);
                }
                node->parent->is_red = false;
                node->parent->parent->is_red = true;
                rbtree_rotate_left(node->parent->parent);
            }
        }
    }
    if (g_sched.root) g_sched.root->is_red = false;
}

static inline void rbtree_remove_fixup(rbtree_node_t *node, rbtree_node_t *parent) {
    while (node != g_sched.root && !node->is_red) {
        if (node == parent->left) {
            rbtree_node_t *sibling = parent->right;

            if (sibling && sibling->is_red) {
                sibling->is_red = false;
                parent->is_red = true;
                rbtree_rotate_left(parent);
                sibling = parent->right;
            }

            if (sibling && 
                (!sibling->left || !sibling->left->is_red) &&
                (!sibling->right || !sibling->right->is_red)) {
                sibling->is_red = true;
                node = parent;
                parent = node->parent;
            } else if (sibling) {
                if (!sibling->right || !sibling->right->is_red) {
                    if (sibling->left) sibling->left->is_red = false;
                    sibling->is_red = true;
                    rbtree_rotate_right(sibling);
                    sibling = parent->right;
                }
                sibling->is_red = parent->is_red;
                parent->is_red = false;
                if (sibling->right) sibling->right->is_red = false;
                rbtree_rotate_left(parent);
                node = g_sched.root;
                break;
            }
        } else {
            rbtree_node_t *sibling = parent->left;

            if (sibling && sibling->is_red) {
                sibling->is_red = false;
                parent->is_red = true;
                rbtree_rotate_right(parent);
                sibling = parent->left;
            }

            if (sibling && 
                (!sibling->right || !sibling->right->is_red) &&
                (!sibling->left || !sibling->left->is_red)) {
                sibling->is_red = true;
                node = parent;
                parent = node->parent;
            } else if (sibling) {
                if (!sibling->left || !sibling->left->is_red) {
                    if (sibling->right) sibling->right->is_red = false;
                    sibling->is_red = true;
                    rbtree_rotate_left(sibling);
                    sibling = parent->left;
                }
                sibling->is_red = parent->is_red;
                parent->is_red = false;
                if (sibling->left) sibling->left->is_red = false;
                rbtree_rotate_right(parent);
                node = g_sched.root;
                break;
            }
        }
    }
    if (node) node->is_red = false;
}

// ---------------------------------------------------------------------------
// Tree insertion
// ---------------------------------------------------------------------------

static void rbtree_insert(process_t *p) {
    if (!p) return;

    rbtree_node_t *node = (rbtree_node_t *)kzalloc(sizeof(rbtree_node_t));
    if (!node) return;

    node->process = p;
    node->vruntime = p->vruntime;
    node->is_red = true;
    node->left = NULL;
    node->right = NULL;
    node->parent = NULL;

    set_sched_node(p, node);

    if (g_sched.root == NULL) {
        g_sched.root = node;
        node->is_red = false;
        g_sched.leftmost = node;
        return;
    }

    rbtree_node_t *current = g_sched.root;
    rbtree_node_t *parent = NULL;

    while (current != NULL) {
        parent = current;
        if (node->vruntime < current->vruntime) {
            current = current->left;
        } else {
            current = current->right;
        }
    }

    node->parent = parent;
    if (node->vruntime < parent->vruntime) {
        parent->left = node;
    } else {
        parent->right = node;
    }

    if (node->vruntime < g_sched.leftmost->vruntime) {
        g_sched.leftmost = node;
    }

    rbtree_insert_fixup(node);
}

// ---------------------------------------------------------------------------
// Tree removal
// ---------------------------------------------------------------------------

static void rbtree_remove(process_t *p) {
    if (!p) return;

    rbtree_node_t *node = get_sched_node(p);
    if (!node) return;
    rbtree_node_t *child, *parent;
    bool node_was_red = node->is_red;

    if (node->left == NULL) {
        child = node->right;
        parent = node->parent;

        if (child != NULL) child->parent = parent;
        if (parent == NULL) {
            g_sched.root = child;
        } else if (parent->left == node) {
            parent->left = child;
        } else {
            parent->right = child;
        }

        if (g_sched.leftmost == node) {
            g_sched.leftmost = (child != NULL) ? child : parent;
        }
    } else if (node->right == NULL) {
        child = node->left;
        parent = node->parent;

        if (parent == NULL) {
            g_sched.root = child;
        } else if (parent->left == node) {
            parent->left = child;
        } else {
            parent->right = child;
        }

        child->parent = parent;

        if (g_sched.leftmost == node) {
            g_sched.leftmost = child;
        }
    } else {
        rbtree_node_t *successor = rbtree_successor(node);
        node->vruntime = successor->vruntime;
        node->process = successor->process;
        set_sched_node(successor->process, node);

        rbtree_remove(successor->process);
        return;
    }

    if (!node_was_red && child != NULL) {
        rbtree_remove_fixup(child, parent);
    }

    kfree(node);
    set_sched_node(p, NULL);
}

// ---------------------------------------------------------------------------
// Public API for process.c
// ---------------------------------------------------------------------------

void scheduler_init(void) {
    spinlock_init(&sched_lock);
    g_sched.root = NULL;
    g_sched.leftmost = NULL;
    g_sched.min_vruntime = 0;
    g_sched.clock_ticks = 0;
    sched_enabled = 1;
    com_write_string(COM1_PORT, "[SCHED] HTDS (Red-Black Tree Decay Scheduler) initialized\n");
}

// Compatibility alias
void scheduler_compat_init(void) { /* no-op */ }

void scheduler_add_process(process_t *p) {
    if (!p) return;
    spinlock_lock(&sched_lock);
    if (p->state != PROCESS_STATE_ZOMBIE && p->state != PROCESS_STATE_TERMINATED) {
        if (p->weight == 0) p->weight = nice_to_weight(p->nice);
        if (p->vruntime < g_sched.min_vruntime) p->vruntime = g_sched.min_vruntime;
        rbtree_insert(p);
        p->state = PROCESS_STATE_READY;
    }
    spinlock_unlock(&sched_lock);
}

void scheduler_add(process_t *p) { scheduler_add_process(p); }

void scheduler_remove_process(process_t *p) {
    if (!p) return;
    spinlock_lock(&sched_lock);
    rbtree_remove(p);
    spinlock_unlock(&sched_lock);
}

void scheduler_remove(process_t *p) { scheduler_remove_process(p); }

// Getters
uint64_t scheduler_get_min_vruntime(void)  { return g_sched.min_vruntime; }
uint64_t scheduler_get_clock_ticks(void)   { return g_sched.clock_ticks;  }

// Pick next process to run
static process_t *pick_next(void) {
    spinlock_lock(&sched_lock);
    process_t *p = NULL;
    if (g_sched.leftmost) {
        p = g_sched.leftmost->process;
        rbtree_remove(p);
    }
    spinlock_unlock(&sched_lock);
    return p;
}

// Re-enqueue a process
static void requeue(process_t *p) {
    if (!p) return;
    spinlock_lock(&sched_lock);
    if (p->state != PROCESS_STATE_ZOMBIE && p->state != PROCESS_STATE_TERMINATED) {
        if (p->weight == 0) p->weight = nice_to_weight(p->nice);
        if (p->vruntime < g_sched.min_vruntime) p->vruntime = g_sched.min_vruntime;
        rbtree_insert(p);
        p->state = PROCESS_STATE_READY;
    }
    spinlock_unlock(&sched_lock);
}

// Update vruntime based on execution time
static void update_curr(process_t *p, uint64_t delta_ns) {
    if (!p) return;
    if (p->weight == 0) p->weight = nice_to_weight(p->nice);
    if (p->weight == 0) p->weight = NICE_0_WEIGHT;

    p->vruntime += (delta_ns * NICE_0_WEIGHT) / p->weight;

    // Advance min_vruntime
    if (g_sched.leftmost && g_sched.leftmost->vruntime > g_sched.min_vruntime)
        g_sched.min_vruntime = g_sched.leftmost->vruntime;
}

// ---------------------------------------------------------------------------
// schedule() — called from process.c
// ---------------------------------------------------------------------------

void schedule(void) {
    if (!sched_enabled) return;

    process_t *prev = (process_t *)current;
    process_t *next = NULL;

    if (prev) prev->need_resched = 0;

    if (prev && prev->pid != 0 &&
        (prev->state == PROCESS_STATE_RUNNING  ||
         prev->state == PROCESS_STATE_RUNNABLE ||
         prev->state == PROCESS_STATE_READY)) {
        requeue(prev);
    }

    next = pick_next();
    if (!next) next = process_find(0);

    if (next) {
        next->state = PROCESS_STATE_RUNNING;
        next->need_resched = 0;
    }

    if (prev != next && next) {
        extern void set_curproc(process_t *p);
        set_curproc(next);

        extern void switch_to(process_t *prev, process_t *next);
        switch_to(prev, next);
    }
}

// ---------------------------------------------------------------------------
// scheduler_tick() — called from timer IRQ (~1 kHz)
// ---------------------------------------------------------------------------

void scheduler_tick(void) {
    if (!sched_enabled) return;

    process_t *curr_cast = (process_t *)current;

    g_sched.clock_ticks++;

    update_curr(curr_cast, 1000000ULL /* 1 ms per tick */);

    if (g_sched.min_vruntime > 0 && curr_cast->vruntime > g_sched.min_vruntime + MIN_GRANULARITY_NS)
        curr_cast->need_resched = 1;
}

// ---------------------------------------------------------------------------
// sleep / wakeup
// ---------------------------------------------------------------------------

void sleep_on(void *channel) {
    if (!current) return;
    
    process_t *curr_cast = (process_t *)current;
    curr_cast->wait_channel = channel;
    curr_cast->state = PROCESS_STATE_SLEEPING;
    scheduler_remove_process(curr_cast);
    schedule();
}

void wakeup(void *channel) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = process_table[i];
        if (p && p->state == PROCESS_STATE_SLEEPING &&
            p->wait_channel == channel) {
            p->wait_channel = NULL;
            scheduler_add_process(p);
        }
    }
}

// ---------------------------------------------------------------------------
// Preemption helpers
// ---------------------------------------------------------------------------

int should_reschedule(void) {
    if (!sched_enabled || !current) return 0;
    if (current->need_resched) return 1;
    if (current->state != PROCESS_STATE_RUNNING) return 1;
    return 0;
}

void clear_need_resched(void) {
    if (current) current->need_resched = 0;
}

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------

void debug_print_ready_queue(void) {
    if (!kernel_debug_is_on()) return;
    com_write_string(COM1_PORT, "[SCHED-DEBUG] Ready queue (in-order traversal):\n");
    // Would need in-order tree walk here for full debugging
    com_write_string(COM1_PORT, "[SCHED-DEBUG] Red-Black Tree structure (simplified output)\n");
}
