// automan.c - Autostart Manager (userland version)
// Reads /ModuOS/System64/auto/autostart and executes programs

#include "libc.h"

#define AUTOSTART_PATH "/ModuOS/System64/auto/autostart"
#define MAX_LINE 256

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void strip_comments_inplace(char *line) {
    if (!line) return;
    char *in = line;
    char *out = line;
    
    while (*in) {
        // Check for line comment - only if at start or after whitespace
        if (in[0] == '/' && in[1] == '/') {
            if (in == line || is_space(in[-1])) {
                break; // line comment
            }
        }
        *out++ = *in++;
    }
    *out = 0;
}

static void trim(char *str) {
    if (!str) return;
    
    // Trim leading spaces
    char *start = str;
    while (*start && is_space(*start)) start++;
    
    // Trim trailing spaces
    char *end = start + strlen(start) - 1;
    while (end > start && is_space(*end)) end--;
    *(end + 1) = 0;
    
    // Move trimmed string to beginning
    if (start != str) {
        char *dst = str;
        while (*start) *dst++ = *start++;
        *dst = 0;
    }
}

static int parse_sleep_ms(const char *arg) {
    int val = 0;
    int i = 0;
    while (arg[i] >= '0' && arg[i] <= '9') {
        val = val * 10 + (arg[i] - '0');
        i++;
    }
    
    // Check suffix
    if (arg[i] == 's' || arg[i] == 'S') {
        val *= 1000; // seconds to milliseconds
    }
    // else assume milliseconds
    
    return val;
}

static void execute_autostart_line(const char *line) {
    if (!line || !*line) return;
    
    char buf[MAX_LINE];
    int i;
    for (i = 0; i < MAX_LINE - 1 && line[i]; i++) {
        buf[i] = line[i];
    }
    buf[i] = 0;
    
    trim(buf);
    if (!buf[0]) return;
    
    // Parse command
    char *cmd = buf;
    char *arg = NULL;
    
    // Find first space
    for (i = 0; buf[i] && !is_space(buf[i]); i++);
    if (buf[i]) {
        buf[i] = 0;
        arg = &buf[i + 1];
        trim(arg);
    }
    
    // Handle commands
    if (strcmp(cmd, "bg") == 0) {
        if (!arg || !*arg) {
            printf("[AUTO] 'bg' requires a program path\n");
            return;
        }
        printf("[AUTO] start: %s\n", arg);
        
        // Fork and exec in background
        int pid = fork();
        if (pid == 0) {
            // Child process - exec the program
            char *argv[] = { (char*)arg, NULL };
            char *envp[] = { NULL };
            execve(arg, argv, envp);
            printf("[AUTO] Failed to exec: %s\n", arg);
            exit(1);
        } else if (pid > 0) {
            // Parent - continue
            printf("[AUTO] spawned %s (PID %d)\n", arg, pid);
        } else {
            printf("[AUTO] fork failed for: %s\n", arg);
        }
    }
    else if (strcmp(cmd, "sleep") == 0) {
        if (!arg || !*arg) {
            printf("[AUTO] 'sleep' requires a duration\n");
            return;
        }
        int ms = parse_sleep_ms(arg);
        printf("[AUTO] sleep %d ms\n", ms);
        
        // Use proper sleep syscall (sleep takes seconds, so convert)
        unsigned int seconds = (ms + 999) / 1000;
        sleep(seconds);
    }
    else {
        printf("[AUTO] Unknown command: %s\n", cmd);
    }
}

int md_main(long argc, char **argv) {
    printf("[AUTOMAN] Autostart Manager started\n");
    
    // Open autostart file
    int fd = open(AUTOSTART_PATH, 0, O_RDONLY);
    if (fd < 0) {
        printf("[AUTOMAN] No autostart file at: %s\n", AUTOSTART_PATH);
        return 0;
    }
    
    // Read file content
    char buffer[4096];
    int bytes_read = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    
    if (bytes_read <= 0) {
        printf("[AUTOMAN] Empty or unreadable autostart file\n");
        return 0;
    }
    
    buffer[bytes_read] = 0;
    
    printf("[AUTOMAN] Processing autostart entries\n");
    
    // Process line by line
    char *line_start = buffer;
    for (int i = 0; i <= bytes_read; i++) {
        if (buffer[i] == '\n' || buffer[i] == 0) {
            char old_char = buffer[i];
            buffer[i] = 0;
            
            strip_comments_inplace(line_start);
            trim(line_start);
            
            if (*line_start) {
                execute_autostart_line(line_start);
            }
            
            if (old_char == 0) break;
            line_start = &buffer[i + 1];
        }
    }
    
    printf("[AUTOMAN] Autostart complete\n");
    return 0;
}
