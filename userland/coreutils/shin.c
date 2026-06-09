// shin.c — SHell INterpreter for ModuOS (.sh scripts)
// Usage: shin script.sh [args...]   OR   shin -c "command"

#include "libc.h"

#define SHIN_MAX_LINE    1024
#define SHIN_MAX_VARS    128
#define SHIN_MAX_WORDS   32
#define SHIN_MAX_STACK   16
#define SHIN_MAX_SCRIPT  (128 * 1024)
#define SHIN_LOOP_LINES  256

// -- Variable store

typedef struct {
    char k[64];
    char v[256];
} shin_var_t;

static shin_var_t shin_vars[SHIN_MAX_VARS];
static int        shin_nvar = 0;
static int        shin_last_status = 0;
static char     **shin_argv;
static int        shin_argc;

static const char *var_get(const char *k)
{
    // Special positional / status variables handled inline to avoid
    // polluting the var store with ephemeral state.
    static char shin_special_buf[32];
    if (!strcmp(k, "?")) {
        sprintf(shin_special_buf, "%d", shin_last_status);
        return shin_special_buf;
    }
    if (!strcmp(k, "#")) {
        sprintf(shin_special_buf, "%d", shin_argc ? shin_argc - 1 : 0);
        return shin_special_buf;
    }
    if (!strcmp(k, "@")) {
        static char shin_at_buf[SHIN_MAX_LINE];
        shin_at_buf[0] = 0;
        for (int i = 1; i < shin_argc; i++) {
            if (i > 1) strlcat(shin_at_buf, " ", sizeof(shin_at_buf));
            strlcat(shin_at_buf, shin_argv[i], sizeof(shin_at_buf));
        }
        return shin_at_buf;
    }
    if (k[0] >= '0' && k[0] <= '9' && !k[1]) {
        int i = k[0] - '0';
        return i < shin_argc ? shin_argv[i] : "";
    }

    for (int i = 0; i < shin_nvar; i++)
        if (!strcmp(shin_vars[i].k, k))
            return shin_vars[i].v;
    return "";
}

static void var_set(const char *k, const char *v)
{
    for (int i = 0; i < shin_nvar; i++) {
        if (!strcmp(shin_vars[i].k, k)) {
            strlcpy(shin_vars[i].v, v, 256);
            return;
        }
    }
    if (shin_nvar < SHIN_MAX_VARS) {
        strlcpy(shin_vars[shin_nvar].k, k, 64);
        strlcpy(shin_vars[shin_nvar].v, v, 256);
        shin_nvar++;
    }
}

static void var_unset(const char *k)
{
    for (int i = 0; i < shin_nvar; i++) {
        if (!strcmp(shin_vars[i].k, k)) {
            shin_vars[i] = shin_vars[--shin_nvar];
            return;
        }
    }
}

// -- Expansion

static void expand_vars(const char *s, char *d, size_t dsz)
{
    size_t di = 0;
    while (*s && di + 1 < dsz) {
        if (*s == '\\' && s[1]) {
            d[di++] = s[1];
            s += 2;
            continue;
        }
        if (*s != '$') {
            d[di++] = *s++;
            continue;
        }
        s++;
        char name[64];
        size_t ni = 0;
        if (*s == '{') {
            s++;
            while (*s && *s != '}' && ni + 1 < 64) name[ni++] = *s++;
            if (*s == '}') s++;
        } else if (*s == '?' || *s == '#' || *s == '@' ||
                   (*s >= '0' && *s <= '9')) {
            name[ni++] = *s++;
        } else {
            while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
                   (*s >= '0' && *s <= '9') || *s == '_') {
                if (ni + 1 < 64) name[ni++] = *s;
                s++;
            }
        }
        name[ni] = 0;
        if (!ni) {
            d[di++] = '$';
            continue;
        }
        const char *val = var_get(name);
        while (*val && di + 1 < dsz) d[di++] = *val++;
    }
    d[di] = 0;
}

// Forward declaration required for $(...) expansion.
static int shin_exec_line(const char *line);

