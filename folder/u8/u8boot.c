// U8 (CH32V006) bootloader for the AK820 Pro screen chip. Lives in the first 2 KB of U8's flash
// (installed once with the Pico, research/u8-probe/flashwr.c); the screen firmware sits at 0x800
// and is updated through this bootloader over the one-wire link to U7 (U8 PD6 <-> U7 PB21-23),
// so the Pico is never needed again.
//
// Link: 115200 8N1, half-duplex, open-drain on both sides (U7 provides the pull-up), U7 = master.
// Stays in the bootloader when: the app slot is empty, the app asked for it (RAM magic), or U7
// sends 0x7F within 500 ms after reset (U7 resets U8 when it reboots). Otherwise starts the app.
// Commands (U7 -> U8), replies after >= 3 bit times:
//   0x7F                         -> 0x79                          (sync / "are you there")
//   5A 'P' 0 0 0 0 sum           -> 79 'B' version
//   5A 'W' aL aH 00 01 data[256] sum -> 79 ok / 1F error          (erase + program + verify page)
//   5A 'R' aL aH lL lH sum       -> 79 data[len] sum8(data)       (len <= 256)
//   5A 'G' 0 0 0 0 sum           -> 79, then the app starts
// sum = 8-bit sum of every byte after 0x5A. Addresses are flash offsets; 'W' only >= 0x800.
#include <stdint.h>
#define R32(a) (*(volatile uint32_t *)(a))

#define VERSION 2
#define APP 0x800u
#define APP_END 0xF800u
#define BOOT_MAGIC (*(volatile uint32_t *)0x20001FFCu)
#define MAGIC 0xB007B007u

#define RCC_CFGR0    0x40021004u
#define RCC_APB2PCEN 0x40021018u
#define STK_CTLR     0xE000F000u
#define STK_CNT      0xE000F008u
#define GPIOD 0x40011400u
#define PIN 6
#define BIT (24000000u / 115200u)
#define MS  24000u
#define WINDOW (500 * MS)   // v2: 30 ms was shorter than U7's start-up after power-on

#define F_ACTLR  0x40022000u
#define KEYR     0x40022004u
#define STATR    0x4002200Cu
#define CTLR     0x40022010u
#define ADDR     0x40022014u
#define MODEKEYR 0x40022024u
#define C_STRT   0x00040u
#define C_FTPG   0x10000u
#define C_FTER   0x20000u
#define C_BUFLD  0x40000u
#define C_BUFRST 0x80000u

static inline uint32_t now(void) { return R32(STK_CNT); }
static inline void until(uint32_t t) { while ((int32_t)(now() - t) < 0) {} }
static inline int rd(void) { return (R32(GPIOD + 8) >> PIN) & 1; }
static void mode(uint32_t m) { R32(GPIOD) = (R32(GPIOD) & ~(0xFu << (PIN * 4))) | (m << (PIN * 4)); }

static uint32_t t_last;   // end of the last received / sent byte

// one byte, or -1 after `timeout` ticks without a start bit
static int rx(uint32_t timeout) {
    uint32_t t0 = now();
    while (rd()) if (now() - t0 > timeout) return -1;
    uint32_t t = now() + BIT + BIT / 2;
    uint8_t b = 0;
    for (int k = 0; k < 8; k++) { until(t); b |= rd() << k; t += BIT; }
    until(t);                                   // middle of the stop bit
    int ok = rd();
    while (!rd()) if (now() - t > 2 * BIT) break;
    t_last = now();
    return ok ? b : -1;
}
static void tx(uint8_t b) {
    uint32_t w = 0x200u | ((uint32_t)b << 1), t = now();
    R32(GPIOD + 0x10) = 1u << PIN; mode(0x7);   // open-drain output, released
    for (int k = 0; k < 10; k++) { R32(GPIOD + 0x10) = (w >> k) & 1 ? (1u << PIN) : (1u << (PIN + 16)); t += BIT; until(t); }
    mode(0x8);                                   // pull-up input again (ODR = 1)
    t_last = now();
}
static void reply_gap(void) { until(t_last + 3 * BIT); }

