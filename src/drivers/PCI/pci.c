#include "moduos/drivers/PCI/pci.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/io/io.h"
#include "moduos/kernel/interrupts/irq.h"
#include "moduos/kernel/interrupts/idt.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/COM/com.h"

// Device storage
static pci_device_t pci_devices[MAX_PCI_DEVICES];
static int pci_device_count = 0;

// Driver storage
#define MAX_PCI_DRIVERS 32
static pci_driver_t* pci_drivers[MAX_PCI_DRIVERS];
static int pci_driver_count = 0;

// ============================================================================
// PCI Configuration Space Access
// ============================================================================

uint32_t pci_config_read_dword(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)(
        (1 << 31) |
        ((uint32_t)bus << 16) |
        ((uint32_t)(device & 0x1F) << 11) |
        ((uint32_t)(func & 0x07) << 8) |
        (offset & 0xFC)
    );
    
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read_word(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    uint32_t dword = pci_config_read_dword(bus, device, func, offset);
    return (uint16_t)((dword >> ((offset & 2) * 8)) & 0xFFFF);
}

uint8_t pci_config_read_byte(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    uint32_t dword = pci_config_read_dword(bus, device, func, offset);
    return (uint8_t)((dword >> ((offset & 3) * 8)) & 0xFF);
}

