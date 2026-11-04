// stwp_client.c -- C port of the Python STWP test client, using sockets.h
#include "sockets.h"
#include "libc/libc.h"

#define PORT      8080
#define END_BYTE  0x03
#define RESP_MAX  8192
#define CHUNK     1024

static uint8_t g_resp[RESP_MAX];

// Connects, sends `command`, and reads into g_resp until the END byte
// arrives, the server closes, or the buffer fills.
// Returns the number of bytes received, or -1 with errno set.
static ssize_t send_request(const char *host, const char *command) {
    ssize_t result = -1;
    size_t total = 0;
    int saved;

    int s = create_connection(host, PORT, 5000);      // like socket() + connect()
    if (s < 0) return -1;

    if (sock_sendall(s, command, strlen(command)) == 0) {
        for (;;) {
            if (total == RESP_MAX) break;             // buffer full: truncate
            size_t room = RESP_MAX - total;
            ssize_t n = sock_recv(s, g_resp + total, room < CHUNK ? room : CHUNK);
            if (n < 0) goto done;                     // error / timeout
            if (n == 0) break;                        // peer closed ("if not chunk")

            int found_end = 0;
            for (ssize_t i = 0; i < n; i++)
                if (g_resp[total + i] == END_BYTE) { found_end = 1; break; }
            total += (size_t)n;
            if (found_end) break;
        }
        result = (ssize_t)total;
    }

done:
    saved = errno;
    sock_close(s);                                    // the `with` block's cleanup
    errno = saved;
    return result;
}

// Prints bytes like Python's repr(b"...")
static void print_repr(const uint8_t *b, size_t n) {
    puts_raw("b'");
    for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c == '\\')      puts_raw("\\\\");
        else if (c == '\'') puts_raw("\\'");
        else if (c == '\n') puts_raw("\\n");
        else if (c == '\r') puts_raw("\\r");
        else if (c == '\t') puts_raw("\\t");
        else if (c >= 32 && c < 127) putchar((char)c);
        else printf("\\x%02x", c);
    }
    puts_raw("'\n");
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    // 127.0.0.1 can't work here: netd has no loopback and sends everything
    // via the gateway. Under passt the gateway address reaches the host.
    sock_netinfo_t ni;
    if (sock_get_netinfo(&ni) != 0) {
        printf("Error: netd is not running (errno=%d)\n", errno);
        return 1;
    }
    char host[16];
    snprintf(host, sizeof(host), "%s", inet_ntoa(ni.gateway));
    printf("Server host: %s:%d\n\n", host, PORT);

    const char *commands[] = {
        "INFO",
        "GET /",
        "STAT /",
        "GET /nonexistent.html",
        "INVALID",
    };

    for (int i = 0; i < (int)(sizeof(commands) / sizeof(commands[0])); i++) {
        printf("--- Sending: %s ---\n", commands[i]);

        ssize_t n = send_request(host, commands[i]);
        if (n < 0) {
            if (errno == ECONNREFUSED)
                puts("Error: Could not connect to the server. Make sure it is running.\n");
            else
                printf("Error: request failed (errno=%d, %s)\n\n", errno, sock_last_message());
            continue;
        }

        printf("Response (bytes): ");
        print_repr(g_resp, (size_t)n);
        printf("Response (decoded): ");
        write(STDOUT_FILENO, g_resp, (size_t)n);
        puts("\n");
    }
    return 0;
}