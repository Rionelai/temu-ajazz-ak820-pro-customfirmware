// U8 RAM program: one-wire UART echo on PD6 (the U7 link, U7 PB21-PB23), 115200 8N1.
// Receive a byte, wait 2 bit times, answer (byte ^ 0xA5) as open-drain (U7 holds the pull-up).
// Mailbox: [0] C0DE0007, [1] bytes received, [2] last byte, [3] framing errors.
#include <stdint.h>
#define R32(a) (*(volatile uint32_t *)(a))
#define RCC_CFGR0    0x40021004u
#define RCC_APB2PCEN 0x40021018u
#define STK_CTLR     0xE000F000u
#define STK_CNT      0xE000F008u
#define MBOX ((volatile uint32_t *)0x20000300u)
#define GPIOD 0x40011400u
#define PIN 6
#define BIT (24000000u / 115200u)   // 208 HCLK ticks

static inline uint32_t now(void) { return R32(STK_CNT); }
static inline void until(uint32_t t) { while ((int32_t)(now() - t) < 0) {} }
static inline int rd(void) { return (R32(GPIOD + 8) >> PIN) & 1; }
static void mode(uint32_t m) { R32(GPIOD) = (R32(GPIOD) & ~(0xFu << (PIN * 4))) | (m << (PIN * 4)); }

void main_echo(void) {
    R32(RCC_APB2PCEN) |= 0x3Du;
    R32(RCC_CFGR0) &= ~0xF0u;                     // 24 MHz
    R32(STK_CTLR) = 0x5;                          // SysTick on, HCLK
    MBOX[0] = 0xC0DE0007u; MBOX[1] = 0; MBOX[2] = 0; MBOX[3] = 0;
    R32(GPIOD + 0x10) = 1u << PIN; mode(0x8);     // input pull-up
    uint32_t rx = 0, ferr = 0;
    for (;;) {
        while (rd()) {}                            // start bit
        uint32_t t = now() + BIT + BIT / 2;
        uint8_t b = 0;
        for (int k = 0; k < 8; k++) { until(t); b |= rd() << k; t += BIT; }
        until(t);
        if (!rd()) { ferr++; MBOX[3] = ferr; while (!rd()) {} continue; }
        MBOX[1] = ++rx; MBOX[2] = b;
        t += 2 * BIT; until(t);
        uint32_t w = 0x200u | ((uint32_t)(b ^ 0xA5) << 1);   // start, 8 data, stop
        R32(GPIOD + 0x10) = 1u << PIN; mode(0x7);  // open-drain output, released
        for (int k = 0; k < 10; k++) {
            R32(GPIOD + 0x10) = (w >> k) & 1 ? (1u << PIN) : (1u << (PIN + 16));
            t += BIT; until(t);
        }
        mode(0x8);                                  // back to pull-up input (ODR = 1)
    }
}

__attribute__((naked, section(".text.start"))) void _start(void) {
    __asm__ volatile("csrci mstatus, 8\n li sp, 0x20000F00\n j main_echo\n");
}
