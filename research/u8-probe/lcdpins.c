// U8 RAM program: find the LCD's RESET and BACKLIGHT pins (CS PC4, DC PD0, SCK PC5, MOSI PC6
// known). Start: every other free pin driven HIGH (screen works that way).
// 1) RESET, automatic: drive one pin LOW, read the display ID; the pin whose LOW makes the
//    answer vanish is RST. Mailbox [2] = bitmask of such pins (bit = port*8+pin), [3] = baseline.
// 2) BACKLIGHT, visual: for each other pin, the screen shows its name (e.g. "C3") for 2 s, then
//    that pin goes LOW for 2 s. The name shown right before the screen goes dark = backlight.
//    Mailbox [1] = pin under test (port*8+pin) | phase<<8 (1 = name shown, 2 = pin low).
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
#define LCS  (16 + 4)
#define LDC  (24 + 0)

static int other(int i) {
    return !(i == 1 || i == 2 || i == 4 || i == 24 + 1 || i == 24 + 7 || i == SCK || i == MOSI || i == MISO ||
             i == FCS || i == LCS || i == LDC);
}
static void mode(int i, uint32_t m) {
    uint32_t b = PORT[i >> 3], sh = (i & 7) * 4;
    R32(b) = (R32(b) & ~(0xFu << sh)) | (m << sh);
}
static void set(int i, int v) { R32(PORT[i >> 3] + 0x10) = 1u << ((i & 7) + (v ? 0 : 16)); }
static int get(int i) { return (R32(PORT[i >> 3] + 8) >> (i & 7)) & 1; }
static void ms(uint32_t n) { while (n--) for (volatile uint32_t w = 0; w < 2400; w++) {} }
static void dly(uint32_t n) { while (n--) __asm__ volatile("nop"); }

static void tx8(uint8_t v) {
    uint32_t r = PORT[2] + 0x10;
    for (int k = 7; k >= 0; k--) {
        R32(r) = (v >> k) & 1 ? (1u << 6) : (1u << 22);
        R32(r) = 1u << 5;
        R32(r) = 1u << 21;
    }
}
static void cmd(uint8_t c) { set(LDC, 0); set(LCS, 0); tx8(c); set(LCS, 1); }
static void dat(uint8_t d) { set(LDC, 1); set(LCS, 0); tx8(d); set(LCS, 1); }

static uint32_t read_id(void) {          // cmd 0x04, 1 dummy + 24 bits on SDA (PC6)
    uint32_t a = 0;
    set(LDC, 0); set(LCS, 0); tx8(0x04);
    mode(MOSI, 0x8); set(MOSI, 1);
    for (int k = 0; k < 25; k++) { dly(3); set(SCK, 1); dly(3); a = (a << 1) | get(MOSI); set(SCK, 0); }
    set(LCS, 1); mode(MOSI, 0x3); set(MOSI, 0);
    return a & 0xFFFFFF;
}
static int answers(void) { uint32_t v = read_id(); return v != 0 && v != 0xFFFFFF; }

static void init(void) {
    cmd(0x01); ms(150); cmd(0xFE); cmd(0xEF); cmd(0x11); ms(150);
    cmd(0x3A); dat(0x05); cmd(0x36); dat(0x00); cmd(0x29); ms(20);
}

// 3x5 font: A B C D 0..7, rows top->bottom, 3 bits each (MSB = left)
static const uint16_t FONT[12] = {
    0x2BED, 0x6BAE, 0x3923, 0x6B6E,                    // A B C D
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249};   // 0-7
static int glyph_px(int g, int cx, int cy) {         // cx 0..2, cy 0..4
    return (FONT[g] >> (14 - (cy * 3 + cx))) & 1;
}
static void draw_label(int pin) {
    const uint16_t bg = 0x2104, fg = 0xDBAA;
    int g0 = pin >> 3, g1 = 4 + (pin & 7);
    cmd(0x2A); dat(0); dat(0); dat(0); dat(131);
    cmd(0x2B); dat(0); dat(0); dat(0); dat(131);
    cmd(0x2C);
    set(LDC, 1); set(LCS, 0);
    for (int y = 0; y < 132; y++) for (int x = 0; x < 132; x++) {
        int on = 0, ty = y - 36;                      // glyphs 36x60 (scale 12), at y 36..95
        if (ty >= 0 && ty < 60) {
            // v * 2731 >> 15 = v / 12 for v < 60 (no divide on V2C)
            if (x >= 18 && x < 54) on = glyph_px(g0, (x - 18) * 2731 >> 15, ty * 2731 >> 15);
            else if (x >= 78 && x < 114) on = glyph_px(g1, (x - 78) * 2731 >> 15, ty * 2731 >> 15);
        }
        uint16_t c = on ? fg : bg;
        tx8(c >> 8); tx8(c & 0xFF);
    }
    set(LCS, 1);
}

void main_pins(void) {
    R32(RCC_APB2PCEN) |= 0x3Du;
    R32(RCC_CFGR0) &= ~0xF0u;
    MBOX[0] = 0xC0DE0005u; MBOX[1] = 0; MBOX[2] = 0; MBOX[3] = 0;
    set(FCS, 1); mode(FCS, 0x3);
    set(MISO, 1); mode(MISO, 0x8);
    set(SCK, 0); mode(SCK, 0x3); set(MOSI, 0); mode(MOSI, 0x3);
    set(LCS, 1); mode(LCS, 0x3); set(LDC, 1); mode(LDC, 0x3);
    for (int i = 0; i < 32; i++) if (other(i)) { set(i, 1); mode(i, 0x3); }
    ms(50);

    // 1) reset pin(s), automatic
    uint32_t base = 0, rst = 0;
    for (int r = 0; r < 4; r++) base += answers();
    MBOX[3] = base;                                    // 4 = LCD answers reliably with all high
    for (int p = 0; p < 32; p++) if (other(p)) {
        set(p, 0); ms(5);
        int a = 0; for (int r = 0; r < 4; r++) a += answers();
        set(p, 1); ms(130);                            // if it was RST: LCD restarts, wait
        if (base == 4 && a == 0) rst |= 1u << p;
    }
    MBOX[2] = rst;

    // 2) backlight, visual (RST pins skipped: they would blank the screen too)
    init();
    // v2: one pin low never darkened it, all low does -> probably 2+ pins in parallel. So now
    // ALL OTHER pins go low and only p stays high: the screen stays lit only for backlight pins.
    for (;;) for (int p = 0; p < 32; p++) if (other(p) && !(rst & (1u << p))) {
        MBOX[1] = p | (1 << 8); draw_label(p); ms(2000);
        MBOX[1] = p | (2 << 8);
        for (int i = 0; i < 32; i++) if (other(i) && !(rst & (1u << i)) && i != p) set(i, 0);
        ms(2000);
        for (int i = 0; i < 32; i++) if (other(i)) set(i, 1);
        ms(200);
        init();
    }
}

__attribute__((naked, section(".text.start"))) void _start(void) {
    __asm__ volatile("csrci mstatus, 8\n li sp, 0x20000F00\n j main_pins\n");
}