// Forks a child, redirects its stdout to a pipe, and captures the output.
// Trailing newlines are stripped to match POSIX $() semantics.
static void expand_cmdsub(const char *s, char *d, size_t dsz)
{
    size_t di = 0;
    while (*s && di + 1 < dsz) {
        if (s[0] != '$' || s[1] != '(') {
            d[di++] = *s++;
            continue;
        }
        s += 2;
        char cmd[SHIN_MAX_LINE];
        size_t ci = 0;
        int depth = 1;
        while (*s && depth > 0) {
            if (*s == '(') depth++;
            else if (*s == ')') depth--;
            if (depth > 0 && ci + 1 < sizeof(cmd)) cmd[ci++] = *s;
            s++;
        }
        cmd[ci] = 0;

        int fds[2];
        if (pipe(fds) != 0) continue;
        int pid = fork();
        if (pid == 0) {
            close(fds[0]);
            dup2(fds[1], STDOUT_FILENO);
            close(fds[1]);
            exit(shin_exec_line(cmd));
        }
        close(fds[1]);
        char rb[512];
        ssize_t n;
        while ((n = read(fds[0], rb, sizeof(rb) - 1)) > 0) {
            rb[n] = 0;
            while (n > 0 && (rb[n-1] == '\n' || rb[n-1] == '\r'))
                rb[--n] = 0;
            for (ssize_t i = 0; i < n && di + 1 < dsz; i++)
                d[di++] = rb[i];
        }
        close(fds[0]);
        int st = 0;
        waitpid(pid, &st, 0);
    }
    d[di] = 0;
}

static void expand_full(const char *s, char *d, size_t dsz)
{
    char tmp[SHIN_MAX_LINE * 2];
    expand_cmdsub(s, tmp, sizeof(tmp));
    expand_vars(tmp, d, dsz);
}

// -- Word splitting

static int words_split(const char *line, char **w, int maxw,
                       char *pool, size_t psz)
{
    int wc = 0;
    size_t pi = 0;
    const char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') break;
        if (*p == ';') { p++; break; }
        if (wc >= maxw) break;
        w[wc++] = pool + pi;
        while (*p && *p != ' ' && *p != '\t' && *p != ';') {
            if (*p == '\'') {
                p++;
                while (*p && *p != '\'') {
                    if (pi + 1 < psz) pool[pi++] = *p;
                    p++;
                }
                if (*p) p++;
            } else if (*p == '"') {
                p++;
                char inner[SHIN_MAX_LINE];
                size_t ii = 0;
                while (*p && *p != '"') {
                    if (ii + 1 < sizeof(inner)) inner[ii++] = *p;
                    p++;
                }
                inner[ii] = 0;
                if (*p) p++;
                char ex[SHIN_MAX_LINE];
                expand_vars(inner, ex, sizeof(ex));
                for (const char *e = ex; *e && pi + 1 < psz; e++)
                    pool[pi++] = *e;
            } else if (*p == '\\') {
                p++;
                if (*p && pi + 1 < psz) pool[pi++] = *p++;
            } else {
                if (pi + 1 < psz) pool[pi++] = *p++;
            }
        }
        if (pi < psz) pool[pi++] = 0;
    }
    return wc;
}

// -- Condition evaluation

// Returns 0 for true, non-zero for false (POSIX test semantics).
static int cond_eval(char **w, int wc)
{
    int s = 0, e = wc;
    if (wc > 0 && (!strcmp(w[0], "[") || !strcmp(w[0], "test"))) s = 1;
    if (wc > 0 && !strcmp(w[wc-1], "]")) e--;
    int c = e - s;
    char **a = w + s;
    if (c == 0) return 1;
    if (c == 2) {
        if (!strcmp(a[0], "-z")) return strlen(a[1]) == 0 ? 0 : 1;
        if (!strcmp(a[0], "-n")) return strlen(a[1]) != 0 ? 0 : 1;
        if (!strcmp(a[0], "-f")) {
            fs_file_info_t fi;
            return stat(a[1], &fi) == 0 ? 0 : 1;
        }
    }
    if (c == 3) {
        const char *L = a[0], *op = a[1], *R = a[2];
        if (!strcmp(op, "=")  || !strcmp(op, "==")) return !strcmp(L, R)  ? 0 : 1;
        if (!strcmp(op, "!="))                      return  strcmp(L, R)  ? 0 : 1;
        int l = atoi(L), r = atoi(R);
        if (!strcmp(op, "-eq")) return l == r ? 0 : 1;
        if (!strcmp(op, "-ne")) return l != r ? 0 : 1;
        if (!strcmp(op, "-lt")) return l <  r ? 0 : 1;
        if (!strcmp(op, "-gt")) return l >  r ? 0 : 1;
        if (!strcmp(op, "-le")) return l <= r ? 0 : 1;
        if (!strcmp(op, "-ge")) return l >= r ? 0 : 1;
    }
    return 1;
}

// -- if-stack

typedef struct {
    int active;
    int done;
} shin_ifframe_t;

static shin_ifframe_t shin_ifs[SHIN_MAX_STACK];
static int shin_ifdepth = 0;

