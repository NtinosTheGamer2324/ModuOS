#include "libc.h"
#include "string.h"

// zenith5 - userland shell (Linux-like semantics)

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static char *strdup_local(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = (char*)malloc(n);
    if (!d) return NULL;
    memcpy(d, s, n);
    return d;
}

static int g_last_status = 0;

static size_t append_str(char *dst, size_t di, size_t cap, const char *s) {
    if (!dst || cap == 0) return di;
    if (!s) s = "";
    while (*s && di + 1 < cap) dst[di++] = *s++;
    dst[di] = 0;
    return di;
}

static size_t append_int(char *dst, size_t di, size_t cap, int v) {
    char tmp[16];
    itoa(v, tmp, 10);
    return append_str(dst, di, cap, tmp);
}

static int is_var_start(char c) {
    return (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}
static int is_var_char(char c) {
    return is_var_start(c) || (c >= '0' && c <= '9');
}

// Parse a line into argv[] with:
// - single quotes (no escapes, no expansion)
// - double quotes (supports escapes and $ expansion)
// - backslash escaping in unquoted/double-quoted contexts
// - $VAR and $? expansion
// Returns argc, argv is NULL-terminated.
static int parse_line(const char *in, char ***out_argv) {
    if (!out_argv) return -1;
    *out_argv = NULL;

    const int MAX = 64;
    char **argv = (char**)malloc((MAX + 1) * sizeof(char*));
    if (!argv) return -1;
    memset(argv, 0, (MAX + 1) * sizeof(char*));

    int argc = 0;
    const char *p = in;

    while (p && *p) {
        while (*p && is_space(*p)) p++;
        if (!*p) break;
        if (argc >= MAX) break;

        char buf[512];
        size_t bi = 0;
        buf[0] = 0;

        int in_dq = 0;
        int in_sq = 0;

        while (*p) {
            char c = *p;
            if (!in_dq && !in_sq && is_space(c)) break;

            if (!in_dq && c == '\'' ) { in_sq = !in_sq; p++; continue; }
            if (!in_sq && c == '"') { in_dq = !in_dq; p++; continue; }

            if (!in_sq && c == '\\') {
                p++;
                if (!*p) break;
                c = *p;
                if (c == 'n') c = '\n';
                else if (c == 't') c = '\t';
                // otherwise literal
                if (bi + 1 < sizeof(buf)) buf[bi++] = c;
                buf[bi] = 0;
                p++;
                continue;
            }

            if (!in_sq && c == '$') {
                p++;
                if (*p == '?') {
                    bi = append_int(buf, bi, sizeof(buf), g_last_status);
                    p++;
                    continue;
                }

                if (!is_var_start(*p)) {
                    // literal '$'
                    if (bi + 1 < sizeof(buf)) buf[bi++] = '$';
                    buf[bi] = 0;
                    continue;
                }

                char name[64];
                size_t ni = 0;
                while (*p && is_var_char(*p) && ni + 1 < sizeof(name)) {
                    name[ni++] = *p++;
                }
                name[ni] = 0;

                const char *val = getenv(name);
                if (val) bi = append_str(buf, bi, sizeof(buf), val);
                continue;
            }

            if (bi + 1 < sizeof(buf)) buf[bi++] = c;
            buf[bi] = 0;
            p++;
        }

        argv[argc++] = strdup_local(buf);
        if (*p) p++; // consume space
    }

    argv[argc] = NULL;
    *out_argv = argv;
    return argc;
}

static void free_argv(char **argv) {
    if (!argv) return;
    for (int i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}

static int builtin_cd(int argc, char **argv) {
    const char *p = (argc >= 2) ? argv[1] : "/";
    if (chdir(p) != 0) {
        printf("cd: failed\n");
        return 1;
    }
    return 0;
}

static int builtin_pwd(void) {
    char buf[256];
    if (!getcwd(buf, sizeof(buf))) {
        printf("pwd: failed\n");
        return 1;
    }
    printf("%s\n", buf);
    return 0;
}

static int starts_with(const char *s, const char *pfx) {
    size_t n = strlen(pfx);
    return strncmp(s, pfx, n) == 0;
}

static int builtin_export(int argc, char **argv) {
    int print_as_export = 0;

    if (argc >= 2 && strcmp(argv[1], "-p") == 0) {
        print_as_export = 1;
        argc--;
        argv++;
    }

    if (argc < 2) {
        size_t off = 0;
        char buf[512];

        if (print_as_export) {
            // Print as: export KEY=VALUE
            // envlist2 streams as newline-separated KEY=VALUE\n
            int bol = 1;
            for (;;) {
                int n = envlist2(&off, buf, sizeof(buf));
                if (n < 0) {
                    printf("export: envlist2 failed (errno=%d)\n", errno);
                    return 1;
                }
                if (n == 0) break;

                for (int i = 0; buf[i]; i++) {
                    if (bol) {
                        printf("export ");
                        bol = 0;
                    }
                    char c = buf[i];
                    printf("%c", c);
                    if (c == '\n') bol = 1;
                }
            }
            if (!bol) printf("\n");
            return 0;
        }

        // Default: print raw env listing.
        for (;;) {
            int n = envlist2(&off, buf, sizeof(buf));
            if (n < 0) {
                printf("export: envlist2 failed (errno=%d)\n", errno);
                return 1;
            }
            if (n == 0) break;
            printf("%s", buf);
        }
        return 0;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            if (putenv(argv[i]) != 0) {
                printf("export: failed (errno=%d)\n", errno);
                return 1;
            }
            continue;
        }

        // export KEY (no '=') => ensure it's present.
        const char *v = getenv(argv[i]);
        if (!v) {
            printf("export: %s: not set\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}

int md_main(long argc0, char **argv0) {
    (void)argc0; (void)argv0;

    // Kernel-managed process environment
    (void)putenv("PATH=/Apps");
    (void)putenv("SHELL=ZENITH5");

    char host[64] = "pc";
    char user[64] = "user";

    for (;;) {
        char cwd[256];
        if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");

        printf("%s@%s %s> ", user, host, cwd);
        char *line = input();
        if (!line) continue;

        char **argv = NULL;
        int argc = parse_line(line, &argv);
        if (argc <= 0) { free_argv(argv); continue; }

        if (strcmp(argv[0], "exit") == 0) {
            free_argv(argv);
            return 0;
        }
        if (strcmp(argv[0], "cd") == 0) {
            g_last_status = builtin_cd(argc, argv);
            free_argv(argv);
            continue;
        }
        if (strcmp(argv[0], "pwd") == 0) {
            g_last_status = builtin_pwd();
            free_argv(argv);
            continue;
        }
        if (strcmp(argv[0], "export") == 0) {
            g_last_status = builtin_export(argc, argv);
            free_argv(argv);
            continue;
        }
        if (strcmp(argv[0], "unset") == 0) {
            int verbose = 0;
            int rc = 0;
            int start = 1;
            if (argc >= 2 && strcmp(argv[1], "-v") == 0) {
                verbose = 1;
                start = 2;
            }
            if (start >= argc) {
                printf("unset: missing operand\n");
                g_last_status = 1;
                free_argv(argv);
                continue;
            }

            for (int i = start; i < argc; i++) {
                if (unsetenv(argv[i]) != 0) {
                    rc = 1;
                    if (verbose) printf("unset: %s: failed (errno=%d)\n", argv[i], errno);
                } else {
                    if (verbose) printf("unset: %s\n", argv[i]);
                }
            }
            g_last_status = rc;
            free_argv(argv);
            continue;
        }

        int pid = fork();
        if (pid < 0) {
            printf("fork failed (errno=%d)\n", errno);
            g_last_status = 1;
            free_argv(argv);
            continue;
        }

        if (pid == 0) {
            // child
            // envp==NULL => inherit current process env (kernel-managed)
            execve(argv[0], argv, NULL);
            printf("execve failed (errno=%d)\n", errno);
            exit(127);
        }

        // parent
        int st = 0;
        if (waitpid(pid, &st, 0) < 0) {
            g_last_status = 1;
        } else {
            // Kernel encodes exit status in high byte (see process_exit).
            g_last_status = (st >> 8) & 0xFF;
        }
        free_argv(argv);
    }

}
