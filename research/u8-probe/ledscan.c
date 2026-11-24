// RAM-resident LED scan for the keyboard's U8 (WCH CH32V006, QingKe V2C, RV32EC).
// Loaded into SRAM by the Pico debug probe and started by setting DPC; never touches flash.
// For each GPIO pin it bit-bangs a WS2812-style "dim white" frame stream with three
// pulse-width profiles, so whichever pin drives the RGB LED chain makes the LEDs light up.
// Progress is published in a RAM mailbox that the probe reads while the scan runs.
#include <stdint.h>

#define R32(a) (*(volatile uint32_t *)(a))
#define RCC_CFGR0    0x40021004u
#define RCC_APB2PCEN 0x40021018u
#define MBOX ((volatile uint32_t *)0x20000300u)   // [0] magic/state [1] port<<8|pin [2] profile [3] heartbeat

static const uint32_t ports[4] = {0x40010800u, 0x40010C00u, 0x40011000u, 0x40011400u}; // GPIOA..D
// High-time loop counts for a 0 bit and a 1 bit: short, medium, long (clock speed is uncertain).
static const uint8_t prof[3][2] = {{1, 6}, {2, 10}, {4, 16}};

static inline void dly(uint32_t n) { while (n--) __asm__ volatile("nop"); }

void main_scan(void);

__attribute__((naked, section(".text.start"))) void _start(void) {
    __asm__ volatile(
        "csrci mstatus, 8\n"      // no interrupts: the halted firmware's handlers must not run
        "li sp, 0x20001F00\n"
        "j main_scan\n");
}

void main_scan(void) {
    R32(RCC_APB2PCEN) |= 0x3Du;                 // AFIO + GPIOA/B/C/D clocks
    R32(RCC_CFGR0) &= ~0xF0u;                   // HCLK = SYSCLK (no AHB divider)
    MBOX[0] = 0xC0DE0001u;
    MBOX[3] = 0;
    for (uint32_t round = 0;; round++) {
        for (uint32_t p = 0; p < 4; p++) {
            for (uint32_t pin = 0; pin < 8; pin++) {
                if (p == 0 && (pin == 1 || pin == 2 || pin == 4)) continue;   // PA1/PA2: 24 MHz crystal; PA4 reset U8 when driven
                if (p == 3 && (pin == 1 || pin == 7)) continue;   // PD1 = SWIO (our link), PD7 = NRST
                uint32_t base = ports[p];
                uint32_t old = R32(base);                          // CFGLR
                uint32_t sh = pin * 4;
                R32(base) = (old & ~(0xFu << sh)) | (0x3u << sh);  // push-pull output, fast
                uint32_t set = 1u << pin, clr = 1u << (pin + 16);
                for (uint32_t pr = 0; pr < 3; pr++) {
                    MBOX[1] = (p << 8) | pin;
                    MBOX[2] = pr | (round << 8);
                    for (uint32_t f = 0; f < 100; f++) {
                        MBOX[3]++;
                        for (uint32_t led = 0; led < 150; led++) {
                            for (uint32_t b = 0; b < 24; b++) {
                                uint32_t one = (b & 7) == 3;      // each byte = 0x10 -> ~6% white
                                R32(base + 0x10) = set;           // BSHR: high
                                dly(one ? prof[pr][1] : prof[pr][0]);
                                R32(base + 0x10) = clr;           // BSHR: low
                                dly(12);
                            }
                        }
                        dly(3000);                                 // >80 us latch/reset gap
                    }
                }
                R32(base + 0x10) = clr;
                R32(base) = old;                                   // pin back to its old mode
            }
        }
    }
}