static int ifstack_is_active(void)
{
    for (int i = 0; i < shin_ifdepth; i++)
        if (!shin_ifs[i].active) return 0;
    return 1;
}

// -- Loop body buffer

typedef struct {
    char ln[SHIN_LOOP_LINES][SHIN_MAX_LINE];
    int  n;
} shin_lbuf_t;

// -- Script reader

typedef struct {
    const char *d;
    size_t      sz;
    size_t      pos;
} shin_reader_t;

static int reader_next_line(shin_reader_t *r, char *buf, size_t bsz)
{
    if (r->pos >= r->sz) return 0;
    size_t bi = 0;
    while (r->pos < r->sz && bi + 1 < bsz) {
        char c = r->d[r->pos++];
        if (c == '\r') continue;
        if (c == '\n') {
            // Line continuation: fold into the caller's buffer.
            if (bi > 0 && buf[bi-1] == '\\') { bi--; continue; }
            break;
        }
        buf[bi++] = c;
    }
    buf[bi] = 0;
    return 1;
}

// -- Builtins + dispatch

static int shin_run_file(const char *path);

static int shin_exec_words(char **w, int wc)
{
    if (!wc) return 0;
    const char *cmd = w[0];

    if (!strcmp(cmd, "echo")) {
        int nl = 1, s = 1;
        if (wc > 1 && !strcmp(w[1], "-n")) { nl = 0; s = 2; }
        for (int i = s; i < wc; i++) {
            if (i > s) putc(' ');
            puts_raw(w[i]);
        }
        if (nl) putc('\n');
        return 0;
    }
    if (!strcmp(cmd, "cd")) {
        const char *p = wc > 1 ? w[1] : "/";
        if (chdir(p)) { printf("cd: %s: failed\n", p); return 1; }
        return 0;
    }
    if (!strcmp(cmd, "pwd")) {
        char b[512];
        if (getcwd(b, sizeof(b))) puts(b);
        return 0;
    }
    if (!strcmp(cmd, "exit"))
        exit(wc > 1 ? atoi(w[1]) : shin_last_status);
    if (!strcmp(cmd, "true"))  return 0;
    if (!strcmp(cmd, "false")) return 1;
    if (!strcmp(cmd, "sleep")) {
        sleep(wc > 1 ? (unsigned)atoi(w[1]) : 1);
        return 0;
    }
    if (!strcmp(cmd, "read")) {
        char b[SHIN_MAX_LINE];
        ssize_t n = input_line_to_buffer(b, sizeof(b));
        if (wc > 1 && n >= 0) var_set(w[1], b);
        return n < 0 ? 1 : 0;
    }
    if (!strcmp(cmd, "unset")) {
        for (int i = 1; i < wc; i++) var_unset(w[i]);
        return 0;
    }
    if (!strcmp(cmd, "source") || !strcmp(cmd, ".")) {
        if (wc < 2) { puts("source: filename required"); return 1; }
        return shin_run_file(w[1]);
    }
    if (!strcmp(cmd, "expr")) {
        if (wc < 4) return 1;
        long a = atoi(w[1]), b = atoi(w[3]);
        long res = 0;
        if      (!strcmp(w[2], "+")) res = a + b;
        else if (!strcmp(w[2], "-")) res = a - b;
        else if (!strcmp(w[2], "*")) res = a * b;
        else if (!strcmp(w[2], "/")) res = b ? a / b : 0;
        else if (!strcmp(w[2], "%")) res = b ? a % b : 0;
        else return 1;
        char buf[32];
        sprintf(buf, "%ld", res);
        puts(buf);
        return 0;
    }
    if (!strcmp(cmd, "test") || !strcmp(cmd, "["))
        return cond_eval(w, wc);

    // Variable assignment: KEY=value (no subprocess).
    char *eq = strchr(cmd, '=');
    if (eq && eq != cmd) {
        int valid = 1;
        for (const char *c = cmd; c < eq; c++) {
            if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                  (*c >= '0' && *c <= '9') || *c == '_')) {
                valid = 0;
                break;
            }
        }
        if (valid) {
            char name[64];
            size_t l = (size_t)(eq - cmd);
            if (l >= 64) l = 63;
            memcpy(name, cmd, l);
            name[l] = 0;
            var_set(name, eq + 1);
            return 0;
        }
    }

    // External program.
    char *av[SHIN_MAX_WORDS + 1];
    for (int i = 0; i < wc; i++) av[i] = w[i];
    av[wc] = NULL;
    int pid = fork();
    if (pid == 0) {
        execve(w[0], av, NULL);
        printf("shin: %s: not found\n", w[0]);
        exit(127);
    }
    if (pid > 0) {
        int st = 0;
        waitpid(pid, &st, 0);
        return st;
    }
    return 1;
}

