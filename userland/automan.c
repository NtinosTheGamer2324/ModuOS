// automan.c - Autostart Manager (ModuOS Init / PID 1)
// Full-featured init: service registry, respawn, fg/bg/run, signals, shutdown
#include "libc.h"

#define AUTOSTART_PATH   "/ModuOS/System64/auto/autostart"
#define MAX_LINE         1280
#define MAX_SERVICES     64         // Maximum tracked services
#define MAX_NAME_LEN     128        // Max service path length
#define RESPAWN_DELAY    2          // Seconds before respawning a crashed service
#define MAX_RESPAWN_RATE 5          // Max respawns within RESPAWN_WINDOW seconds
#define RESPAWN_WINDOW   30         // Sliding window for respawn rate limiting (seconds)

//  Service flags 
#define SVC_FLAG_RESPAWN    (1 << 0)  // Restart automatically on exit
#define SVC_FLAG_CRITICAL   (1 << 1)  // Halt system if service can't be kept alive
#define SVC_FLAG_ONESHOT    (1 << 2)  // Expected to exit; never respawn
#define SVC_FLAG_FG         (1 << 3)  // Foreground: automan waits for it

//  Service states 
typedef enum {
    SVC_STOPPED  = 0,
    SVC_STARTING,
    SVC_RUNNING,
    SVC_CRASHED,
    SVC_DONE,       // Oneshot exited cleanly
    SVC_DEAD,       // Respawn limit reached; given up
} svc_state_t;

//  Service descriptor 
typedef struct {
    char        path[MAX_NAME_LEN];
    int         pid;
    int         last_exit_status;
    svc_state_t state;
    unsigned int flags;
    uint64_t    started_at_ms;
    int         respawn_count;
    uint64_t    respawn_times[MAX_RESPAWN_RATE]; // Ring buffer of respawn timestamps
    int         respawn_ring_idx;
} service_t;

//  Global state 
static service_t g_services[MAX_SERVICES];
static int       g_svc_count          = 0;
static int       g_shutdown_requested = 0;
static int       g_reboot_requested   = 0;

// 
// String / parsing utilities
// 

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int parse_sleep_ms(const char *arg) {
    if (!arg || !*arg) return 0;
    char *endptr;
    long val = strtol(arg, &endptr, 10);
    if (val < 0 || val > 3600000) return 0;
    if (!*endptr)        return (int)val;
    if (*endptr == 's')  return (int)(val * 1000);
    if (*endptr == 'm')  return (int)(val * 60000);
    if (*endptr == 'h')  return (int)(val * 3600000);
    return (int)val;
}

static void strip_comments_inplace(char *line) {
    if (!line) return;
    char *in = line, *out = line;
    while (*in) {
        if (in[0] == '/' && in[1] == '/') {
            if (in == line || is_space(*(in - 1))) break;
        }
        *out++ = *in++;
    }
    *out = 0;
}

static char *trim(char *str) {
    if (!str) return NULL;
    while (*str && is_space(*str)) str++;
    if (*str == 0) return str;
    char *end = str + strlen(str) - 1;
    while (end > str && is_space(*end)) *end-- = 0;
    return str;
}