void pci_config_write_dword(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = (uint32_t)(
        (1 << 31) |
        ((uint32_t)bus << 16) |
        ((uint32_t)(device & 0x1F) << 11) |
        ((uint32_t)(func & 0x07) << 8) |
        (offset & 0xFC)
    );
    
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

void pci_config_write_word(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint16_t value) {
    uint32_t dword = pci_config_read_dword(bus, device, func, offset);
    uint8_t shift = (offset & 2) * 8;
    dword = (dword & ~(0xFFFF << shift)) | ((uint32_t)value << shift);
    pci_config_write_dword(bus, device, func, offset, dword);
}

void pci_config_write_byte(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint8_t value) {
    uint32_t dword = pci_config_read_dword(bus, device, func, offset);
    uint8_t shift = (offset & 3) * 8;
    dword = (dword & ~(0xFF << shift)) | ((uint32_t)value << shift);
    pci_config_write_dword(bus, device, func, offset, dword);
}

// ============================================================================
// Device Enumeration
// ============================================================================

static void pci_probe_bars(pci_device_t *dev) {
    for (int i = 0; i < 6; i++) {
        uint8_t bar_offset = PCI_BAR0 + (i * 4);
        uint32_t bar = pci_config_read_dword(dev->bus, dev->device, dev->function, bar_offset);
        
        if (bar == 0) {
            dev->bar[i] = 0;
            dev->bar_size[i] = 0;
            dev->bar_type[i] = 0;
            continue;
        }
        
        dev->bar[i] = bar;
        
        // Determine BAR type
        if (bar & PCI_BAR_IO) {
            dev->bar_type[i] = 1; // I/O space
            dev->bar[i] &= 0xFFFFFFFC;
        } else {
            dev->bar_type[i] = 0; // Memory space
            dev->bar[i] &= 0xFFFFFFF0;
        }
        
        // Get BAR size
        pci_config_write_dword(dev->bus, dev->device, dev->function, bar_offset, 0xFFFFFFFF);
        uint32_t size = pci_config_read_dword(dev->bus, dev->device, dev->function, bar_offset);
        pci_config_write_dword(dev->bus, dev->device, dev->function, bar_offset, bar);
        
        if (dev->bar_type[i] == 1) {
            size &= 0xFFFFFFFC;
        } else {
            size &= 0xFFFFFFF0;
        }
        
        dev->bar_size[i] = ~size + 1;
        
        // Handle 64-bit BARs
        if ((bar & 0x06) == 0x04) {
            i++; // Skip next BAR as it's part of 64-bit address
        }
    }
}

static void pci_check_device(uint8_t bus, uint8_t device) {
    uint8_t function = 0;
    uint16_t vendor_id = pci_config_read_word(bus, device, function, PCI_VENDOR_ID);
    
    if (vendor_id == 0xFFFF) {
        return; // No device
    }
    
    // Check function 0
    uint8_t header_type = pci_config_read_byte(bus, device, function, PCI_HEADER_TYPE);
    
    if (pci_device_count >= MAX_PCI_DEVICES) {
        COM_LOG_WARN(COM1_PORT, "PCI: Too many devices, skipping some");
        return;
    }
    
    pci_device_t *dev = &pci_devices[pci_device_count++];
    dev->bus = bus;
    dev->device = device;
    dev->function = function;
    dev->vendor_id = vendor_id;
    dev->device_id = pci_config_read_word(bus, device, function, PCI_DEVICE_ID);
    dev->class_code = pci_config_read_byte(bus, device, function, PCI_CLASS);
    dev->subclass = pci_config_read_byte(bus, device, function, PCI_SUBCLASS);
    dev->prog_if = pci_config_read_byte(bus, device, function, PCI_PROG_IF);
    dev->revision_id = pci_config_read_byte(bus, device, function, PCI_REVISION_ID);
    dev->header_type = header_type & 0x7F;
    dev->interrupt_line = pci_config_read_byte(bus, device, function, PCI_INTERRUPT_LINE);
    dev->interrupt_pin = pci_config_read_byte(bus, device, function, PCI_INTERRUPT_PIN);
    dev->command = pci_config_read_word(bus, device, function, PCI_COMMAND);
    dev->status = pci_config_read_word(bus, device, function, PCI_STATUS);
    
    pci_probe_bars(dev);
    
    // Check for multifunction device
    if (header_type & 0x80) {
        for (function = 1; function < 8; function++) {
            vendor_id = pci_config_read_word(bus, device, function, PCI_VENDOR_ID);
            if (vendor_id == 0xFFFF) {
                continue;
            }
            
            if (pci_device_count >= MAX_PCI_DEVICES) {
                COM_LOG_WARN(COM1_PORT, "PCI: Too many devices, skipping some");
                return;
            }
            
            dev = &pci_devices[pci_device_count++];
            dev->bus = bus;
            dev->device = device;
            dev->function = function;
            dev->vendor_id = vendor_id;
            dev->device_id = pci_config_read_word(bus, device, function, PCI_DEVICE_ID);
            dev->class_code = pci_config_read_byte(bus, device, function, PCI_CLASS);
            dev->subclass = pci_config_read_byte(bus, device, function, PCI_SUBCLASS);
            dev->prog_if = pci_config_read_byte(bus, device, function, PCI_PROG_IF);
            dev->revision_id = pci_config_read_byte(bus, device, function, PCI_REVISION_ID);
            dev->header_type = pci_config_read_byte(bus, device, function, PCI_HEADER_TYPE) & 0x7F;
            dev->interrupt_line = pci_config_read_byte(bus, device, function, PCI_INTERRUPT_LINE);
            dev->interrupt_pin = pci_config_read_byte(bus, device, function, PCI_INTERRUPT_PIN);
            dev->command = pci_config_read_word(bus, device, function, PCI_COMMAND);
            dev->status = pci_config_read_word(bus, device, function, PCI_STATUS);
            
            pci_probe_bars(dev);
        }
    }
}

int pci_scan_bus(void) {
    pci_device_count = 0;
    
    COM_LOG_INFO(COM1_PORT, "Scanning PCI bus...");
    
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            pci_check_device(bus, device);
        }
    }
    
    com_write_string(COM1_PORT, "[PCI] Found ");
    char buf[16];
    int i = 0;
    int count = pci_device_count;
    if (count == 0) {
        buf[i++] = '0';
    } else {
        char temp[16];
        int j = 0;
        while (count > 0) {
            temp[j++] = '0' + (count % 10);
            count /= 10;
        }
        while (j > 0) {
            buf[i++] = temp[--j];
        }
    }
    buf[i] = '\0';
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " devices\n");
    
    return pci_device_count;
}

pci_device_t* pci_get_device(int index) {
    if (index < 0 || index >= pci_device_count) {
        return NULL;
    }
    return &pci_devices[index];
}

int pci_get_device_count(void) {
    return pci_device_count;
}

pci_device_t* pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == vendor_id && pci_devices[i].device_id == device_id) {
            return &pci_devices[i];
        }
    }
    return NULL;
}

pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass) {
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].class_code == class_code && 
            (subclass == 0xFF || pci_devices[i].subclass == subclass)) {
            return &pci_devices[i];
        }
    }
    return NULL;
}

// ============================================================================
// BAR Operations
// ============================================================================

uint32_t pci_read_bar(pci_device_t *dev, int bar_num) {
    if (bar_num < 0 || bar_num >= 6) {
        return 0;
    }
    return dev->bar[bar_num];
}

void pci_write_bar(pci_device_t *dev, int bar_num, uint32_t value) {
    if (bar_num < 0 || bar_num >= 6) {
        return;
    }
    
    uint8_t offset = PCI_BAR0 + (bar_num * 4);
    pci_config_write_dword(dev->bus, dev->device, dev->function, offset, value);
    dev->bar[bar_num] = value;
}

int pci_get_bar_size(pci_device_t *dev, int bar_num) {
    if (bar_num < 0 || bar_num >= 6) {
        return 0;
    }
    return dev->bar_size[bar_num];
}

void* pci_map_bar(pci_device_t *dev, int bar_num) {
    if (bar_num < 0 || bar_num >= 6) {
        return NULL;
    }
    
    if (dev->bar_type[bar_num] == 1) {
        // I/O BAR, return as-is (no mapping needed)
        return (void*)(uintptr_t)dev->bar[bar_num];
    }
    
    // Memory BAR - would need virtual memory mapping
    // For now, return physical address (identity mapped in your kernel)
    return (void*)(uintptr_t)dev->bar[bar_num];
}

// ============================================================================
// Device Control
// ============================================================================

void pci_enable_bus_mastering(pci_device_t *dev) {
    uint16_t command = pci_config_read_word(dev->bus, dev->device, dev->function, PCI_COMMAND);
    command |= PCI_COMMAND_MASTER;
    pci_config_write_word(dev->bus, dev->device, dev->function, PCI_COMMAND, command);
    dev->command = command;
}

void pci_disable_bus_mastering(pci_device_t *dev) {
    uint16_t command = pci_config_read_word(dev->bus, dev->device, dev->function, PCI_COMMAND);
    command &= ~PCI_COMMAND_MASTER;
    pci_config_write_word(dev->bus, dev->device, dev->function, PCI_COMMAND, command);
    dev->command = command;
}

void pci_enable_memory_space(pci_device_t *dev) {
    uint16_t command = pci_config_read_word(dev->bus, dev->device, dev->function, PCI_COMMAND);
    command |= PCI_COMMAND_MEMORY;
    pci_config_write_word(dev->bus, dev->device, dev->function, PCI_COMMAND, command);
    dev->command = command;
}

void pci_enable_io_space(pci_device_t *dev) {
    uint16_t command = pci_config_read_word(dev->bus, dev->device, dev->function, PCI_COMMAND);
    command |= PCI_COMMAND_IO;
    pci_config_write_word(dev->bus, dev->device, dev->function, PCI_COMMAND, command);
    dev->command = command;
}

void pci_set_command(pci_device_t *dev, uint16_t command) {
    pci_config_write_word(dev->bus, dev->device, dev->function, PCI_COMMAND, command);
    dev->command = command;
}

// ============================================================================
// Driver Registration
// ============================================================================

static void pci_match_driver(pci_device_t *dev) {
    for (int i = 0; i < pci_driver_count; i++) {
        pci_driver_t *drv = pci_drivers[i];
        
        int match = 0;
        
        // Check vendor/device ID match
        if (drv->vendor_id != 0xFFFF && drv->device_id != 0xFFFF) {
            if (dev->vendor_id == drv->vendor_id && dev->device_id == drv->device_id) {
                match = 1;
            }
        }
        
        // Check class match
        if (drv->class_code != 0xFF) {
            if (dev->class_code == drv->class_code && 
                (drv->subclass == 0xFF || dev->subclass == drv->subclass)) {
                match = 1;
            }
        }
        
        if (match && drv->probe) {
            com_write_string(COM1_PORT, "[PCI] Probing driver: ");
            com_write_string(COM1_PORT, drv->name);
            com_write_string(COM1_PORT, "\n");
            
            if (drv->probe(dev) == 0) {
                COM_LOG_OK(COM1_PORT, "Driver loaded successfully");
            }
        }
    }
}

