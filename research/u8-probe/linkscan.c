// U8 RAM program: map the U7<->U8 link (U7 PB20-PB23). Each free U8 pin in turn sends a burst
// of (index + 1) pulses, 1 ms high / 1 ms low (index = port*8 + pin), then 150 ms idle LOW and
// goes back to a floating input. U7 ledfinder phase 22 counts the pulses per burst on PB20-23:
// "PB22: 30" = U8 pin index 29 = PD5.  Mailbox [1] = pin being pulsed.
// LCD/flash pins (PC0 PC3-PC7 PD0 PD2) are left alone; the screen keeps its picture.
#include <stdint.h>
#define R32(a) (*(volatile uint32_t *)(a))
#define RCC_CFGR0    0x40021004u
#define RCC_APB2PCEN 0x40021018u
#define MBOX ((volatile uint32_t *)0x20000300u)

static const uint32_t PORT[4] = {0x40010800u, 0x40010C00u, 0x40011000u, 0x40011400u};
static int free_pin(int i) {
    return !(i == 1 || i == 2 || i == 4 || i == 24 + 1 || i == 24 + 7 ||       // crystal, PA4, SWIO, NRST
             i == 16 + 0 || (i >= 16 + 3 && i <= 16 + 7) || i == 24 + 0 || i == 24 + 2);   // LCD + flash
}
static void mode(int i, uint32_t m) {
    uint32_t b = PORT[i >> 3], sh = (i & 7) * 4;
    R32(b) = (R32(b) & ~(0xFu << sh)) | (m << sh);
}
static void set(int i, int v) { R32(PORT[i >> 3] + 0x10) = 1u << ((i & 7) + (v ? 0 : 16)); }
static void ms(uint32_t n) { while (n--) for (volatile uint32_t w = 0; w < 2400; w++) {} }

void main_link(void) {
    R32(RCC_APB2PCEN) |= 0x3Du;
    R32(RCC_CFGR0) &= ~0xF0u;
    MBOX[0] = 0xC0DE0006u;
    for (int i = 0; i < 32; i++) if (free_pin(i)) mode(i, 0x4);   // floating inputs
    for (uint32_t round = 0;; round++) {
        for (int p = 0; p < 32; p++) if (free_pin(p)) {
            MBOX[1] = p; MBOX[2] = round;
            set(p, 0); mode(p, 0x3);
            for (int k = 0; k <= p; k++) { set(p, 1); ms(1); set(p, 0); ms(1); }
            ms(150);
            mode(p, 0x4);
            ms(20);
        }
    }
}

__attribute__((naked, section(".text.start"))) void _start(void) {
    __asm__ volatile("csrci mstatus, 8\n li sp, 0x20000F00\n j main_link\n");
}
