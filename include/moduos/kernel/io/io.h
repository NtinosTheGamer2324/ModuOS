#ifndef IO_H
#define IO_H

#include <stdint.h>

// Basic port I/O
uint8_t inb(uint16_t port);
void outb(uint16_t port, uint8_t val);

uint16_t inw(uint16_t port);
void outw(uint16_t port, uint16_t val);

uint32_t inl(uint16_t port);
void outl(uint16_t port, uint32_t val);

// Block I/O
void insb(uint16_t port, void *addr, int count);
void outsb(uint16_t port, const void *addr, int count);

void insw(uint16_t port, void *addr, int count);
void outsw(uint16_t port, const void *addr, int count);

void insl(uint16_t port, void *addr, int count);
void outsl(uint16_t port, const void *addr, int count);

// I/O wait
void io_wait(void);

#endif // IO_H