static int shin_exec_line(const char *raw)
{
    while (*raw == ' ' || *raw == '\t') raw++;
    if (!*raw || *raw == '#') return 0;

    char ex[SHIN_MAX_LINE * 2];
    expand_full(raw, ex, sizeof(ex));

    // Short-circuit operators are resolved before word-splitting so that
    // both sides are expanded independently with up-to-date $?.
    char *ap = strstr(ex, " && ");
    char *op = strstr(ex, " || ");
    if (ap || op) {
        char *pp = (ap && (!op || ap < op)) ? ap : op;
        int is_and = (pp == ap);
        char left[SHIN_MAX_LINE];
        size_t l = (size_t)(pp - ex);
        if (l >= SHIN_MAX_LINE) l = SHIN_MAX_LINE - 1;
        memcpy(left, ex, l);
        left[l] = 0;
        int lr = shin_exec_line(left);
        shin_last_status = lr;
        if (is_and) return lr == 0 ? shin_exec_line(pp + 4) : lr;
        else        return lr != 0 ? shin_exec_line(pp + 4) : 0;
    }

    // Semicolons outside quotes become recursive calls so that each
    // statement updates $? before the next one reads it.
    {
        char buf[SHIN_MAX_LINE * 2];
        strlcpy(buf, ex, sizeof(buf));
        int sq = 0, dq = 0;
        char *semi = NULL;
        for (char *c = buf; *c; c++) {
            if (*c == '\'' && !dq) sq = !sq;
            else if (*c == '"' && !sq) dq = !dq;
            else if (*c == ';' && !sq && !dq) { semi = c; break; }
        }
        if (semi) {
            *semi = 0;
            int r = shin_exec_line(buf);
            shin_last_status = r;
            return shin_exec_line(semi + 1);
        }
    }

    char pool[SHIN_MAX_LINE * 4];
    char *w[SHIN_MAX_WORDS];
    int wc = words_split(ex, w, SHIN_MAX_WORDS, pool, sizeof(pool));
    if (!wc) return 0;
    return shin_exec_words(w, wc);
}

// -- Loop body collection

// Reads lines from the reader until the matching "done", tracking nested
// while/for depth so inner loops are buffered correctly.
static shin_lbuf_t *lbuf_collect(shin_reader_t *r)
{
    shin_lbuf_t *b = (shin_lbuf_t *)malloc(sizeof(shin_lbuf_t));
    if (!b) return NULL;
    b->n = 0;
    char line[SHIN_MAX_LINE];
    int depth = 1;
    while (reader_next_line(r, line, sizeof(line))) {
        const char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (!strncmp(t, "while ", 6) || !strncmp(t, "for ", 4)) depth++;
        else if (!strcmp(t, "done") && --depth == 0) break;
        if (b->n < SHIN_LOOP_LINES)
            strlcpy(b->ln[b->n++], line, SHIN_MAX_LINE);
    }
    return b;
}

static int lbuf_run(shin_lbuf_t *b)
{
    int r = 0;
    for (int i = 0; i < b->n; i++) {
        const char *t = b->ln[i];
        while (*t == ' ' || *t == '\t') t++;
        r = shin_exec_line(t);
        shin_last_status = r;
    }
    return r;
}

// -- Main interpreter loop

