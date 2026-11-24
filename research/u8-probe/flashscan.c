// U8 RAM program: find the SPI flash (PUYA PY25Qxx, JEDEC 85 xx xx) on U8's pins by trying
// every (CS, SCK, MOSI) combination; MISO = any other pin whose read-back looks like an ID.
// The LCD most likely shares SCK/MOSI with it. Flash is only read (0xAB wake + 0x9F JEDEC ID).
// Mailbox (probe 'S'): [0] state (C0DE0002 running, C0DE00D2 done) [1] tuples tried | hits<<24
// [2] first hit cs<<24|sck<<16|mosi<<8|miso [3] its 3 ID bytes.
#include <stdint.h>
#define R32(a) (*(volatile uint32_t *)(a))
#define RCC_CFGR0    0x40021004u
#define RCC_APB2PCEN 0x40021018u
#define MBOX ((volatile uint32_t *)0x20000300u)

static const uint32_t PORT[4] = {0x40010800u, 0x40010C00u, 0x40011000u, 0x40011400u};   // GPIOA..D
// Pins never driven: PA1/PA2 crystal, PA4 resets U8, PD1 = SWIO (our probe), PD7 = NRST.
static int usable(int i) { return !(i == 1 || i == 2 || i == 4 || i == 24 + 1 || i == 24 + 7); }

static void mode(int i, uint32_t m) {          // m: 0x3 = push-pull out, 0x8 = input pull (ODR=1 -> up)
    uint32_t b = PORT[i >> 3], sh = (i & 7) * 4;
    R32(b) = (R32(b) & ~(0xFu << sh)) | (m << sh);
}
static void set(int i, int v) { R32(PORT[i >> 3] + 0x10) = 1u << ((i & 7) + (v ? 0 : 16)); }
static void dly(uint32_t n) { while (n--) __asm__ volatile("nop"); }

static uint8_t rd[3][4];   // per ID byte, per port: the bits of all 8 pins, assembled per pin below

static void xfer(int sck, int mosi, uint8_t out, int byte) {
    for (int k = 7; k >= 0; k--) {
        set(mosi, (out >> k) & 1); dly(2);
        set(sck, 1); dly(2);
        if (byte >= 0) for (int p = 0; p < 4; p++) {
            uint32_t v = R32(PORT[p] + 8);
            // store pin bits transposed later: keep 8 samples per port per byte
            ((uint8_t *)0x20001000u)[(byte * 4 + p) * 8 + (7 - k)] = (uint8_t)v;
        }
        set(sck, 0); dly(2);
    }
}

void main_scan(void) {
    R32(RCC_APB2PCEN) |= 0x3Du;                 // AFIO + GPIOA-D clocks
    R32(RCC_CFGR0) &= ~0xF0u;                   // HCLK = SYSCLK
    MBOX[0] = 0xC0DE0002u; MBOX[1] = 0; MBOX[2] = 0; MBOX[3] = 0;
    uint32_t tried = 0, hits = 0;
    for (int i = 0; i < 32; i++) if (usable(i)) { set(i, 1); mode(i, 0x8); }   // all pull-up inputs
    for (int cs = 0; cs < 32; cs++) for (int sck = 0; sck < 32; sck++) for (int mosi = 0; mosi < 32; mosi++) {
        if (cs == sck || cs == mosi || sck == mosi || !usable(cs) || !usable(sck) || !usable(mosi)) continue;
        set(cs, 1); set(sck, 0); set(mosi, 0);
        mode(cs, 0x3); mode(sck, 0x3); mode(mosi, 0x3);
        set(cs, 0); xfer(sck, mosi, 0xAB, -1); set(cs, 1); dly(200);   // release from power-down
        set(cs, 0); xfer(sck, mosi, 0x9F, -1);
        for (int b = 0; b < 3; b++) xfer(sck, mosi, 0xFF, b);
        set(cs, 1);
        for (int m = 0; m < 32; m++) {
            if (m == cs || m == sck || m == mosi || !usable(m)) continue;
            uint8_t id[3];
            for (int b = 0; b < 3; b++) {
                uint8_t v = 0;
                for (int s = 0; s < 8; s++) v = (v << 1) | ((((uint8_t *)0x20001000u)[(b * 4 + (m >> 3)) * 8 + s] >> (m & 7)) & 1);
                id[b] = v;
            }
            int plausible = id[0] != 0xFF && id[0] != 0x00 && !(id[0] == id[1] && id[1] == id[2]);
            if (plausible) {
                if (!hits) { MBOX[2] = (cs << 24) | (sck << 16) | (mosi << 8) | m; MBOX[3] = (id[0] << 16) | (id[1] << 8) | id[2]; }
                hits++;
            }
        }
        for (int i = 0; i < 32; i++) if (i == cs || i == sck || i == mosi) { set(i, 1); mode(i, 0x8); }
        tried++;
        MBOX[1] = tried | (hits << 24);
    }
    MBOX[0] = 0xC0DE00D2u;
    for (;;) {}
}

__attribute__((naked, section(".text.start"))) void _start(void) {
    __asm__ volatile("csrci mstatus, 8\n li sp, 0x20000F00\n j main_scan\n");
}
