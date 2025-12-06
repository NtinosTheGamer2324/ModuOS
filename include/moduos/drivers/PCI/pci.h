#ifndef PCI_H
#define PCI_H

#include <stdint.h>

// PCI Configuration Space Registers
#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

// PCI Configuration Space Offsets
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION_ID     0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS           0x0B
#define PCI_CACHE_LINE_SIZE 0x0C
#define PCI_LATENCY_TIMER   0x0D
#define PCI_HEADER_TYPE     0x0E
#define PCI_BIST            0x0F
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_INTERRUPT_LINE  0x3C
#define PCI_INTERRUPT_PIN   0x3D

// PCI Command Register Bits
#define PCI_COMMAND_IO              0x01
#define PCI_COMMAND_MEMORY          0x02
#define PCI_COMMAND_MASTER          0x04
#define PCI_COMMAND_SPECIAL         0x08
#define PCI_COMMAND_INVALIDATE      0x10
#define PCI_COMMAND_VGA_PALETTE     0x20
#define PCI_COMMAND_PARITY          0x40
#define PCI_COMMAND_WAIT            0x80
#define PCI_COMMAND_SERR            0x100
#define PCI_COMMAND_FAST_BACK       0x200
#define PCI_COMMAND_INTX_DISABLE    0x400

// PCI Header Types
#define PCI_HEADER_TYPE_NORMAL      0x00
#define PCI_HEADER_TYPE_BRIDGE      0x01
#define PCI_HEADER_TYPE_CARDBUS     0x02

// PCI Class Codes (common ones)
#define PCI_CLASS_UNCLASSIFIED      0x00
#define PCI_CLASS_STORAGE           0x01
#define PCI_CLASS_NETWORK           0x02
#define PCI_CLASS_DISPLAY           0x03
#define PCI_CLASS_MULTIMEDIA        0x04
#define PCI_CLASS_MEMORY            0x05
#define PCI_CLASS_BRIDGE            0x06
#define PCI_CLASS_COMMUNICATION     0x07
#define PCI_CLASS_SYSTEM            0x08
#define PCI_CLASS_INPUT             0x09
#define PCI_CLASS_DOCKING           0x0A
#define PCI_CLASS_PROCESSOR         0x0B
#define PCI_CLASS_SERIAL_BUS        0x0C

// PCI BAR Types
#define PCI_BAR_IO                  0x01
#define PCI_BAR_MEMORY_32           0x00
#define PCI_BAR_MEMORY_64           0x04
#define PCI_BAR_PREFETCHABLE        0x08

// Maximum PCI devices to track
#define MAX_PCI_DEVICES             256

// PCI Device Structure
typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    
    uint16_t vendor_id;
    uint16_t device_id;
    
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision_id;
    
    uint8_t header_type;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    
    uint32_t bar[6];
    uint32_t bar_size[6];
    uint8_t bar_type[6];  // 0=Memory, 1=I/O
    
    uint16_t command;
    uint16_t status;
} pci_device_t;

// PCI Driver Structure
typedef struct {
    const char *name;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    int (*probe)(pci_device_t *dev);
    void (*remove)(pci_device_t *dev);
} pci_driver_t;

// PCI Functions
void pci_init(void);
uint32_t pci_config_read_dword(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset);
uint16_t pci_config_read_word(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset);
uint8_t pci_config_read_byte(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset);
void pci_config_write_dword(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value);
void pci_config_write_word(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint16_t value);
void pci_config_write_byte(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint8_t value);

// Device enumeration
int pci_scan_bus(void);
pci_device_t* pci_get_device(int index);
int pci_get_device_count(void);
pci_device_t* pci_find_device(uint16_t vendor_id, uint16_t device_id);
pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass);

// BAR operations
uint32_t pci_read_bar(pci_device_t *dev, int bar_num);
void pci_write_bar(pci_device_t *dev, int bar_num, uint32_t value);
int pci_get_bar_size(pci_device_t *dev, int bar_num);
void* pci_map_bar(pci_device_t *dev, int bar_num);

// Device control
void pci_enable_bus_mastering(pci_device_t *dev);
void pci_disable_bus_mastering(pci_device_t *dev);
void pci_enable_memory_space(pci_device_t *dev);
void pci_enable_io_space(pci_device_t *dev);
void pci_set_command(pci_device_t *dev, uint16_t command);

// Driver registration
int pci_register_driver(pci_driver_t *driver);
void pci_unregister_driver(pci_driver_t *driver);

// Utility functions
const char* pci_class_name(uint8_t class_code);
const char* pci_vendor_name(uint16_t vendor_id);
void pci_dump_device(pci_device_t *dev);

#endif // PCI_H