// U8 RAM program: first picture on the AK820 Pro screen (GC9107, 128x128, RGB565).
// Pins found by flashscan/lcdscan: SCK PC5, MOSI PC6 (shared with the SPI flash, CS PD2 held
// high), LCD CS/DC = PC4/PD0 in an unknown order. All other free pins are driven HIGH (keeps
// the LCD out of reset and, if active-high, the backlight on). Draws a Claude-style starburst:
//   order A (CS=PC4, DC=PD0): orange burst,  order B (CS=PD0, DC=PC4): blue burst.
// Alternates A / B every ~5 s. Mailbox [0] = C0DE0004, [1] = 0xA or 0xB (order last drawn).
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
#define P_C4 (16 + 4)
#define P_D0 (24 + 0)
static int cs_pin, dc_pin;

static int other(int i) {   // free pins we bias high
    return !(i == 1 || i == 2 || i == 4 || i == 24 + 1 || i == 24 + 7 || i == SCK || i == MOSI || i == MISO ||
             i == FCS || i == P_C4 || i == P_D0);
}
static void mode(int i, uint32_t m) {
    uint32_t b = PORT[i >> 3], sh = (i & 7) * 4;
    R32(b) = (R32(b) & ~(0xFu << sh)) | (m << sh);
}
static void set(int i, int v) { R32(PORT[i >> 3] + 0x10) = 1u << ((i & 7) + (v ? 0 : 16)); }
static void ms(uint32_t n) { while (n--) for (volatile uint32_t w = 0; w < 2400; w++) {} }

static void tx8(uint8_t v) {
    uint32_t sck = PORT[2] + 0x10;   // PC5 / PC6 on GPIOC: fast path via BSHR
    for (int k = 7; k >= 0; k--) {
        R32(sck) = (v >> k) & 1 ? (1u << 6) : (1u << 22);
        R32(sck) = 1u << 5;
        R32(sck) = 1u << 21;
    }
}
static void cmd(uint8_t c) { set(dc_pin, 0); set(cs_pin, 0); tx8(c); set(cs_pin, 1); }
static void dat(uint8_t d) { set(dc_pin, 1); set(cs_pin, 0); tx8(d); set(cs_pin, 1); }

static const int16_t RAY[12][3] = {
    {255,-18,50}, {204,154,40}, {143,212,54}, {-31,254,44}, {-112,230,48}, {-236,100,38},
    {-255,18,52}, {-204,-154,42}, {-143,-212,50}, {31,-254,40}, {112,-230,46}, {236,-100,44}};

static int in_burst(int x, int y) {
    if (x * x + y * y < 100) return 1;                        // solid centre
    for (int r = 0; r < 12; r++) {
        int t = (x * RAY[r][0] + y * RAY[r][1]) >> 8;          // along the ray
        int d = (x * RAY[r][1] - y * RAY[r][0]) >> 8;          // across it
        if (d < 0) d = -d;
        int L = RAY[r][2];
        if (t < 0 || t > L) continue;
        int w = 6 - (t * 3 >= L) - (t * 3 >= 2 * L) - (t >= L);  // tapers 6..3 px (V2C has no divide)
        if (d <= w) return 1;
    }
    return 0;
}

static void draw(uint16_t fg) {
    const uint16_t bg = 0x2104;                                // near-black
    cmd(0x01); ms(150);                                        // software reset
    cmd(0xFE); cmd(0xEF);                                      // GC inter-register enable 1/2
    cmd(0x11); ms(150);                                        // sleep out
    cmd(0x3A); dat(0x05);                                      // 16-bit RGB565
    cmd(0x36); dat(0x00);                                      // MADCTL: normal
    cmd(0x29); ms(20);                                         // display on
    cmd(0x2A); dat(0); dat(0); dat(0); dat(131);               // columns 0..131 (covers offsets)
    cmd(0x2B); dat(0); dat(0); dat(0); dat(131);               // rows 0..131
    cmd(0x2C);
    set(dc_pin, 1); set(cs_pin, 0);
    for (int y = 0; y < 132; y++) for (int x = 0; x < 132; x++) {
        uint16_t c = in_burst(x - 66, y - 66) ? fg : bg;
        tx8(c >> 8); tx8(c & 0xFF);
    }
    set(cs_pin, 1);
}

void main_lcd(void) {
    R32(RCC_APB2PCEN) |= 0x3Du;
    R32(RCC_CFGR0) &= ~0xF0u;                                  // 24 MHz HSI, no divider
    MBOX[0] = 0xC0DE0004u;
    set(FCS, 1); mode(FCS, 0x3);
    set(MISO, 1); mode(MISO, 0x8);
    set(SCK, 0); mode(SCK, 0x3); set(MOSI, 0); mode(MOSI, 0x3);
    for (int i = 0; i < 32; i++) if (other(i)) { set(i, 1); mode(i, 0x3); }
    set(P_C4, 1); mode(P_C4, 0x3); set(P_D0, 1); mode(P_D0, 0x3);
    ms(50);
    // VERIFIED on the board (2026-11-13): order A = CS PC4, DC PD0 shows the orange burst.
    cs_pin = P_C4; dc_pin = P_D0; MBOX[1] = 0xA; draw(0xDBAA);   // Claude orange
#ifdef BLINK_ALL
    // backlight check: every other free pin LOW for 3 s, HIGH for 3 s (RST = PC3 stays high)
    for (uint32_t n = 0;; n++) {
        int lo = !(n & 1);
        for (int i = 0; i < 32; i++) if (other(i) && i != 16 + 3) set(i, !lo);
        MBOX[2] = lo; MBOX[3] = n;
        ms(3000);
    }
#endif
#ifdef HIGH_MASK
    // bisect: only the free pins in HIGH_MASK (bit = port*8+pin) high, others low (RST PC3 high);
    // re-init + redraw every 2 s so "dark" means backlight/power off, not a lost picture
    for (int i = 0; i < 32; i++) if (other(i) && i != 16 + 3) set(i, (HIGH_MASK >> i) & 1);
    ms(200); draw(0xDBAA);                                        // one redraw, then steady
    for (;;) {}
#endif
#ifdef BLINK_ONE
    // confirm one backlight pin: all other free pins LOW (RST PC3 high), BLINK_ONE toggles 2 s
    for (int i = 0; i < 32; i++) if (other(i) && i != 16 + 3 && i != BLINK_ONE) set(i, 0);
    for (uint32_t n = 0;; n++) { set(BLINK_ONE, n & 1); MBOX[2] = n & 1; MBOX[3] = n; ms(2000); }
#endif
    for (;;) {}
}

__attribute__((naked, section(".text.start"))) void _start(void) {
    __asm__ volatile("csrci mstatus, 8\n li sp, 0x20000F00\n j main_lcd\n");
}
