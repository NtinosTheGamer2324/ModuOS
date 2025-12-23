#include "libc.h"
#include "string.h"

/*
 * Neofetch (ModuOS)
 * Better layout + more useful info.
 *
 * Flags:
 *   --no-logo      Disable ASCII logo
 *   --no-bar       Disable memory bar
 *   --no-features  Don't print CPU feature flags
 *   --color        Enable ANSI color output (SGR)
 *   --help, -h     Show usage
 */

static int streq(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    return strcmp(a, b) == 0;
}

static const char* safe_str(const char *s) {
    return (s && s[0]) ? s : "";
}

static void usage(const char *argv0) {
    printf("Usage: %s [--no-logo] [--no-bar] [--no-features] [--color]\n", argv0 ? argv0 : "neofetch");
}

static void format_uptime(uint64_t ms, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;

    uint64_t total_sec = ms / 1000;
    uint64_t days = total_sec / 86400;
    uint64_t hours = (total_sec % 86400) / 3600;
    uint64_t mins = (total_sec % 3600) / 60;

    /* Keep format strings simple: our userland snprintf supports ll + u, but not custom %llud style */
    if (days > 0) {
        snprintf(out, out_sz, "%llu d %llu h %llu m", (unsigned long long)days, (unsigned long long)hours, (unsigned long long)mins);
    } else if (hours > 0) {
        snprintf(out, out_sz, "%llu h %llu m", (unsigned long long)hours, (unsigned long long)mins);
    } else {
        snprintf(out, out_sz, "%llu m", (unsigned long long)mins);
    }
}

static void make_bar(char *out, size_t out_sz, uint64_t used, uint64_t total, int width) {
    if (!out || out_sz == 0) return;
    if (width <= 0) width = 10;

    if (out_sz < (size_t)width + 3) {
        out[0] = 0;
        return;
    }

    if (total == 0) {
        size_t pos = 0;
        out[pos++] = '[';
        for (int i = 0; i < width; i++) out[pos++] = '-';
        out[pos++] = ']';
        out[pos] = 0;
        return;
    }

    uint64_t filled = (used * (uint64_t)width) / total;
    if (filled > (uint64_t)width) filled = (uint64_t)width;

    size_t pos = 0;
    out[pos++] = '[';
    for (int i = 0; i < width; i++) {
        out[pos++] = (i < (int)filled) ? '#' : '-';
    }
    out[pos++] = ']';
    out[pos] = 0;
}

static int is_ansi_escape_start(const char *s) {
    return s && ((uint8_t)s[0] == 0x1B) && s[1] == '[';
}

/* Approximate visible length (ignores legacy \\c* / \\b* / \\rr and ANSI SGR). */
static int visible_len(const char *s) {
    if (!s) return 0;

    int n = 0;
    for (size_t i = 0; s[i]; ) {
        /* ANSI: ESC[...m */
        if (is_ansi_escape_start(&s[i])) {
            i += 2;
            while (s[i] && s[i] != 'm') i++;
            if (s[i] == 'm') i++;
            continue;
        }

        /* Legacy VGA codes used by kernel VGA driver: \\cr, \\clb, \\br, \\rr, etc. */
        if (s[i] == '\\' && s[i + 1]) {
            /* reset: \\rr */
            if (s[i + 1] == 'r' && s[i + 2] == 'r') { i += 3; continue; }

            /* 2-char codes: cX or bX */
            if ((s[i + 1] == 'c' || s[i + 1] == 'b') && s[i + 2]) {
                /* 3-char bright codes: clX / blX */
                if (s[i + 2] == 'l' && s[i + 3]) { i += 4; continue; }
                i += 3;
                continue;
            }
        }

        n++;
        i++;
    }

    return n;
}

static int logo_max_width(const char **logo, int logo_lines) {
    int w = 0;
    for (int i = 0; i < logo_lines; i++) {
        int lw = visible_len(logo[i]);
        if (lw > w) w = lw;
    }
    return w;
}

static void print_spaces(int n) {
    for (int i = 0; i < n; i++) putc(' ');
}

