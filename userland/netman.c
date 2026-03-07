// SPDX-License-Identifier: GPL-2.0-only
//
// netman.c - Network Manager for ModuOS
// Implements TCP/IP stack in userspace
//
// Exposes network services via UserFS:
// - $/user/network/arp
// - $/user/network/ip
// - $/user/network/icmp
// - $/user/network/udp
// - $/user/network/tcp
//
// HTTP is NOT implemented here - browsers/apps handle HTTP on top of TCP!

#include "libc.h"

#define NETMAN_VERSION "0.1.0"

// Network interface state
typedef struct {
    char name[16];              // e.g., "eth0"
    uint8_t mac_addr[6];        // MAC address
    uint32_t ip_addr;           // IPv4 address (host byte order)
    uint32_t netmask;           // Netmask
    uint32_t gateway;           // Default gateway
    int link_up;                // Link status
    int nic_fd;                 // File descriptor to NIC device
} netif_t;

// ARP cache entry
typedef struct {
    uint32_t ip_addr;           // IPv4 address
    uint8_t mac_addr[6];        // MAC address
    uint32_t timestamp;         // Last update time
    int valid;                  // Entry is valid
} arp_entry_t;

#define ARP_CACHE_SIZE 256

// TCP connection state
typedef enum {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
} tcp_state_t;

// UDP socket
typedef struct {
    uint32_t local_ip;
    uint16_t local_port;
    int bound;
} udp_socket_t;

// TCP socket
typedef struct {
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    tcp_state_t state;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t *rx_buffer;
    uint32_t rx_len;
    uint8_t *tx_buffer;
    uint32_t tx_len;
} tcp_socket_t;

// Global state
static netif_t g_netif;
static arp_entry_t g_arp_cache[ARP_CACHE_SIZE];
static udp_socket_t g_udp_sockets[16];
static tcp_socket_t g_tcp_sockets[16];

// Forward declarations
static int netman_init(void);
static int netman_detect_nic(void);
static int netman_arp_resolve(uint32_t ip_addr, uint8_t mac_out[6]);
static int netman_send_ip_packet(uint32_t dst_ip, uint8_t protocol, const uint8_t *data, uint16_t len);
static int netman_send_udp(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port, const uint8_t *data, uint16_t len);
static int netman_send_tcp(tcp_socket_t *sock, const uint8_t *data, uint16_t len, uint8_t flags);
static void netman_receive_loop(void);

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("[NetMan] Network Manager v%s starting\n", NETMAN_VERSION);

    // Initialize network manager
    if (netman_init() != 0) {
        printf("[NetMan] Initialization failed\n");
        return 1;
    }

    printf("[NetMan] Network stack initialized\n");
    printf("[NetMan] IP:      %d.%d.%d.%d\n",
           (g_netif.ip_addr >> 24) & 0xFF,
           (g_netif.ip_addr >> 16) & 0xFF,
           (g_netif.ip_addr >> 8) & 0xFF,
           g_netif.ip_addr & 0xFF);
    printf("[NetMan] Netmask: %d.%d.%d.%d\n",
           (g_netif.netmask >> 24) & 0xFF,
           (g_netif.netmask >> 16) & 0xFF,
           (g_netif.netmask >> 8) & 0xFF,
           g_netif.netmask & 0xFF);
    printf("[NetMan] Gateway: %d.%d.%d.%d\n",
           (g_netif.gateway >> 24) & 0xFF,
           (g_netif.gateway >> 16) & 0xFF,
           (g_netif.gateway >> 8) & 0xFF,
           g_netif.gateway & 0xFF);
    printf("[NetMan] MAC:     %02x:%02x:%02x:%02x:%02x:%02x\n",
           g_netif.mac_addr[0], g_netif.mac_addr[1], g_netif.mac_addr[2],
           g_netif.mac_addr[3], g_netif.mac_addr[4], g_netif.mac_addr[5]);

    printf("[NetMan] Registering UserFS network services...\n");
    // TODO: Register $/user/network/* nodes

    printf("[NetMan] Entering receive loop\n");
    netman_receive_loop();

    return 0;
}

static int netman_init(void) {
    // Clear state
    memset(&g_netif, 0, sizeof(g_netif));
    memset(g_arp_cache, 0, sizeof(g_arp_cache));
    memset(g_udp_sockets, 0, sizeof(g_udp_sockets));
    memset(g_tcp_sockets, 0, sizeof(g_tcp_sockets));

    // Detect and open NIC
    if (netman_detect_nic() != 0) {
        return -1;
    }

    // TODO: Get IP config via DHCP or static config
    // For now, use hardcoded config for testing
    g_netif.ip_addr = (10 << 24) | (0 << 16) | (2 << 8) | 15;  // 10.0.2.15
    g_netif.netmask = (255 << 24) | (255 << 16) | (255 << 8) | 0;  // 255.255.255.0
    g_netif.gateway = (10 << 24) | (0 << 16) | (2 << 8) | 2;   // 10.0.2.2

    return 0;
}

static int netman_detect_nic(void) {
    // Try to open E1000 NIC (most common in VMs)
    // TODO: Implement proper NIC detection via syscalls
    
    printf("[NetMan] Detecting network interface...\n");
    
    // For now, assume NIC is already initialized by kernel driver
    // We'll use a syscall to get NIC info
    
    strcpy(g_netif.name, "eth0");
    
    // Hardcoded MAC for testing (should come from NIC driver)
    g_netif.mac_addr[0] = 0x52;
    g_netif.mac_addr[1] = 0x54;
    g_netif.mac_addr[2] = 0x00;
    g_netif.mac_addr[3] = 0x12;
    g_netif.mac_addr[4] = 0x34;
    g_netif.mac_addr[5] = 0x56;
    
    g_netif.link_up = 1;
    
    printf("[NetMan] Found NIC: %s\n", g_netif.name);
    
    return 0;
}

static int netman_arp_resolve(uint32_t ip_addr, uint8_t mac_out[6]) {
    // Check ARP cache first
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_arp_cache[i].valid && g_arp_cache[i].ip_addr == ip_addr) {
            memcpy(mac_out, g_arp_cache[i].mac_addr, 6);
            return 0;
        }
    }

    // TODO: Send ARP request
    // For now, return error
    return -1;
}

static int netman_send_ip_packet(uint32_t dst_ip, uint8_t protocol, const uint8_t *data, uint16_t len) {
    (void)dst_ip;
    (void)protocol;
    (void)data;
    (void)len;
    
    // TODO: Build IP packet
    // TODO: Get destination MAC via ARP
    // TODO: Send Ethernet frame via NIC driver
    
    return 0;
}

static int netman_send_udp(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port, const uint8_t *data, uint16_t len) {
    (void)dst_ip;
    (void)dst_port;
    (void)src_port;
    (void)data;
    (void)len;
    
    // TODO: Build UDP packet
    // TODO: Send via IP layer
    
    return 0;
}

static int netman_send_tcp(tcp_socket_t *sock, const uint8_t *data, uint16_t len, uint8_t flags) {
    (void)sock;
    (void)data;
    (void)len;
    (void)flags;
    
    // TODO: Build TCP packet
    // TODO: Send via IP layer
    
    return 0;
}

static void netman_receive_loop(void) {
    printf("[NetMan] Waiting for network packets...\n");
    
    // TODO: Receive packets from NIC driver
    // TODO: Parse Ethernet frame
    // TODO: Handle ARP/IP/ICMP/UDP/TCP
    
    while (1) {
        // Sleep to avoid busy-wait
        // TODO: Use proper event-driven I/O
    }
}