static int shin_run_reader(shin_reader_t *r)
{
    char line[SHIN_MAX_LINE];
    int ret = 0;

    while (reader_next_line(r, line, sizeof(line))) {
        const char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (!*t || *t == '#') continue;

        char ex[SHIN_MAX_LINE * 2];
        expand_full(t, ex, sizeof(ex));
        char pool[SHIN_MAX_LINE * 4];
        char *w[SHIN_MAX_WORDS];
        int wc = words_split(ex, w, SHIN_MAX_WORDS, pool, sizeof(pool));
        if (!wc) continue;
        const char *kw = w[0];

        if (!strcmp(kw, "if")) {
            if (shin_ifdepth >= SHIN_MAX_STACK) return 1;
            int ok = 0;
            if (ifstack_is_active()) {
                char *cw[SHIN_MAX_WORDS];
                int cc = 0;
                for (int i = 1; i < wc; i++)
                    if (strcmp(w[i], "then")) cw[cc++] = w[i];
                ok = cond_eval(cw, cc) == 0;
            }
            shin_ifs[shin_ifdepth++] = (shin_ifframe_t){ ok, ok };
            continue;
        }
        if (!strcmp(kw, "elif")) {
            if (!shin_ifdepth) return 1;
            shin_ifframe_t *f = &shin_ifs[shin_ifdepth - 1];
            if (!f->done) {
                char *cw[SHIN_MAX_WORDS];
                int cc = 0;
                for (int i = 1; i < wc; i++)
                    if (strcmp(w[i], "then")) cw[cc++] = w[i];
                int ok = cond_eval(cw, cc) == 0;
                f->active = ok;
                if (ok) f->done = 1;
            } else {
                f->active = 0;
            }
            continue;
        }
        if (!strcmp(kw, "else")) {
            if (shin_ifdepth)
                shin_ifs[shin_ifdepth - 1].active =
                    !shin_ifs[shin_ifdepth - 1].done;
            continue;
        }
        if (!strcmp(kw, "fi")) {
            if (shin_ifdepth) shin_ifdepth--;
            continue;
        }

        if (!strcmp(kw, "while")) {
            if (!ifstack_is_active()) continue;
            // The condition line is re-expanded on each iteration so that
            // variables updated inside the loop body are reflected.
            char cond_line[SHIN_MAX_LINE];
            strlcpy(cond_line, t, sizeof(cond_line));
            shin_lbuf_t *body = lbuf_collect(r);
            if (!body) return 1;
            for (;;) {
                char ex2[SHIN_MAX_LINE * 2];
                expand_full(cond_line, ex2, sizeof(ex2));
                char p2[SHIN_MAX_LINE * 4];
                char *cw[SHIN_MAX_WORDS];
                int cc = words_split(ex2, cw, SHIN_MAX_WORDS, p2, sizeof(p2));
                char *condw[SHIN_MAX_WORDS];
                int condc = 0;
                for (int i = 1; i < cc; i++)
                    if (strcmp(cw[i], "do")) condw[condc++] = cw[i];
                if (cond_eval(condw, condc) != 0) break;
                lbuf_run(body);
            }
            free(body);
            continue;
        }

        if (!strcmp(kw, "for")) {
            if (!ifstack_is_active() || wc < 4) continue;
            char var[64];
            strlcpy(var, w[1], sizeof(var));
            char items[SHIN_MAX_LINE];
            items[0] = 0;
            for (int i = 3; i < wc; i++) {
                if (!strcmp(w[i], "do")) continue;
                if (items[0]) strlcat(items, " ", sizeof(items));
                strlcat(items, w[i], sizeof(items));
            }
            shin_lbuf_t *body = lbuf_collect(r);
            if (!body) return 1;
            char tmp[SHIN_MAX_LINE];
            strlcpy(tmp, items, sizeof(tmp));
            char *tok = strtok(tmp, " \t");
            while (tok) {
                var_set(var, tok);
                lbuf_run(body);
                tok = strtok(NULL, " \t");
            }
            free(body);
            continue;
        }

        if (!strcmp(kw, "done") || !strcmp(kw, "do") || !strcmp(kw, "then"))
            continue;

        if (!ifstack_is_active()) continue;
        ret = shin_exec_line(t);
        shin_last_status = ret;
    }
    return ret;
}

static int shin_run_file(const char *path)
{
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) { printf("shin: %s: not found\n", path); return 1; }
    long fsz = lseek(fd, 0, 2);
    if (fsz <= 0 || fsz > SHIN_MAX_SCRIPT) { close(fd); return 1; }
    lseek(fd, 0, 0);
    char *buf = (char *)malloc((size_t)fsz + 1);
    if (!buf) { close(fd); return 1; }
    ssize_t got = read(fd, buf, (size_t)fsz);
    close(fd);
    if (got <= 0) { free(buf); return 1; }
    buf[got] = 0;
    size_t pos = 0;
    if (got >= 2 && buf[0] == '#' && buf[1] == '!')
        while (pos < (size_t)got && buf[pos] != '\n') pos++;
    shin_reader_t r = { buf, (size_t)got, pos };
    int ret = shin_run_reader(&r);
    free(buf);
    return ret;
}

// -- Entry point

int md_main(long argc, char **argv)
{
    shin_argv = (char **)argv;
    shin_argc = (int)argc;

    if (argc < 2) {
        puts("Usage: shin script.sh [args...]");
        puts("       shin -c \"cmd\"");
        return 1;
    }
    if (!strcmp(argv[1], "-c")) {
        if (argc < 3) { puts("shin: -c requires a command"); return 1; }
        return shin_exec_line(argv[2]);
    }
    shin_argv = (char **)argv + 1;
    shin_argc = (int)argc - 1;
    return shin_run_file(argv[1]);
}