int pci_register_driver(pci_driver_t *driver) {
    if (pci_driver_count >= MAX_PCI_DRIVERS) {
        COM_LOG_ERROR(COM1_PORT, "Too many PCI drivers");
        return -1;
    }
    
    pci_drivers[pci_driver_count++] = driver;
    
    // Try to match with existing devices
    for (int i = 0; i < pci_device_count; i++) {
        pci_match_driver(&pci_devices[i]);
    }
    
    return 0;
}

void pci_unregister_driver(pci_driver_t *driver) {
    for (int i = 0; i < pci_driver_count; i++) {
        if (pci_drivers[i] == driver) {
            // Remove driver
            for (int j = i; j < pci_driver_count - 1; j++) {
                pci_drivers[j] = pci_drivers[j + 1];
            }
            pci_driver_count--;
            break;
        }
    }
}

// ============================================================================
// Utility Functions
// ============================================================================

const char* pci_class_name(uint8_t class_code) {
    switch (class_code) {
        case PCI_CLASS_UNCLASSIFIED: return "Unclassified";
        case PCI_CLASS_STORAGE: return "Storage";
        case PCI_CLASS_NETWORK: return "Network";
        case PCI_CLASS_DISPLAY: return "Display";
        case PCI_CLASS_MULTIMEDIA: return "Multimedia";
        case PCI_CLASS_MEMORY: return "Memory";
        case PCI_CLASS_BRIDGE: return "Bridge";
        case PCI_CLASS_COMMUNICATION: return "Communication";
        case PCI_CLASS_SYSTEM: return "System";
        case PCI_CLASS_INPUT: return "Input";
        case PCI_CLASS_DOCKING: return "Docking";
        case PCI_CLASS_PROCESSOR: return "Processor";
        case PCI_CLASS_SERIAL_BUS: return "Serial Bus";
        default: return "Unknown";
    }
}

const char* pci_vendor_name(uint16_t vendor_id) {
    switch (vendor_id) {
        case 0x8086: return "Intel";
        case 0x1022: return "AMD";
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "ATI/AMD";
        case 0x15AD: return "VMware";
        case 0x1234: return "QEMU";
        case 0x80EE: return "VirtualBox";
        case 0x1AF4: return "VirtIO";
        default: return "Unknown";
    }
}

void pci_dump_device(pci_device_t *dev) {
    com_write_string(COM1_PORT, "[PCI] Device: ");
    
    // Bus:Device.Function
    char buf[32];
    int pos = 0;
    
    // Bus
    if (dev->bus < 10) buf[pos++] = '0';
    buf[pos++] = '0' + (dev->bus / 10);
    buf[pos++] = '0' + (dev->bus % 10);
    buf[pos++] = ':';
    
    // Device
    if (dev->device < 10) buf[pos++] = '0';
    buf[pos++] = '0' + (dev->device / 10);
    buf[pos++] = '0' + (dev->device % 10);
    buf[pos++] = '.';
    
    // Function
    buf[pos++] = '0' + dev->function;
    buf[pos++] = ' ';
    buf[pos] = '\0';
    
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, pci_vendor_name(dev->vendor_id));
    com_write_string(COM1_PORT, " [");
    com_write_string(COM1_PORT, pci_class_name(dev->class_code));
    com_write_string(COM1_PORT, "]\n");
}

// ============================================================================
// Initialization
// ============================================================================

void pci_init(void) {
    COM_LOG_INFO(COM1_PORT, "Initializing PCI subsystem");
    
    pci_device_count = 0;
    pci_driver_count = 0;
    
    int count = pci_scan_bus();
    
    if (count == 0) {
        COM_LOG_WARN(COM1_PORT, "No PCI devices found");
        return;
    }
    
    COM_LOG_OK(COM1_PORT, "PCI subsystem initialized");
    
    // Dump all devices
    for (int i = 0; i < pci_device_count; i++) {
        pci_dump_device(&pci_devices[i]);
    }
}