static void print_kv_color(int logo_on, const char **logo, int logo_lines, int logo_width, int line,
                           int use_color, const char *key, const char *val) {
    const int pad = 2;

    if (logo_on) {
        if (line < logo_lines) {
            puts_raw(logo[line]);
            int lw = visible_len(logo[line]);
            if (lw < logo_width) print_spaces(logo_width - lw);
        } else {
            /* Keep left column aligned after the logo ends */
            print_spaces(logo_width);
        }
        print_spaces(pad);
    }

    if (key && key[0]) {
        if (use_color) {
            /* bright cyan keys, reset before values */
            puts_raw("\x1b[96m");
            puts_raw(key);
            puts_raw(": ");
            puts_raw("\x1b[0m");
            puts_raw(val ? val : "");
            putc('\n');
        } else {
            printf("%s: %s\n", key, val ? val : "");
        }
    } else {
        if (use_color) {
            puts_raw(val ? val : "");
            putc('\n');
        } else {
            printf("%s\n", val ? val : "");
        }
    }
}

int md_main(long argc, char** argv) {
    int show_logo = 1;
    int show_bar = 1;
    int show_features = 1;
    int use_color = 0;

    for (long i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (streq(a, "--no-logo")) show_logo = 0;
        else if (streq(a, "--no-bar")) show_bar = 0;
        else if (streq(a, "--no-features")) show_features = 0;
        else if (streq(a, "--color")) use_color = 1;
        else if (streq(a, "--help") || streq(a, "-h")) { usage(argv[0]); return 0; }
    }

    md64api_sysinfo_data *info = get_system_info();
    if (!info) {
        puts("Error: Cannot get system info");
        return 1;
    }

    /* ASCII logo (kept simple for VGA text mode) */
    static const char *logo[] = {
        "\\cp      $$\\      $$\\                 $$\\            $$$$$$\\   $$$$$$\\  \\rr",
        "\\cp      $$$\\    $$$ |                $$ |          $$  __$$\\ $$  __$$\\ \\rr",
        "\\cp      $$$$\\  $$$$ | $$$$$$\\   $$$$$$$ |$$\\   $$\\ $$ /  $$ |$$ /  \\__|\\rr",
        "\\cp      $$\\$$\\$$ $$ |$$  __$$\\ $$  __$$ |$$ |  $$ |$$ |  $$ |\\$$$$$$\\  \\rr",
        "\\cp      $$ \\$$$  $$ |$$ /  $$ |$$ /  $$ |$$ |  $$ |$$ |  $$ | \\____$$\\ \\rr",
        "\\cp      $$ |\\$  /$$ |$$ |  $$ |$$ |  $$ |$$ |  $$ |$$ |  $$ |$$\\   $$ |\\rr",
        "\\cp      $$ | \\_/ $$ |\\$$$$$$  |\\$$$$$$$ |\\$$$$$$  | $$$$$$  |\\$$$$$$  |\\rr",
        "\\cp      \\__|     \\__| \\______/  \\_______| \\______/  \\______/  \\______/ \\rr"
    };
    const int logo_lines = (int)(sizeof(logo) / sizeof(logo[0]));
    const int logo_width = logo_max_width(logo, logo_lines);

    char header[128];
    snprintf(header, sizeof(header), "%s@%s", safe_str(info->username), safe_str(info->pcname));

    char uptime_buf[64];
    format_uptime(time_ms(), uptime_buf, sizeof(uptime_buf));

    uint64_t mem_total = info->sys_total_ram;
    uint64_t mem_avail = info->sys_available_ram;
    uint64_t mem_used = (mem_total > mem_avail) ? (mem_total - mem_avail) : 0;

    char mem_line[96];
    mem_line[0] = 0;
    if (mem_total > 0) {
        if (show_bar) {
            char bar[32];
            /* Default bar width. May shrink later once we know layout. */
            make_bar(bar, sizeof(bar), mem_used, mem_total, 12);
            snprintf(mem_line, sizeof(mem_line), "%llu/%llu %s", mem_used, mem_total, bar);
        } else {
            snprintf(mem_line, sizeof(mem_line), "%llu/%llu", mem_used, mem_total);
        }
    }

    const char *cpu = (safe_str(info->cpu_model)[0]) ? info->cpu_model : info->cpu;

    char bios_line[128];
    bios_line[0] = 0;
    if (safe_str(info->bios_vendor)[0]) {
        if (safe_str(info->bios_version)[0])
            snprintf(bios_line, sizeof(bios_line), "%s %s", info->bios_vendor, info->bios_version);
        else
            snprintf(bios_line, sizeof(bios_line), "%s", info->bios_vendor);
    }

    char vm_line[64];
    vm_line[0] = 0;
    if (info->is_virtual_machine) {
        snprintf(vm_line, sizeof(vm_line), "Yes (%s)", safe_str(info->virtualization_vendor)[0] ? info->virtualization_vendor : "Unknown");
    }

    /* If logo is too wide for 80-column VGA, fall back to stacked layout */
    const int console_cols = 80;
    const int approx_right_cols = 30; /* enough for "KEY: value" */
    const int pad = 2;
    const int stacked = (show_logo && (logo_width + pad + approx_right_cols > console_cols));

    /* If we're side-by-side with a logo, shrink memory bar more to avoid wrapping */
    if (mem_line[0] && show_bar && !stacked && show_logo) {
        char bar[32];
        make_bar(bar, sizeof(bar), mem_used, mem_total, 8);
        snprintf(mem_line, sizeof(mem_line), "%llu/%llu %s", mem_used, mem_total, bar);
    }

    int line = 0;

    if (stacked && show_logo) {
        for (int i = 0; i < logo_lines; i++) {
            puts_raw(logo[i]);
            putc('\n');
        }
        puts_raw("\n");
    }

    int logo_on = show_logo && !stacked;

    if (use_color) {
        puts_raw("\x1b[95m"); /* bright magenta header */
        print_kv_color(logo_on, logo, logo_lines, logo_width, line++, use_color, NULL, header);
        puts_raw("\x1b[0m");
    } else {
        print_kv_color(logo_on, logo, logo_lines, logo_width, line++, use_color, NULL, header);
    }
    print_kv_color(logo_on, logo, logo_lines, logo_width, line++, use_color, NULL, "------------------------------");

    {
        char os_line[96];
        snprintf(os_line, sizeof(os_line), "%s %s", safe_str(info->os_name), safe_str(info->os_arch));
        print_kv_color(logo_on, logo, logo_lines, logo_width, line++, use_color, "OS", os_line);
    }

    print_kv_color(logo_on, logo, logo_lines, logo_width, line++, use_color, "Kernel", safe_str(info->KernelVendor));
    print_kv_color(logo_on, logo, logo_lines, logo_width, line++, use_color, "Uptime", uptime_buf);

    if (safe_str(info->kconsole)[0]) print_kv_color(logo_on, logo, logo_lines, logo_width, line++, use_color, "Console", info->kconsole);
    if (safe_str(cpu)[0]) print_kv_color(logo_on, logo, logo_lines, logo_width, line++, use_color, "CPU", cpu);

    if (show_features && safe_str(info->cpu_flags)[0]) {
        print_kv_color(logo_on, logo, logo_lines, logo_width, line++, use_color, "CPU Features", info->cpu_flags);
    }

    if (safe_str(info->gpu_name)[0]) print_kv_color(logo_on, logo, logo_lines, logo_width, line++, use_color, "GPU", info->gpu_name);
    if (safe_str(info->primary_disk_model)[0]) print_kv_color(logo_on, logo, logo_lines, logo_width, line++, use_color, "Disk", info->primary_disk_model);
    if (mem_line[0]) print_kv_color(logo_on, logo, logo_lines, logo_width, line++, use_color, "Memory", mem_line);

    if (safe_str(info->motherboard_model)[0]) print_kv_color(logo_on, logo, logo_lines, logo_width, line++, use_color, "Board", info->motherboard_model);
    if (bios_line[0]) print_kv_color(logo_on, logo, logo_lines, logo_width, line++, use_color, "BIOS", bios_line);
    if (vm_line[0]) print_kv_color(logo_on, logo, logo_lines, logo_width, line++, use_color, "VM", vm_line);

    if (info->tpm_version > 0) {
        print_kv_color(logo_on, logo, logo_lines, logo_width, line++, use_color, "TPM", (info->tpm_version == 2) ? "2.0" : "1.2");
    }

    if (info->secure_boot_enabled) {
        print_kv_color(logo_on, logo, logo_lines, logo_width, line++, use_color, "Secure Boot", "Enabled");
    }

    /* Ensure the logo isn't cut off */
    while (logo_on && line < logo_lines) {
        print_kv_color(logo_on, logo, logo_lines, logo_width, line++, use_color, NULL, "");
    }

    if (use_color) {
        puts_raw(ANSI_RESET);
    }

    return 0;
}
