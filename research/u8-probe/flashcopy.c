// RAM-resident helper for U8 (CH32V006): the CPU itself copies one chunk of its own flash into
// SRAM at 0x20000800, so the debug probe can read it from RAM. Read-only on flash.
// Build with -DSRC=<flash address> (chunk size CHUNK bytes). Result in mailbox at 0x20000300:
// [0] = 0xF1A50000 | done flag, [1] = SRC, [2] = word count, [3] = sum of copied words.
#include <stdint.h>
#define MBOX ((volatile uint32_t *)0x20000300u)
#define DST  ((volatile uint32_t *)0x20000800u)
#ifndef CHUNK
#define CHUNK 0x1600u
#endif

void main_copy(void);

__attribute__((naked, section(".text.start"))) void _start(void) {
    __asm__ volatile("csrci mstatus, 8\n li sp, 0x20001F00\n j main_copy\n");
}

void main_copy(void) {
    volatile const uint32_t *src = (volatile const uint32_t *)(SRC);
    uint32_t sum = 0;
    MBOX[0] = 0xF1A50000u;
    for (uint32_t i = 0; i < CHUNK / 4; i++) { uint32_t v = src[i]; DST[i] = v; sum += v; }
    MBOX[1] = SRC; MBOX[2] = CHUNK / 4; MBOX[3] = sum;
    MBOX[0] = 0xF1A50001u;
    for (;;) __asm__ volatile("nop");
}
