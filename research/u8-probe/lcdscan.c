// U8 RAM program: find the GC9107 LCD's CS and DC pins. SCK = PC5 and MOSI = PC6 are shared
// with the SPI flash (flashscan: flash CS = PD2, kept high). For every (CS, DC) pair, read the
// display ID (cmd 0x04, then 0xDA): the GC9107 answers on its SDA line (= MOSI, PC6) or an SDO
// line (maybe MISO, PC7). The other candidate pins are biased so RST / backlight / power pins
// are on: pass 0 = pull-up inputs, pass 1 = driven high, pass 2 = driven low.
// Mailbox: [0] state (C0DE0003 running, C0DE00D3 done) [1] tried | hits<<24
// [2] first hit pass<<24|cs<<16|dc<<8|pin [3] its 24-bit answer (0x04) | 0xDA answer<<24
#include <stdint.h>
#define R32(a) (*(volatile uint32_t *)(a))
#define RCC_CFGR0    0x40021004u
#define RCC_APB2PCEN 0x40021018u
#define MBOX ((volatile uint32_t *)0x20000300u)

static const uint32_t PORT[4] = {0x40010800u, 0x40010C00u, 0x40011000u, 0x40011400u};
#define SCK  (16 + 5)
#define MOSI (16 + 6)
#define MISO (16 + 7)
#define FCS  (24 + 2)
static int cand(int i) {
    return !(i == 1 || i == 2 || i == 4 || i == 24 + 1 || i == 24 + 7 || i == SCK || i == MOSI || i == MISO || i == FCS);
}
static void mode(int i, uint32_t m) {
    uint32_t b = PORT[i >> 3], sh = (i & 7) * 4;
    R32(b) = (R32(b) & ~(0xFu << sh)) | (m << sh);
}
static void set(int i, int v) { R32(PORT[i >> 3] + 0x10) = 1u << ((i & 7) + (v ? 0 : 16)); }
static int get(int i) { return (R32(PORT[i >> 3] + 8) >> (i & 7)) & 1; }
static void dly(uint32_t n) { while (n--) __asm__ volatile("nop"); }

static void tx8(uint8_t v) {
    for (int k = 7; k >= 0; k--) { set(MOSI, (v >> k) & 1); dly(3); set(SCK, 1); dly(3); set(SCK, 0); }
}
// clock n bits in, sampling PC6 and PC7 on the rising edge
static void rxn(int n, uint32_t *a, uint32_t *b) {
    *a = *b = 0;
    for (int k = 0; k < n; k++) {
        dly(3); set(SCK, 1); dly(3);
        *a = (*a << 1) | get(MOSI); *b = (*b << 1) | get(MISO);
        set(SCK, 0);
    }
}
static int plaus(uint32_t v, int bits) { uint32_t all = (1u << bits) - 1; v &= all; return v != 0 && v != all; }

void main_scan(void) {
    R32(RCC_APB2PCEN) |= 0x3Du;
    R32(RCC_CFGR0) &= ~0xF0u;
    MBOX[0] = 0xC0DE0003u; MBOX[1] = 0; MBOX[2] = 0; MBOX[3] = 0;
    uint32_t tried = 0, hits = 0;
    set(FCS, 1); mode(FCS, 0x3);                       // flash deselected
    set(MISO, 1); mode(MISO, 0x8);                     // pull-up input
    set(SCK, 0); mode(SCK, 0x3); set(MOSI, 0); mode(MOSI, 0x3);
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < 32; i++) if (cand(i)) { set(i, pass != 2); mode(i, pass == 0 ? 0x8 : 0x3); }
        for (volatile uint32_t w = 0; w < 300000; w++) {}   // ~ tens of ms: LCD power / reset settle
        for (int cs = 0; cs < 32; cs++) for (int dc = 0; dc < 32; dc++) {
            if (cs == dc || !cand(cs) || !cand(dc)) continue;
            set(cs, 1); mode(cs, 0x3); set(dc, 0); mode(dc, 0x3);
            uint32_t a04, b04, ada, bda;
            set(cs, 0); tx8(0x04); mode(MOSI, 0x8); set(MOSI, 1); rxn(25, &a04, &b04); set(cs, 1);
            mode(MOSI, 0x3); set(MOSI, 0); dly(20);
            set(cs, 0); tx8(0xDA); mode(MOSI, 0x8); set(MOSI, 1); rxn(9, &ada, &bda); set(cs, 1);
            mode(MOSI, 0x3); set(MOSI, 0);
            int pa = plaus(a04, 24) || plaus(ada, 8), pb = plaus(b04, 24) || plaus(bda, 8);
            if (pa || pb) {
                if (!hits) {
                    MBOX[2] = (pass << 24) | (cs << 16) | (dc << 8) | (pa ? MOSI : MISO);
                    MBOX[3] = pa ? ((a04 & 0xFFFFFF) | ((ada & 0xFF) << 24)) : ((b04 & 0xFFFFFF) | ((bda & 0xFF) << 24));
                }
                if (hits < 16) {   // full list for a later regdump of 0x20001100..
                    volatile uint32_t *L = (volatile uint32_t *)0x20001100u;
                    L[hits * 2] = (pass << 24) | (cs << 16) | (dc << 8) | (pa ? MOSI : MISO);
                    L[hits * 2 + 1] = pa ? ((a04 & 0xFFFFFF) | ((ada & 0xFF) << 24)) : ((b04 & 0xFFFFFF) | ((bda & 0xFF) << 24));
                }
                hits++;
            }
            // back to this pass's bias
            set(cs, pass != 2); set(dc, pass != 2);
            if (pass == 0) { mode(cs, 0x8); mode(dc, 0x8); }
            tried++;
            MBOX[1] = tried | (hits << 24);
        }
    }
    MBOX[0] = 0xC0DE00D3u;
    for (;;) {}
}

__attribute__((naked, section(".text.start"))) void _start(void) {
    __asm__ volatile("csrci mstatus, 8\n li sp, 0x20000F00\n j main_scan\n");
}
