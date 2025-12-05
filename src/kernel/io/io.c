#include "moduos/kernel/io/io.h"

uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

void insb(uint16_t port, void *addr, int count) {
    __asm__ volatile (
        "rep insb"
        : "+D"(addr), "+c"(count)
        : "d"(port)
        : "memory"
    );
}

void outsb(uint16_t port, const void *addr, int count) {
    __asm__ volatile (
        "rep outsb"
        : "+S"(addr), "+c"(count)
        : "d"(port)
    );
}

void insw(uint16_t port, void *addr, int count) {
    __asm__ volatile (
        "rep insw"
        : "+D"(addr), "+c"(count)
        : "d"(port)
        : "memory"
    );
}

void outsw(uint16_t port, const void *addr, int count) {
    __asm__ volatile (
        "rep outsw"
        : "+S"(addr), "+c"(count)
        : "d"(port)
    );
}

void insl(uint16_t port, void *addr, int count) {
    __asm__ volatile (
        "rep insl"
        : "+D"(addr), "+c"(count)
        : "d"(port)
        : "memory"
    );
}

void outsl(uint16_t port, const void *addr, int count) {
    __asm__ volatile (
        "rep outsl"
        : "+S"(addr), "+c"(count)
        : "d"(port)
    );
}

void io_wait(void) {
    __asm__ volatile ("outb %%al, $0x80" : : "a"(0));
}
