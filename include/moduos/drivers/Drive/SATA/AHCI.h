#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

// AHCI PCI Class codes
#define AHCI_CLASS_STORAGE      0x01
#define AHCI_SUBCLASS_SATA      0x06
#define AHCI_PROG_IF_AHCI       0x01

// FIS Types
#define FIS_TYPE_REG_H2D        0x27  // Register FIS - host to device
#define FIS_TYPE_REG_D2H        0x34  // Register FIS - device to host
#define FIS_TYPE_DMA_ACT        0x39  // DMA activate FIS
#define FIS_TYPE_DMA_SETUP      0x41  // DMA setup FIS
#define FIS_TYPE_DATA           0x46  // Data FIS
#define FIS_TYPE_BIST           0x58  // BIST activate FIS
#define FIS_TYPE_PIO_SETUP      0x5F  // PIO setup FIS
#define FIS_TYPE_DEV_BITS       0xA1  // Set device bits FIS

// ATA Commands
#define ATA_CMD_READ_DMA_EX     0x25
#define ATA_CMD_WRITE_DMA_EX    0x35
#define ATA_CMD_IDENTIFY        0xEC

// Port Signature Values
#define SATA_SIG_ATA            0x00000101  // SATA drive
#define SATA_SIG_ATAPI          0xEB140101  // SATAPI drive
#define SATA_SIG_SEMB           0xC33C0101  // Enclosure management bridge
#define SATA_SIG_PM             0x96690101  // Port multiplier

// Port Command Register Bits
#define HBA_PxCMD_ST            0x0001  // Start
#define HBA_PxCMD_SUD           0x0002  // Spin-Up Device
#define HBA_PxCMD_POD           0x0004  // Power On Device
#define HBA_PxCMD_FRE           0x0010  // FIS Receive Enable
#define HBA_PxCMD_FR            0x4000  // FIS Receive Running
#define HBA_PxCMD_CR            0x8000  // Command List Running

// Port Interrupt Status/Enable Bits
#define HBA_PxIS_TFES           (1 << 30)  // Task File Error Status

// Port SATA Status Register
#define HBA_PxSSTS_DET_MASK     0x0F
#define HBA_PxSSTS_DET_PRESENT  0x03

// Host Control Register Bits
#define HBA_GHC_AHCI_ENABLE     (1 << 31)
#define HBA_GHC_RESET           (1 << 0)
#define HBA_GHC_IE              (1 << 1)

// ===========================================================================
// HBA (Host Bus Adapter) Memory Registers
// ===========================================================================

// Generic Host Control Registers
typedef volatile struct {
    uint32_t cap;        // 0x00: Host capability
    uint32_t ghc;        // 0x04: Global host control
    uint32_t is;         // 0x08: Interrupt status
    uint32_t pi;         // 0x0C: Port implemented
    uint32_t vs;         // 0x10: Version
    uint32_t ccc_ctl;    // 0x14: Command completion coalescing control
    uint32_t ccc_pts;    // 0x18: Command completion coalescing ports
    uint32_t em_loc;     // 0x1C: Enclosure management location
    uint32_t em_ctl;     // 0x20: Enclosure management control
    uint32_t cap2;       // 0x24: Host capabilities extended
    uint32_t bohc;       // 0x28: BIOS/OS handoff control and status
} hba_mem_t;

// Port Registers
typedef volatile struct {
    uint32_t clb;        // 0x00: Command list base address, 1K-byte aligned
    uint32_t clbu;       // 0x04: Command list base address upper 32 bits
    uint32_t fb;         // 0x08: FIS base address, 256-byte aligned
    uint32_t fbu;        // 0x0C: FIS base address upper 32 bits
    uint32_t is;         // 0x10: Interrupt status
    uint32_t ie;         // 0x14: Interrupt enable
    uint32_t cmd;        // 0x18: Command and status
    uint32_t rsv0;       // 0x1C: Reserved
    uint32_t tfd;        // 0x20: Task file data
    uint32_t sig;        // 0x24: Signature
    uint32_t ssts;       // 0x28: SATA status (SCR0:SStatus)
    uint32_t sctl;       // 0x2C: SATA control (SCR2:SControl)
    uint32_t serr;       // 0x30: SATA error (SCR1:SError)
    uint32_t sact;       // 0x34: SATA active (SCR3:SActive)
    uint32_t ci;         // 0x38: Command issue
    uint32_t sntf;       // 0x3C: SATA notification (SCR4:SNotification)
    uint32_t fbs;        // 0x40: FIS-based switch control
    uint32_t rsv1[11];   // 0x44 ~ 0x6F: Reserved
    uint32_t vendor[4];  // 0x70 ~ 0x7F: Vendor specific
} hba_port_t;

// ===========================================================================
// Command List and FIS Structures
// ===========================================================================

// Received FIS Structure
typedef struct {
    // DMA Setup FIS
    uint8_t dsfis[0x20];
    uint8_t pad0[4];
    
    // PIO Setup FIS
    uint8_t psfis[0x20];
    uint8_t pad1[12];
    
    // D2H Register FIS
    uint8_t rfis[0x18];
    uint8_t pad2[4];
    
    // Set Device Bits FIS
    uint8_t sdbfis[8];
    
    // Unknown FIS
    uint8_t ufis[0x40];
    
    uint8_t rsv[0x60];
} hba_fis_t;

