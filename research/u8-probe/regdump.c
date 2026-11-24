// U8 RAM program: copy 4 registers (A0..A3, given with -D) into the mailbox the probe's 'S'
// command reads, then spin. Shows how the halted stock firmware configured its pins/peripherals.
#include <stdint.h>
#define R32(a) (*(volatile uint32_t *)(a))
#define MBOX ((volatile uint32_t *)0x20000300u)

__attribute__((naked, section(".text.start"))) void _start(void) {
    __asm__ volatile("csrci mstatus, 8\n li sp, 0x20001F00\n j main_dump\n");
}
void main_dump(void) {
    for (;;) { MBOX[0] = R32(A0); MBOX[1] = R32(A1); MBOX[2] = R32(A2); MBOX[3] = R32(A3); }
}
