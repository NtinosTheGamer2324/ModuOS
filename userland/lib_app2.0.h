#pragma once
#include "NodGL.h"
#include "libc/libc.h"

#define ERR_ICON_NOT_PRESENT 1
#define ERR_PATH_NOT_DIRECTORY 2
#define ERR_PATH_NOT_VALID 3
#define ERR_NOT_APP 4
#define ERR_FILESYSTEM_ERROR 5

/* 
    Application Format v2.0!
    What is this?
    Basicaly a new app format. like the macos {app}.app folder thingy but for ModuOS
*/

/* INI Helpers */
int ini_get_value(const char *buffer, const char *key, char *out_value, size_t max_len) {
    if (!buffer || !key || !out_value || max_len == 0) return 0;

    const char *p = buffer;
    size_t key_len = strlen(key);

    while (*p != '\0') {
        while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') break;

        if (*p == ';' || *p == '#' || *p == '[') {
            while (*p != '\n' && *p != '\0') p++;
            continue;
        }
        const char *line_start = p;
        const char *eq = NULL;
        const char *line_end = p;

        while (*line_end != '\n' && *line_end != '\0') {
            if (*line_end == '=' && eq == NULL) {
                eq = line_end;
            }
            line_end++;
        }

        if (eq != NULL) {
            const char *k_start = line_start;
            while (*k_start == ' ' || *k_start == '\t') k_start++;
            
            const char *k_end = eq - 1;
            while (k_end > k_start && (*k_end == ' ' || *k_end == '\t' || *k_end == '\r')) {
                k_end--;
            }

            size_t cur_key_len = (k_end >= k_start) ? (size_t)(k_end - k_start + 1) : 0;

            if (cur_key_len == key_len && strncmp(k_start, key, key_len) == 0) {
                const char *v_start = eq + 1;
                while (*v_start == ' ' || *v_start == '\t') v_start++;

                const char *v_end = line_end - 1;
                while (v_end >= v_start && (*v_end == ' ' || *v_end == '\t' || *v_end == '\r')) {
                    v_end--;
                }

                size_t val_len = (v_end >= v_start) ? (size_t)(v_end - v_start + 1) : 0;
                if (val_len >= max_len) {
                    val_len = max_len - 1; 
                }

                memcpy(out_value, v_start, val_len);
                out_value[val_len] = '\0';
                return 1; 
            }
        }

        p = line_end;
    }

    return 0;
}

int ini_get_sizeof_value(const char *buffer, const char *key) {
    if (!buffer || !key) return -1;

    const char *p = buffer;
    size_t key_len = strlen(key);

    while (*p != '\0') {
        while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') break;

        if (*p == ';' || *p == '#' || *p == '[') {
            while (*p != '\n' && *p != '\0') p++;
            continue;
        }

        const char *line_start = p;
        const char *eq = NULL;
        const char *line_end = p;

        while (*line_end != '\n' && *line_end != '\0') {
            if (*line_end == '=' && eq == NULL) {
                eq = line_end;
            }
            line_end++;
        }

        if (eq != NULL) {
            const char *k_start = line_start;
            while (*k_start == ' ' || *k_start == '\t') k_start++;
            
            const char *k_end = eq - 1;
            while (k_end > k_start && (*k_end == ' ' || *k_end == '\t' || *k_end == '\r')) {
                k_end--;
            }

            size_t cur_key_len = (k_end >= k_start) ? (size_t)(k_end - k_start + 1) : 0;

            if (cur_key_len == key_len && strncmp(k_start, key, key_len) == 0) {
                const char *v_start = eq + 1;
                while (*v_start == ' ' || *v_start == '\t') v_start++;

                const char *v_end = line_end - 1;
                while (v_end >= v_start && (*v_end == ' ' || *v_end == '\t' || *v_end == '\r')) {
                    v_end--;
                }

                int val_len = (v_end >= v_start) ? (int)(v_end - v_start + 1) : 0;
                return val_len; 
            }
        }

        p = line_end;
    }

    return -1;
}

/* Files */
char *read_entire_file_to_buffer(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }

    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0) {
        close(fd);
        return NULL;
    }
    lseek(fd, 0, SEEK_SET);

    char *buffer = malloc(size + 1);
    if (!buffer) {
        close(fd);
        return NULL;
    }

    ssize_t bytes_read = read(fd, buffer, size);
    if (bytes_read < 0) {
        free(buffer);
        close(fd);
        return NULL;
    }
    
    buffer[bytes_read] = '\0';

    close(fd);
    return buffer;
}

/* 
    Standard Format:
    MyApp.app
        info.cfg - ALWAYS
        resources - Info.cfg defines.
            whateverman
            myapp.sqr

    info.cfg must haves:
    path_sqr = ./myapp.sqr
    icon = ./myicon.bmp
    or
    icon = ./myicon.ilib:0
*/

int GetSQRFile(const char* path, char SQRPath[512]) {
    char *ini_data = read_entire_file_to_buffer(path);
    if (!ini_data) {
        printf("Failed to open or read file: %s\n", path);
        return 1; 
    }

    int size = ini_get_sizeof_value(ini_data, "path_sqr");
    if (size < 0) {
        printf("Key 'path_sqr' not found.\n");
        free(ini_data);
        return 1;
    }

    char value[size + 1];
    ini_get_value(ini_data, "path_sqr", value, sizeof(value));
    
    free(ini_data);
    const char *src_val = value;
    if (src_val[0] == '.' && src_val[1] == '/') {
        src_val += 2; 
    }

    const char *last_slash = strrchr(path, '/');

    if (last_slash != NULL) {
        size_t dir_len = (size_t)(last_slash - path + 1);
        
        snprintf(SQRPath, 512, "%.*s%s", (int)dir_len, path, src_val);
    } else {
        snprintf(SQRPath, 512, "%s", src_val);
    }

    return 0;
}