// Command Header
typedef struct {
    // DW0
    uint8_t cfl:5;       // Command FIS length in DWORDS, 2 ~ 16
    uint8_t a:1;         // ATAPI
    uint8_t w:1;         // Write, 1: H2D, 0: D2H
    uint8_t p:1;         // Prefetchable
    
    uint8_t r:1;         // Reset
    uint8_t b:1;         // BIST
    uint8_t c:1;         // Clear busy upon R_OK
    uint8_t rsv0:1;      // Reserved
    uint8_t pmp:4;       // Port multiplier port
    
    uint16_t prdtl;      // Physical region descriptor table length in entries
    
    // DW1
    volatile uint32_t prdbc;  // Physical region descriptor byte count transferred
    
    // DW2, 3
    uint32_t ctba;       // Command table descriptor base address
    uint32_t ctbau;      // Command table descriptor base address upper 32 bits
    
    // DW4 - 7
    uint32_t rsv1[4];    // Reserved
} hba_cmd_header_t;

// Physical Region Descriptor Table entry
typedef struct {
    uint32_t dba;        // Data base address
    uint32_t dbau;       // Data base address upper 32 bits
    uint32_t rsv0;       // Reserved
    
    uint32_t dbc:22;     // Byte count, 4M max
    uint32_t rsv1:9;     // Reserved
    uint32_t i:1;        // Interrupt on completion
} hba_prdt_entry_t;

// Command Table
typedef struct {
    // Command FIS
    uint8_t cfis[64];
    
    // ATAPI command, 12 or 16 bytes
    uint8_t acmd[16];
    
    // Reserved
    uint8_t rsv[48];
    
    // Physical region descriptor table entries, 0 ~ 65535
    hba_prdt_entry_t prdt_entry[1];  // We'll allocate more as needed
} hba_cmd_table_t;

// Register H2D FIS
typedef struct {
    // DWORD 0
    uint8_t fis_type;    // FIS_TYPE_REG_H2D
    
    uint8_t pmport:4;    // Port multiplier
    uint8_t rsv0:3;      // Reserved
    uint8_t c:1;         // 1: Command, 0: Control
    
    uint8_t command;     // Command register
    uint8_t featurel;    // Feature register, 7:0
    
    // DWORD 1
    uint8_t lba0;        // LBA low register, 7:0
    uint8_t lba1;        // LBA mid register, 15:8
    uint8_t lba2;        // LBA high register, 23:16
    uint8_t device;      // Device register
    
    // DWORD 2
    uint8_t lba3;        // LBA register, 31:24
    uint8_t lba4;        // LBA register, 39:32
    uint8_t lba5;        // LBA register, 47:40
    uint8_t featureh;    // Feature register, 15:8
    
    // DWORD 3
    uint8_t countl;      // Count register, 7:0
    uint8_t counth;      // Count register, 15:8
    uint8_t icc;         // Isochronous command completion
    uint8_t control;     // Control register
    
    // DWORD 4
    uint8_t rsv1[4];     // Reserved
} fis_reg_h2d_t;

// ===========================================================================
// Driver Structures
// ===========================================================================

#define AHCI_MAX_PORTS 32

typedef enum {
    AHCI_DEV_NULL = 0,
    AHCI_DEV_SATA = 1,
    AHCI_DEV_SATAPI = 2,
    AHCI_DEV_SEMB = 3,
    AHCI_DEV_PM = 4
} ahci_device_type_t;

typedef struct {
    hba_port_t *port;
    ahci_device_type_t type;
    uint8_t port_num;
    
    hba_cmd_header_t *cmd_list;     // 1K per port
    hba_fis_t *fis;                 // 256 bytes per port
    hba_cmd_table_t *cmd_tables[32]; // One per command slot
    
    uint64_t sector_count;
    uint16_t sector_size;
    char model[41];
    char serial[21];
} ahci_port_info_t;

typedef struct {
    hba_mem_t *abar;                // HBA Memory registers
    ahci_port_info_t ports[AHCI_MAX_PORTS];
    int port_count;
    int pci_found;
} ahci_controller_t;

// ===========================================================================
// Function Prototypes
// ===========================================================================

// Initialization
int ahci_init(void);
void ahci_shutdown(void);

// Port management
int ahci_port_rebase(hba_port_t *port, int portno);
void ahci_start_cmd(hba_port_t *port);
void ahci_stop_cmd(hba_port_t *port);
int ahci_find_cmdslot(hba_port_t *port);

// Device operations
int ahci_read_sectors(uint8_t port, uint64_t start_lba, uint32_t count, void *buffer);
int ahci_write_sectors(uint8_t port, uint64_t start_lba, uint32_t count, const void *buffer);
int ahci_identify_device(uint8_t port);

// Device detection
ahci_device_type_t ahci_check_type(hba_port_t *port);
int ahci_probe_ports(void);

// Utility
const char* ahci_get_device_type_string(ahci_device_type_t type);
ahci_controller_t* ahci_get_controller(void);

#endif // AHCI_H