static void safe_copy(char *dst, const char *src, size_t max) {
    if (!dst || !src || max == 0) return;
    size_t i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

// 
// Service table helpers
// 

static service_t *svc_find_by_pid(int pid) {
    for (int i = 0; i < g_svc_count; i++)
        if (g_services[i].pid == pid) return &g_services[i];
    return NULL;
}

static service_t *svc_find_by_path(const char *path) {
    for (int i = 0; i < g_svc_count; i++)
        if (strcmp(g_services[i].path, path) == 0) return &g_services[i];
    return NULL;
}

static service_t *svc_register(const char *path, unsigned int flags) {
    if (g_svc_count >= MAX_SERVICES) {
        printf("[AutoMan] ERROR: service table full, cannot register %s\n", path);
        return NULL;
    }
    // Reuse a dead/done slot if available
    for (int i = 0; i < g_svc_count; i++) {
        service_t *s = &g_services[i];
        if ((s->state == SVC_DEAD || s->state == SVC_DONE) && s->pid == 0) {
            memset(s, 0, sizeof(*s));
            safe_copy(s->path, path, MAX_NAME_LEN);
            s->flags = flags;
            s->state = SVC_STOPPED;
            return s;
        }
    }
    service_t *s = &g_services[g_svc_count++];
    memset(s, 0, sizeof(*s));
    safe_copy(s->path, path, MAX_NAME_LEN);
    s->flags = flags;
    s->state = SVC_STOPPED;
    return s;
}

// 
// Respawn rate-limiter
// 

static int svc_respawn_rate_exceeded(service_t *s) {
    uint64_t now = time_ms();
    int recent = 0;
    for (int i = 0; i < MAX_RESPAWN_RATE; i++) {
        if (s->respawn_times[i] != 0 &&
            (now - s->respawn_times[i]) < (uint64_t)(RESPAWN_WINDOW * 1000))
            recent++;
    }
    return (recent >= MAX_RESPAWN_RATE);
}

static void svc_record_respawn(service_t *s) {
    s->respawn_times[s->respawn_ring_idx % MAX_RESPAWN_RATE] = time_ms();
    s->respawn_ring_idx++;
    s->respawn_count++;
}

// 
// Launch a service (fork + execve)
// 

static int svc_launch(service_t *s) {
    s->state        = SVC_STARTING;
    s->started_at_ms = time_ms();

    int pid = fork();
    if (pid < 0) {
        printf("[AutoMan] ERROR: fork() failed for %s\n", s->path);
        s->state = SVC_CRASHED;
        return -1;
    }
    if (pid == 0) {
        // Child process
        char *argv[] = { s->path, NULL };
        char *envp[] = { NULL };
        int rc = execve(s->path, argv, envp);
        printf("[AutoMan] execve failed for %s (rc=%d)\n", s->path, rc);
        exit(1);
    }
    // Parent
    s->pid   = pid;
    s->state = SVC_RUNNING;
    printf("[AutoMan] Started '%s' (PID %d)\n", s->path, pid);
    return pid;
}

// 
// Handle a child-process exit event
// 

static void handle_child_exit(int pid, int status) {
    service_t *s = svc_find_by_pid(pid);
    if (!s) {
        printf("[AutoMan] Unknown child PID %d exited (status %d)\n", pid, status);
        return;
    }

    s->last_exit_status = status;
    s->pid = 0;
    printf("[AutoMan] '%s' (PID %d) exited with status %d\n",
           s->path, pid, status);

    // Oneshot / fg: just mark done or crashed
    if (s->flags & SVC_FLAG_ONESHOT) {
        s->state = (status == 0) ? SVC_DONE : SVC_CRASHED;
        return;
    }

    // No respawn flag: just note it's down
    if (!(s->flags & SVC_FLAG_RESPAWN)) {
        s->state = SVC_CRASHED;
        printf("[AutoMan] '%s' is down (respawn=off)\n", s->path);
        return;
    }

    //  Respawn logic 
    s->state = SVC_CRASHED;

    if (svc_respawn_rate_exceeded(s)) {
        s->state = SVC_DEAD;
        printf("[AutoMan] WARN: '%s' is crash-looping (%d respawns); giving up.\n",
               s->path, s->respawn_count);
        if (s->flags & SVC_FLAG_CRITICAL) {
            printf("[AutoMan] CRITICAL service '%s' is dead — system halted!\n", s->path);
            while (1) sleep(3600);   // Hang until kernel watchdog or manual reset
        }
        return;
    }

    svc_record_respawn(s);
    printf("[AutoMan] Respawning '%s' in %d s (attempt #%d)...\n",
           s->path, RESPAWN_DELAY, s->respawn_count);
    sleep(RESPAWN_DELAY);
    svc_launch(s);
}

// 
// Graceful shutdown: SIGTERM → wait → SIGKILL
// 

static void shutdown_all_services(void) {
    printf("[AutoMan] Shutting down all services...\n");

    // Phase 1: SIGTERM
    for (int i = 0; i < g_svc_count; i++) {
        service_t *s = &g_services[i];
        if (s->pid > 0 && s->state == SVC_RUNNING) {
            printf("[AutoMan] SIGTERM -> '%s' (PID %d)\n", s->path, s->pid);
            kill(s->pid, 15 /* SIGTERM */);
        }
    }

    // Phase 2: Give them up to 5 s to exit gracefully
    for (int waited = 0; waited < 5; waited++) {
        sleep(1);
        // Reap anything that exited
        for (;;) {
            int st = 0;
            int p = waitpid(-1, &st, WNOHANG);
            if (p <= 0) break;
            service_t *s = svc_find_by_pid(p);
            if (s) { s->pid = 0; s->state = SVC_STOPPED; }
        }
        // Check if anything still alive
        int any = 0;
        for (int i = 0; i < g_svc_count; i++)
            if (g_services[i].pid > 0) { any = 1; break; }
        if (!any) break;
    }

    // Phase 3: SIGKILL stragglers
    for (int i = 0; i < g_svc_count; i++) {
        service_t *s = &g_services[i];
        if (s->pid > 0) {
            printf("[AutoMan] SIGKILL -> '%s' (PID %d)\n", s->path, s->pid);
            kill(s->pid, 9 /* SIGKILL */);
            s->pid   = 0;
            s->state = SVC_STOPPED;
        }
    }

    printf("[AutoMan] All services stopped.\n");
}

// 
// Print human-readable service status table
// 

static const char *svc_state_str(svc_state_t st) {
    switch (st) {
        case SVC_STOPPED:  return "stopped";
        case SVC_STARTING: return "starting";
        case SVC_RUNNING:  return "running";
        case SVC_CRASHED:  return "crashed";
        case SVC_DONE:     return "done";
        case SVC_DEAD:     return "dead";
        default:           return "unknown";
    }
}

static void print_service_status(void) {
    if (g_svc_count == 0) {
        printf("[AutoMan] No services registered.\n");
        return;
    }
    printf("[AutoMan] ---- Service Status (%d registered) ----\n", g_svc_count);
    for (int i = 0; i < g_svc_count; i++) {
        service_t *s = &g_services[i];
        const char *flags = "";
        if ((s->flags & SVC_FLAG_RESPAWN) && (s->flags & SVC_FLAG_CRITICAL))
            flags = "[respawn,critical]";
        else if (s->flags & SVC_FLAG_RESPAWN)
            flags = "[respawn]";
        else if (s->flags & SVC_FLAG_ONESHOT)
            flags = "[oneshot]";
        else if (s->flags & SVC_FLAG_FG)
            flags = "[fg]";

        printf("  [%d] %-36s  pid=%-6d  %-8s  respawns=%-3d  %s\n",
               i,
               s->path,
               s->pid,
               svc_state_str(s->state),
               s->respawn_count,
               flags);
    }
    printf("[AutoMan] ------------------------------------------\n");
}

// 
// Autostart file: parse and execute each line
// 

static void execute_autostart_line(char *line) {
    line = trim(line);
    if (!line || !*line) return;

    // Tokenize: <cmd> [<arg>]
    char *cmd = line;
    char *arg = NULL;
    for (int i = 0; line[i] && i < MAX_LINE; i++) {
        if (is_space(line[i])) {
            line[i] = 0;
            arg = trim(&line[i + 1]);
            break;
        }
    }

    //  run <path>   blocking oneshot 
    if (strcmp(cmd, "run") == 0) {
        if (!arg || !*arg) { printf("[AutoMan] 'run' requires a path\n"); return; }
        service_t *s = svc_register(arg, SVC_FLAG_ONESHOT);
        if (!s) return;
        int pid = svc_launch(s);
        if (pid > 0) {
            printf("[AutoMan] Waiting for: %s...\n", arg);
            int status = 0;
            waitpid(pid, &status, 0);
            s->pid              = 0;
            s->last_exit_status = status;
            s->state            = (status == 0) ? SVC_DONE : SVC_CRASHED;
            printf("[AutoMan] %s exited with %d\n", arg, status);
        }
        return;
    }

    //  fg <path>   foreground, wait for it 
    if (strcmp(cmd, "fg") == 0) {
        if (!arg || !*arg) { printf("[AutoMan] 'fg' requires a path\n"); return; }
        service_t *s = svc_register(arg, SVC_FLAG_FG | SVC_FLAG_ONESHOT);
        if (!s) return;
        int pid = svc_launch(s);
        if (pid > 0) {
            printf("[AutoMan] Foreground: %s (PID %d)\n", arg, pid);
            int status = 0;
            waitpid(pid, &status, 0);
            s->pid   = 0;
            s->state = SVC_DONE;
        }
        return;
    }

    //  bg <path>   background, no respawn 
    if (strcmp(cmd, "bg") == 0) {
        if (!arg || !*arg) { printf("[AutoMan] 'bg' requires a path\n"); return; }
        service_t *s = svc_register(arg, 0);
        if (!s) return;
        svc_launch(s);
        return;
    }

    //  service <path>   managed, auto-respawn 
    if (strcmp(cmd, "service") == 0) {
        if (!arg || !*arg) { printf("[AutoMan] 'service' requires a path\n"); return; }
        service_t *s = svc_register(arg, SVC_FLAG_RESPAWN);
        if (!s) return;
        svc_launch(s);
        return;
    }

    //  critical <path>   managed + critical: halt if it dies permanently 
    if (strcmp(cmd, "critical") == 0) {
        if (!arg || !*arg) { printf("[AutoMan] 'critical' requires a path\n"); return; }
        service_t *s = svc_register(arg, SVC_FLAG_RESPAWN | SVC_FLAG_CRITICAL);
        if (!s) return;
        svc_launch(s);
        return;
    }

    //  sleep <duration>  
    if (strcmp(cmd, "sleep") == 0) {
        if (!arg || !*arg) { printf("[AutoMan] 'sleep' requires a duration\n"); return; }
        int ms = parse_sleep_ms(arg);
        if (ms <= 0) { printf("[AutoMan] Invalid sleep duration '%s'\n", arg); return; }
        printf("[AutoMan] Sleep %d ms\n", ms);
        sleep((ms + 999) / 1000);
        return;
    }

    //  echo <message>  
    if (strcmp(cmd, "echo") == 0) {
        printf("[AutoMan] %s\n", arg ? arg : "");
        return;
    }

    //  setenv <KEY=VALUE>  
    if (strcmp(cmd, "setenv") == 0) {
        if (!arg || !*arg) { printf("[AutoMan] 'setenv' requires KEY=VALUE\n"); return; }
        if (putenv(arg) == 0)
            printf("[AutoMan] env: %s\n", arg);
        else
            printf("[AutoMan] setenv failed: %s\n", arg);
        return;
    }

    //  shutdown  
    if (strcmp(cmd, "shutdown") == 0) {
        printf("[AutoMan] Shutdown requested via autostart\n");
        g_shutdown_requested = 1;
        return;
    }

    //  reboot  
    if (strcmp(cmd, "reboot") == 0) {
        printf("[AutoMan] Reboot requested via autostart\n");
        g_reboot_requested = 1;
        return;
    }

    printf("[AutoMan] Unknown command: '%s'\n", cmd);
}

static void load_autostart(void) {
    int fd = open(AUTOSTART_PATH, 0, O_RDONLY);
    if (fd < 0) {
        printf("[AutoMan] WARNING: Could not open %s\n", AUTOSTART_PATH);
        return;
    }

    char buffer[4096];
    int total = 0, n;
    while ((n = read(fd, buffer + total, sizeof(buffer) - total - 1)) > 0) {
        total += n;
        if (total >= (int)sizeof(buffer) - 1) break;
    }
    buffer[total] = 0;
    close(fd);

    char *line_start = buffer;
    for (int i = 0; i <= total; i++) {
        if (buffer[i] == '\n' || buffer[i] == 0) {
            char old = buffer[i];
            buffer[i] = 0;
            strip_comments_inplace(line_start);
            execute_autostart_line(line_start);
            if (g_shutdown_requested || g_reboot_requested) break;
            if (old == 0) break;
            line_start = &buffer[i + 1];
        }
    }
}

// 
// PID-1 service monitor loop — runs forever
// 

static void service_monitor_loop(void) {
    printf("[AutoMan] Service monitor running. (PID 1 loop active)\n");

    static uint64_t last_status_ms = 0;

    while (!g_shutdown_requested && !g_reboot_requested) {

        // Non-blocking reap loop: catch all dead children this tick
        for (;;) {
            int status = 0;
            int pid = waitpid(-1, &status, WNOHANG);
            if (pid <= 0) break;
            handle_child_exit(pid, status);
        }

        // Periodic status dump every 60 s
        uint64_t now = time_ms();
        if (last_status_ms == 0) last_status_ms = now;
        if (now - last_status_ms >= 60000) {
            print_service_status();
            last_status_ms = now;
        }

        sleep(1);
    }

    //Orderly shutdown
    if (g_shutdown_requested || g_reboot_requested) {
        printf("[AutoMan] %s sequence initiated.\n",
               g_reboot_requested ? "Reboot" : "Shutdown");
        shutdown_all_services();
        printf("[AutoMan] %s complete. Halting.\n",
               g_reboot_requested ? "Reboot" : "Shutdown");
    }

    // PID 1 must never exit — sleep forever and let the kernel do the rest
    while (1) sleep(3600);
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("[AutoMan] ModuOS System Manager v2.0\n");
    printf("[AutoMan] PID: %d (should be 1 or 2)\n", getpid());

    memset(g_services, 0, sizeof(g_services));
    g_svc_count = 0;

    load_autostart();
    print_service_status();

    printf("[AutoMan] Autostart complete. Entering monitor loop.\n");

    service_monitor_loop();

    // NEVER REACHED
    while (1) sleep(3600);
    return 0;
}