static int fwait(void) {
    for (uint32_t i = 0; i < 2000000u; i++) if (!(R32(STATR) & 3)) return (R32(STATR) & 0x10) ? -1 : 0;
    return -2;
}
static int write_page(uint32_t off, const uint32_t *d) {
    uint32_t a = 0x08000000u + off;
    R32(KEYR) = 0x45670123u; R32(KEYR) = 0xCDEF89ABu;
    R32(MODEKEYR) = 0x45670123u; R32(MODEKEYR) = 0xCDEF89ABu;
    int e = 0;
    R32(CTLR) = C_FTER; R32(ADDR) = a; R32(CTLR) = C_FTER | C_STRT; e |= fwait();
    R32(CTLR) = C_FTPG; R32(CTLR) = C_FTPG | C_BUFRST; e |= fwait();
    for (int w = 0; w < 64; w++) { R32(a + w * 4) = d[w]; R32(CTLR) = C_FTPG | C_BUFLD; e |= fwait(); }
    R32(ADDR) = a; R32(CTLR) = C_FTPG | C_STRT; e |= fwait();
    R32(CTLR) = 0x80;                            // lock
    for (int w = 0; w < 64; w++) if (R32(a + w * 4) != d[w]) e = -3;
    return e;
}

static void start_app(void) {
    mode(0x4);                                   // leave PD6 floating for the app
    ((void (*)(void))APP)();
}

void boot_main(void) {
    R32(F_ACTLR) = 1;                            // 1 wait state for 24 MHz
    R32(RCC_APB2PCEN) |= 0x3Du;
    R32(RCC_CFGR0) &= ~0xF0u;                    // HCLK = 24 MHz HSI
    R32(STK_CTLR) = 0x5;
    R32(GPIOD + 0x10) = 1u << PIN; mode(0x8);
    int stay = BOOT_MAGIC == MAGIC || R32(APP) == 0xFFFFFFFFu;
    BOOT_MAGIC = 0;
    if (!stay) {
        uint32_t t0 = now();
        while (now() - t0 < WINDOW) if (rx(WINDOW - (now() - t0)) == 0x7F) { stay = 1; reply_gap(); tx(0x79); break; }
    }
    if (!stay) start_app();

    static uint32_t buf[64];
    for (;;) {
        int b = rx(0xFFFFFFFFu);
        if (b == 0x7F) { reply_gap(); tx(0x79); continue; }
        if (b != 0x5A) continue;
        uint8_t h[5], sum = 0; int bad = 0;
        for (int i = 0; i < 5; i++) { int c = rx(20 * MS); if (c < 0) { bad = 1; break; } h[i] = c; sum += c; }
        if (bad) continue;
        uint32_t off = h[1] | h[2] << 8, len = h[3] | h[4] << 8;
        if (h[0] == 'W') {
            if (len != 256) continue;
            uint8_t *p = (uint8_t *)buf;
            for (int i = 0; i < 256; i++) { int c = rx(20 * MS); if (c < 0) { bad = 1; break; } p[i] = c; sum += c; }
            if (bad) continue;
        }
        int s = rx(20 * MS);
        if (s < 0) continue;
        reply_gap();
        if ((uint8_t)s != sum) { tx(0x1F); continue; }
        switch (h[0]) {
        case 'P': tx(0x79); tx('B'); tx(VERSION); break;
        case 'W':
            if ((off & 0xFF) || off < APP || off + 256 > APP_END) { tx(0x1F); break; }
            tx(write_page(off, buf) ? 0x1F : 0x79);
            break;
        case 'R': {
            if (len > 256 || off + len > APP_END) { tx(0x1F); break; }
            uint8_t s8 = 0; tx(0x79);
            for (uint32_t i = 0; i < len; i++) { uint8_t c = *(volatile uint8_t *)(0x08000000u + off + i); s8 += c; tx(c); }
            tx(s8);
        } break;
        case 'G': tx(0x79); start_app(); break;
        default: tx(0x1F);
        }
    }
}

__attribute__((naked, section(".text.start"))) void _start(void) {
    __asm__ volatile(".option push\n .option norvc\n j 1f\n .option pop\n"
                     "1: csrci mstatus, 8\n li sp, 0x20001FF0\n j boot_main\n");
}
