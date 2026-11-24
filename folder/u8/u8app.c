// U8 (CH32V006) screen firmware for the AK820 Pro. Installed at 0x800 by the U8 bootloader
// (u8boot.c) over the one-wire link to U7 (tools/u8update.ps1); never needs the Pico.
//
// Screen: GC9107 128x128 RGB565. U8 pins (found with research/u8-probe):
//   SPI1 SCK PC5, MOSI PC6, MISO PC7 (hardware SPI, 12 MHz; bit-bang fallback),
//   LCD CS PC4, DC PD0, RST PC3, backlight PC0 (high = on); SPI flash (PUYA) CS PD2.
// This panel needs display inversion + BGR order (without them: inverted colours, R/B swapped).
// Link to U7: PD6, 115200 8N1 (U7 may raise it with 'U'), half-duplex, each side drives push-pull
// only while sending, U7 = master. U7 first knocks with 0x7E
// (keyboard) or 0x7F (u8link / update tool) until U8 answers 0x79 (U8 checks the line between
// drawing steps), then a frame. The bootloader only reacts to 0x7F, so the keyboard's 0x7E can
// never hold U8 in its bootloader after a reset.
//   5A cmd aL aH lL lH data[len] sum   -> 79 ok / 1F error   (sum = 8-bit sum after 5A, len <= 256)
//   'P' -> 79 'A' version        'B' -> 79, restart into the bootloader (updates)
//   'L' a = backlight 0/1        'M' a = preset | rotation << 8
//   'T' data = h m s weekday(0 = Sun) day month year-2000      (U7 resends every 10 s)
//   'I' data = battery% flags(1 caps, 2 win lock, 4 usb, 8 bt) kpm(16) today(16)
//   'H' -> 79 + flash JEDEC id (3)
//   'E' data = addr(3): erase the 4 KB flash sector      'Q' data = addr(3) + bytes: program
//   'J' a = n (<= 128), data = addr(3) -> 79 + n bytes
// Presets: 0 clock (Georgian time from U7), 1 logo, 2 analog clock, 3 status, 4 typing speed,
//          5 matrix rain, 6 plasma, 7 starfield, 8 game of life, 9 own image / GIF (from flash).
// Own image in flash: 0x0 header "AKIM", [4] frames, [8 + 2i] delay ms of frame i (u16 LE);
//                     frame i (128x128 RGB565 big-endian, 32 KB) at 0x1000 + i * 0x8000.
// QingKe V2C: multiply, no divide (no / or % on variables in this file).
#include <stdint.h>
#define R32(a) (*(volatile uint32_t *)(a))
#define R16(a) (*(volatile uint16_t *)(a))

#define VERSION 8
#define BOOT_MAGIC (*(volatile uint32_t *)0x20001FFCu)
#define MAGIC 0xB007B007u

#define RCC_CFGR0    0x40021004u
#define RCC_APB2PCEN 0x40021018u
#define STK_CTLR     0xE000F000u
#define STK_CNT      0xE000F008u
#define PFIC_CFGR    0xE000E048u
#define F_ACTLR      0x40022000u
#define GPIOC 0x40011000u
#define GPIOD 0x40011400u
#define SPI_CTLR1 0x40013000u
#define SPI_STATR 0x40013008u
#define SPI_DATAR 0x4001300Cu
#define HZ  24000000u
// Link speed: starts at 115200 after every reset (like the bootloader); U7 may switch it with
// 'U' (a = baud / 1200) for faster uploads. 460800 garbled bytes on this board's wire, so the
// usable speed is measured, not assumed. Sending is push-pull (open-drain rises too slowly).
static uint32_t bitv = (HZ + 57600) / 115200;
#define BIT bitv
#define MS  24000u

// ================================================================ pins, time
static void cfg(uint32_t port, int pin, uint32_t m) { R32(port) = (R32(port) & ~(0xFu << (pin * 4))) | (m << (pin * 4)); }
static inline void pset(uint32_t port, int pin, int v) { R32(port + 0x10) = 1u << (pin + (v ? 0 : 16)); }
#define BL(v)   pset(GPIOC, 0, v)
#define RST(v)  pset(GPIOC, 3, v)
#define LCS(v)  pset(GPIOC, 4, v)
#define DC(v)   pset(GPIOD, 0, v)
#define FCS(v)  pset(GPIOD, 2, v)
static inline uint32_t now(void) { return R32(STK_CNT); }
static inline void until(uint32_t t) { while ((int32_t)(now() - t) < 0) {} }
static void ms(uint32_t n) { until(now() + n * MS); }

static uint32_t rnd_s = 0x12345678u;
static uint32_t rnd(void) { rnd_s ^= rnd_s << 13; rnd_s ^= rnd_s >> 17; rnd_s ^= rnd_s << 5; return rnd_s; }
static uint32_t udiv(uint32_t a, uint32_t b, uint32_t *rem) {   // software divide (no div on V2C)
    uint32_t q = 0, r = 0;
    for (int i = 31; i >= 0; i--) { r = (r << 1) | ((a >> i) & 1); if (r >= b) { r -= b; q |= 1u << i; } }
    if (rem) *rem = r;
    return q;
}

// ================================================================ SPI (hardware, bit-bang fallback)
static uint8_t hw;
static void spi_w(uint8_t v) {
    if (hw) { while (!(R16(SPI_STATR) & 2)) {} R16(SPI_DATAR) = v; return; }
    for (int k = 7; k >= 0; k--) {
        R32(GPIOC + 0x10) = (v >> k) & 1 ? (1u << 6) : (1u << 22);
        R32(GPIOC + 0x10) = 1u << 5; R32(GPIOC + 0x10) = 1u << 21;
    }
}
static void spi_end(void) {   // wait until the last byte is out, drop what came in
    if (!hw) return;
    while (!(R16(SPI_STATR) & 2)) {}
    while (R16(SPI_STATR) & 0x80) {}
    (void)R16(SPI_DATAR); (void)R16(SPI_STATR);
}
static uint8_t spi_r(void) {
    if (hw) { spi_end(); R16(SPI_DATAR) = 0xFF; while (!(R16(SPI_STATR) & 1)) {} return R16(SPI_DATAR); }
    uint8_t v = 0;
    for (int k = 0; k < 8; k++) { R32(GPIOC + 0x10) = 1u << 5; v = (v << 1) | ((R32(GPIOC + 8) >> 7) & 1); R32(GPIOC + 0x10) = 1u << 21; }
    return v;
}
static void spi_mode(int on_hw) {
    hw = on_hw;
    if (hw) {
        R32(RCC_APB2PCEN) |= 1u << 12;                          // SPI1 clock
        cfg(GPIOC, 5, 0xB); cfg(GPIOC, 6, 0xB);                 // SCK, MOSI: alternate push-pull
        R16(SPI_CTLR1) = 0x0304 | 0x40;                         // master, SSM+SSI, fPCLK/2 = 12 MHz, on
    } else {
        R16(SPI_CTLR1) = 0;
        pset(GPIOC, 5, 0); cfg(GPIOC, 5, 0x3); pset(GPIOC, 6, 0); cfg(GPIOC, 6, 0x3);
    }
}

// ================================================================ SPI flash
static uint8_t jedec[3];
static void fl_cmd(uint8_t c, uint32_t a, int with_addr) {
    FCS(0); spi_w(c);
    if (with_addr) { spi_w(a >> 16); spi_w(a >> 8); spi_w(a); }
}
static void fl_wait(void) {
    for (;;) { FCS(0); spi_w(0x05); uint8_t s = spi_r(); spi_end(); FCS(1); if (!(s & 1)) return; }
}
static void fl_read(uint32_t a, uint8_t *d, int n) {
    fl_cmd(0x03, a, 1);
    for (int i = 0; i < n; i++) d[i] = spi_r();
    spi_end(); FCS(1);
}
static void fl_wren(void) { fl_cmd(0x06, 0, 0); spi_end(); FCS(1); }
static void fl_erase(uint32_t a) { fl_wren(); fl_cmd(0x20, a, 1); spi_end(); FCS(1); fl_wait(); }
static void fl_prog(uint32_t a, const uint8_t *d, int n) {
    fl_wren(); fl_cmd(0x02, a, 1);
    for (int i = 0; i < n; i++) spi_w(d[i]);
    spi_end(); FCS(1); fl_wait();
}
static void fl_id(void) {
    FCS(0); spi_w(0xAB); spi_end(); FCS(1); ms(1);              // release from power-down
    FCS(0); spi_w(0x9F); for (int i = 0; i < 3; i++) jedec[i] = spi_r(); spi_end(); FCS(1);
}

// ================================================================ LCD
static uint8_t rot = 1, ox = 1, oy = 2;
static void lcmd(uint8_t c) { spi_end(); LCS(1); DC(0); LCS(0); spi_w(c); spi_end(); DC(1); }
static void ldat(uint8_t d) { spi_w(d); }
static void lcd_done(void) { spi_end(); LCS(1); }
static void window(int x0, int y0, int x1, int y1) {
    x0 += ox; x1 += ox; y0 += oy; y1 += oy;
    lcmd(0x2A); ldat(x0 >> 8); ldat(x0); ldat(x1 >> 8); ldat(x1);
    lcmd(0x2B); ldat(y0 >> 8); ldat(y0); ldat(y1 >> 8); ldat(y1);
    lcmd(0x2C);                                                  // pixel data follows (DC = 1, CS low)
}
static inline void px(uint16_t c) { spi_w(c >> 8); spi_w(c); }
static void set_rot(uint8_t r) {
    // MADCTL (BGR always) and the visible area's offset inside the controller's 132x162 RAM
    static const uint8_t MAD[4] = {0x08, 0x68, 0xC8, 0xA8}, OX[4] = {2, 1, 2, 31}, OY[4] = {1, 2, 31, 2};
    rot = r & 3; ox = OX[rot]; oy = OY[rot];
    lcmd(0x36); ldat(MAD[rot]); lcd_done();
}
static void lcd_init(void) {
    RST(1); ms(5); RST(0); ms(10); RST(1); ms(120);
    lcmd(0xFE); lcmd(0xEF);                                      // GC inter-register enable 1/2
    lcmd(0x11); lcd_done(); ms(120);                             // sleep out
    lcmd(0x3A); ldat(0x05);                                      // RGB565
    lcmd(0x21);                                                  // display inversion on (this panel)
    set_rot(rot);
    lcmd(0x29); lcd_done(); ms(20);                              // display on
}
static void rect(int x, int y, int w, int h, uint16_t c) {
    if (w <= 0 || h <= 0) return;
    window(x, y, x + w - 1, y + h - 1);
    for (int i = w * h; i > 0; i--) px(c);
    lcd_done();
}
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define BLACK 0x0000
#define WHITE 0xFFFF
#define ORANGE RGB(217, 119, 87)
#define GREY RGB(90, 90, 90)
#define DIM RGB(40, 40, 40)

// ================================================================ text (5x7 font, scaled)
static const char GLYPHS[] = " %-./0123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const uint8_t FONT[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x23,0x13,0x08,0x64,0x62}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30},
    {0x01,0x71,0x09,0x05,0x03}, {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x01,0x01}, {0x3E,0x41,0x41,0x51,0x32}, {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x04,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F}, {0x7F,0x20,0x18,0x20,0x7F}, {0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03}, {0x61,0x51,0x49,0x45,0x43}};
static const uint8_t *glyph(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    for (int i = 0; GLYPHS[i]; i++) if (GLYPHS[i] == c) return FONT[i];
    return FONT[0];
}
static int text_w(const char *s, int sc) { int n = 0; while (s[n]) n++; return n * 6 * sc - sc; }
static void text(int x, int y, const char *s, int sc, uint16_t fg, uint16_t bg) {
    for (; *s; s++, x += 6 * sc) {
        const uint8_t *g = glyph(*s);
        int w = s[1] ? 6 * sc : 5 * sc;                          // no gap column after the last char
        window(x, y, x + w - 1, y + 7 * sc - 1);
        for (int r = 0; r < 7; r++) for (int rr = 0; rr < sc; rr++)
            for (int c = 0; c < (s[1] ? 6 : 5); c++) {
                uint16_t col = c < 5 && ((g[c] >> r) & 1) ? fg : bg;
                for (int cc = 0; cc < sc; cc++) px(col);
            }
        lcd_done();
    }
}
static void text_c(int y, const char *s, int sc, uint16_t fg, uint16_t bg) { text((128 - text_w(s, sc)) >> 1, y, s, sc, fg, bg); }
static void num(char *o, uint32_t v, int digits) {               // zero-padded decimal
    for (int i = digits - 1; i >= 0; i--) { uint32_t r; v = udiv(v, 10, &r); o[i] = '0' + r; }
    o[digits] = 0;
}
static void num_plain(char *o, uint32_t v) {                     // no padding
    char t[11]; num(t, v, 10); int i = 0; while (i < 9 && t[i] == '0') i++;
    int k = 0; while (t[i]) o[k++] = t[i++]; o[k] = 0;
}

// ================================================================ state from U7
static uint8_t mode = 1, dirty = 1, uploading, refresh;
static uint8_t t_valid, t_h, t_m, t_s, t_wd, t_d, t_mo, t_y;
static uint32_t t_tick;
static uint8_t i_bat = 255, i_flags, i_new;
static uint16_t i_kpm, i_today;
static void time_tick(void) {
    if (!t_valid) return;
    while ((int32_t)(now() - t_tick) >= (int32_t)HZ) {
        t_tick += HZ;
        if (++t_s >= 60) { t_s = 0; if (++t_m >= 60) { t_m = 0; if (++t_h >= 24) t_h = 0; } }
    }
}

// ================================================================ link (PD6)
// Receive with the hardware USART1 (RX = PD6): it catches every byte by itself and keeps it
// until we look, so drawing code can be slow. (Bit-banged receive lost bytes that arrived back
// to back once the firmware grew.) Send stays bit-banged, open-drain, receiver off meanwhile.
#define LPIN 6
#define U_STATR 0x40013800u
#define U_DATAR 0x40013804u
#define U_BRR   0x40013808u
#define U_CTLR1 0x4001380Cu
static inline int rx_ready(void) { return R32(U_STATR) & 0x20; }   // RXNE
static uint32_t t_last;
static int rx(uint32_t timeout) {
    uint32_t t0 = now();
    while (!rx_ready()) if (now() - t0 > timeout) return -1;
    uint32_t st = R32(U_STATR);
    int b = R32(U_DATAR) & 0xFF;                 // also clears RXNE / ORE
    t_last = now();
    return (st & 0x02) ? -1 : b;                 // framing error
}
static void uart_init(void) {
    R32(RCC_APB2PCEN) |= 1u << 14;               // USART1
    R32(U_BRR) = bitv;                           // USART divider = HCLK / baud = bit time in ticks
    R32(U_CTLR1) = (1u << 13) | (1u << 2);       // UE, RE (receiver only)
}
static void tx(uint8_t b) {
    uint32_t w = 0x200u | ((uint32_t)b << 1), t = now();
    R32(U_CTLR1) = 1u << 13;                     // receiver off: don't hear ourselves
    pset(GPIOD, LPIN, 1); cfg(GPIOD, LPIN, 0x3);  // push-pull while sending
    for (int k = 0; k < 10; k++) { pset(GPIOD, LPIN, (w >> k) & 1); t += BIT; until(t); }
    cfg(GPIOD, LPIN, 0x8);
    (void)R32(U_STATR); (void)R32(U_DATAR);
    R32(U_CTLR1) = (1u << 13) | (1u << 2);
    t_last = now();
}
static uint8_t buf[260];
static void handle(uint8_t cmd, uint32_t a, uint32_t len) {
    uint32_t fa = buf[0] << 16 | buf[1] << 8 | buf[2];
    switch (cmd) {
    case 'P': tx(0x79); tx('A'); tx(VERSION); break;
    case 'B': tx(0x79); ms(2); BOOT_MAGIC = MAGIC; R32(PFIC_CFGR) = 0xBEEF0080u; for (;;) {}
    case 'L': BL(a & 1); tx(0x79); break;
    case 'S': mode = 1; dirty = 1; tx(0x79); break;
    case 'M': {                              // U7 repeats it every 10 s: only redraw on a change
        uint8_t m = (a & 0xFF) > 9 ? 1 : a & 0xFF;
        if (((a >> 8) & 3) != rot) { set_rot(a >> 8); dirty = 1; }
        if (m != mode || uploading) { mode = m; dirty = 1; }
        uploading = 0; tx(0x79);
    } break;
    case 'T':
        if (len < 7) { tx(0x1F); break; }
        t_h = buf[0]; t_m = buf[1]; t_s = buf[2]; t_wd = buf[3]; t_d = buf[4]; t_mo = buf[5]; t_y = buf[6];
        t_tick = now(); t_valid = 1; tx(0x79); break;
    case 'I':
        if (len < 6) { tx(0x1F); break; }
        i_bat = buf[0]; i_flags = buf[1]; i_kpm = buf[2] | buf[3] << 8; i_today = buf[4] | buf[5] << 8;
        i_new = 1; tx(0x79); break;
    case 'H': tx(0x79); tx(jedec[0]); tx(jedec[1]); tx(jedec[2]); break;
    case 'K': tx(0x79); for (uint32_t i = 0; i < len; i++) tx(buf[i]); break;   // echo (link test)
    case 'U': {                              // speed switch, answered at the old speed
        uint32_t baud = a * 1200;
        if (baud < 9600 || baud > 1000000) { tx(0x1F); break; }
        tx(0x79); until(now() + 20 * BIT);
        bitv = udiv(HZ + (baud >> 1), baud, 0); R32(U_BRR) = bitv;   // no divide on V2C
    } break;
    case 'E': if (len < 3) { tx(0x1F); break; } uploading = 1; dirty = 1; fl_erase(fa); tx(0x79); break;
    case 'Q':
        if (len < 4 || ((fa & 0xFF) + len - 3) > 256) { tx(0x1F); break; }
        uploading = 1; dirty = 1; fl_prog(fa, buf + 3, len - 3); tx(0x79); break;
    case 'J': {
        uint32_t n = a > 128 ? 128 : a;
        fl_read(fa, buf + 4, n); tx(0x79);
        for (uint32_t i = 0; i < n; i++) tx(buf[4 + i]);
    } break;
    default: tx(0x1F);
    }
}
// A byte from U7 is waiting: serve U7 until it has been quiet for 30 ms. Returns 1. A drawing
// step that was cut by this must be redone by its caller (refresh = 1).
#define IWDG_KR 0x40003000u
static int serve(void) {
    lcd_done();
    for (;;) {
        R32(IWDG_KR) = 0xAAAA;                   // feed the watchdog: an upload keeps us here for minutes
        int b = rx(30 * MS);
        if (b < 0) break;
        if (b == 0x7F || b == 0x7E) { until(t_last + 3 * BIT); tx(0x79); continue; }   // knock
        if (b != 0x5A) continue;
        uint8_t h[5], sum = 0; int bad = 0;
        for (int i = 0; i < 5; i++) { int c = rx(20 * MS); if (c < 0) { bad = 1; break; } h[i] = c; sum += c; }
        if (bad) continue;
        uint32_t a = h[1] | h[2] << 8, len = h[3] | h[4] << 8;
        if (len > 259) continue;                   // 'Q': 3 address bytes + a 256-byte page
        for (uint32_t i = 0; i < len; i++) { int c = rx(20 * MS); if (c < 0) { bad = 1; break; } buf[i] = c; sum += c; }
        if (bad) continue;
        int s = rx(20 * MS);
        if (s < 0) continue;
        until(t_last + 3 * BIT);
        if ((uint8_t)s != sum) {                 // diagnosis: 1F + what arrived
            tx(0x1F); for (int i = 0; i < 5; i++) tx(h[i]); tx(s); tx(sum); continue;
        }
        handle(h[0], a, len);
    }
    return 1;
}
static int poll(void) { R32(IWDG_KR) = 0xAAAA; time_tick(); return rx_ready() ? serve() : 0; }   // call often while drawing
#define CUT() if (rx_ready()) { serve(); refresh = 1; return; }       // inside an LCD transfer
// any CPU fault: restart (the bootloader then starts us again) instead of hanging silently
__attribute__((interrupt, aligned(4))) void trap(void) { R32(PFIC_CFGR) = 0xBEEF0080u; for (;;) {} }

// ================================================================ presets
static const int8_t SIN[256] = {0, 3, 6, 9, 12, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46, 49, 51, 54, 57, 60, 63, 65, 68, 71, 73, 76, 78, 81, 83, 85, 88, 90, 92, 94, 96, 98, 100, 102, 104, 106, 107, 109, 111, 112, 113, 115, 116, 117, 118, 120, 121, 122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127, 127, 127, 127, 127, 126, 126, 126, 125, 125, 124, 123, 122, 122, 121, 120, 118, 117, 116, 115, 113, 112, 111, 109, 107, 106, 104, 102, 100, 98, 96, 94, 92, 90, 88, 85, 83, 81, 78, 76, 73, 71, 68, 65, 63, 60, 57, 54, 51, 49, 46, 43, 40, 37, 34, 31, 28, 25, 22, 19, 16, 12, 9, 6, 3, 0, -3, -6, -9, -12, -16, -19, -22, -25, -28, -31, -34, -37, -40, -43, -46, -49, -51, -54, -57, -60, -63, -65, -68, -71, -73, -76, -78, -81, -83, -85, -88, -90, -92, -94, -96, -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116, -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127, -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118, -117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100, -98, -96, -94, -92, -90, -88, -85, -83, -81, -78, -76, -73, -71, -68, -65, -63, -60, -57, -54, -51, -49, -46, -43, -40, -37, -34, -31, -28, -25, -22, -19, -16, -12, -9, -6, -3};
static int cosv(uint8_t a) { return SIN[(uint8_t)(a + 64)]; }
static uint16_t wheel(uint8_t h) {
    if (h < 85) return RGB(255 - h * 3, h * 3, 0);
    if (h < 170) { h -= 85; return RGB(0, 255 - h * 3, h * 3); }
    h -= 170; return RGB(h * 3, 0, 255 - h * 3);
}
static void clear(uint16_t c) { rect(-ox, -oy, 132, 132, c); }   // visible area + the offset rows

// ---- Georgian text (Mkhedruli, generated from Noto Sans Georgian: geofont.h), UTF-8 strings
#include "geofont.h"
static const geo_glyph_t *geo_find(const char **p) {
    const uint8_t *s = (const uint8_t *)*p;
    uint32_t cp = *s++;
    if (cp >= 0xE0 && s[0] && s[1]) { cp = (cp & 0x0F) << 12 | (s[0] & 0x3F) << 6 | (s[1] & 0x3F); s += 2; }
    else if (cp >= 0xC0 && s[0]) { cp = (cp & 0x1F) << 6 | (s[0] & 0x3F); s += 1; }
    *p = (const char *)s;
    for (unsigned i = 0; i < sizeof(GEO) / sizeof(GEO[0]); i++) if (GEO[i].cp == cp) return &GEO[i];
    return &GEO[sizeof(GEO) / sizeof(GEO[0]) - 1];                // unknown: the space glyph
}
static int gtext_w(const char *s) { int w = 0; while (*s) w += geo_find(&s)->w + 1; return w ? w - 1 : 0; }
static void gtext(int x, int y, const char *s, uint16_t fg, uint16_t bg) {
    while (*s) {
        const geo_glyph_t *g = geo_find(&s);
        int w = g->w + (*s ? 1 : 0);                                // 1 px gap except after the last
        window(x, y, x + w - 1, y + 15);
        for (int r = 0; r < 16; r++) for (int c = 0; c < w; c++) px(c < g->w && (g->row[r] >> (15 - c)) & 1 ? fg : bg);
        lcd_done();
        x += w;
    }
}
static void gtext_c(int y, const char *s, uint16_t fg, uint16_t bg) { gtext((128 - gtext_w(s)) >> 1, y, s, fg, bg); }

static const char *const WD[7] = {"კვირა", "ორშაბათი", "სამშაბათი", "ოთხშაბათი", "ხუთშაბათი", "პარასკევი", "შაბათი"};
static const char *const MO[13] = {"", "იანვარი", "თებერვალი", "მარტი", "აპრილი", "მაისი", "ივნისი",
                                   "ივლისი", "აგვისტო", "სექტემბერი", "ოქტომბერი", "ნოემბერი", "დეკემბერი"};
static uint16_t bat_col(uint8_t b) { return b <= 20 ? RGB(230, 60, 50) : b <= 50 ? RGB(240, 190, 40) : RGB(90, 210, 90); }

// ---- 0 digital clock (Tbilisi time, kept by U7), Georgian texts, battery top right
static void p_clock(int full) {
    static uint8_t ls = 255, lm = 255, ld = 255, lv = 2, lb = 254;
    if (full) { clear(BLACK); gtext(4, 2, "თბილისი", GREY, BLACK); }
    if (full || refresh) { ls = lm = ld = 255; lv = 2; lb = 254; }
    if (i_bat != lb) {                                              // "85%" + a small battery icon
        lb = i_bat; char s[8];
        if (i_bat > 100) { s[0] = s[1] = '-'; s[2] = 0; } else num_plain(s, i_bat);
        int n = 0; while (s[n]) n++; s[n] = '%'; s[n + 1] = 0;
        rect(64, 2, 64, 16, BLACK);
        int w = gtext_w(s);
        gtext(104 - 3 - w, 2, s, WHITE, BLACK);
        uint16_t c = i_bat > 100 ? GREY : bat_col(i_bat);
        rect(104, 5, 18, 10, c); rect(105, 6, 16, 8, BLACK); rect(122, 8, 2, 4, c);   // outline + tip
        if (i_bat <= 100) rect(106, 7, (i_bat * 14u * 41u) >> 12, 6, c);            // fill: bat * 14 / 100
    }
    if (t_valid != lv) {
        lv = t_valid;
        rect(0, 24, 128, 104, BLACK);
        if (!t_valid) { text_c(40, "--:--", 4, ORANGE, BLACK); gtext_c(86, "გახსენით", GREY, BLACK); gtext_c(104, "პარამეტრები", GREY, BLACK); }
    }
    if (!t_valid) return;
    char s[12];
    if (t_m != lm) { lm = t_m; num(s, t_h, 2); s[2] = ':'; num(s + 3, t_m, 2); text_c(30, s, 4, ORANGE, BLACK); }
    if (t_s != ls) { ls = t_s; num(s, t_s, 2); text_c(64, s, 2, WHITE, BLACK); }
    if (t_d != ld) {
        ld = t_d;
        rect(0, 84, 128, 40, BLACK);
        gtext_c(84, WD[t_wd > 6 ? 0 : t_wd], WHITE, BLACK);
        char d[40]; num_plain(d, t_d);
        int n = 0; while (d[n]) n++; d[n++] = ' ';
        const char *m = MO[t_mo > 12 ? 0 : t_mo]; while (*m && n < 38) d[n++] = *m++; d[n] = 0;
        gtext_c(104, d, GREY, BLACK);
    }
}

// ---- 1 logo: Claude-style starburst
static const int16_t RAY[12][3] = {
    {255,-18,50}, {204,154,40}, {143,212,54}, {-31,254,44}, {-112,230,48}, {-236,100,38},
    {-255,18,52}, {-204,-154,42}, {-143,-212,50}, {31,-254,40}, {112,-230,46}, {236,-100,44}};
static int in_burst(int x, int y) {
    if (x * x + y * y < 100) return 1;
    for (int r = 0; r < 12; r++) {
        int t = (x * RAY[r][0] + y * RAY[r][1]) >> 8, d = (x * RAY[r][1] - y * RAY[r][0]) >> 8;
        if (d < 0) d = -d;
        int L = RAY[r][2];
        if (t < 0 || t > L) continue;
        if (d <= 6 - (t * 3 >= L) - (t * 3 >= 2 * L) - (t >= L)) return 1;
    }
    return 0;
}
static void p_logo(int full) {
    if (!full && !refresh) return;
    window(-ox, -oy, 131 - ox, 131 - oy);
    for (int y = 0; y < 132; y++) {
        for (int x = 0; x < 132; x++) { px(in_burst(x - 66, y - 66) ? ORANGE : RGB(20, 20, 18)); if (!(x & 31) && rx_ready()) { serve(); refresh = 1; return; } }
        CUT();                         // redraw after the link
    }
    lcd_done();
}

// ---- 2 analog clock
static void p_analog(int full) {
    static uint8_t ls = 255;
    if (full || refresh) ls = 255;
    if (!t_valid) { if (ls != 254) { clear(BLACK); text_c(54, "SET TIME IN", 1, GREY, BLACK); text_c(66, "THE SETTINGS PAGE", 1, GREY, BLACK); ls = 254; } return; }
    if (t_s == ls) return;
    ls = t_s;
    uint8_t h12 = t_h >= 12 ? t_h - 12 : t_h;
    uint8_t as = (t_s * 4369u) >> 10, am = (t_m * 4369u) >> 10, ah = ((h12 * 60u + t_m) * 364u) >> 10;
    struct { int8_t x, y; uint8_t len, w; uint16_t c; } hand[3] = {
        {SIN[ah], -cosv(ah), 30, 3, WHITE}, {SIN[am], -cosv(am), 44, 2, WHITE}, {SIN[as], -cosv(as), 50, 1, ORANGE}};
    window(0, 0, 127, 127);
    for (int y = 0; y < 128; y++) {
        int dy = y - 64;
        for (int x = 0; x < 128; x++) {
            int dx = x - 64, d2 = dx * dx + dy * dy;
            uint16_t c = BLACK;
            if (d2 >= 57 * 57 && d2 <= 60 * 60) c = GREY;
            else if (d2 >= 47 * 47 && d2 < 55 * 55) {                // hour ticks
                for (int k = 0; k < 12; k++) {
                    uint8_t a = (k * 5461u) >> 8;                    // k * 256 / 12
                    int along = dx * SIN[a] - dy * cosv(a);           // along the tick's direction
                    int across = dx * cosv(a) + dy * SIN[a];          // distance from its line
                    if (across < 0) across = -across;
                    if (along > 0 && across <= 2 * 127) { c = k == 0 ? ORANGE : WHITE; break; }
                }
            }
            for (int k = 0; k < 3; k++) {
                int t = dx * hand[k].x + dy * hand[k].y, d = dx * hand[k].y - dy * hand[k].x;
                if (d < 0) d = -d;
                if (t >= -6 * 127 && t <= hand[k].len * 127 && d <= hand[k].w * 127) c = hand[k].c;
            }
            if (d2 <= 9) c = ORANGE;
            px(c);
        }
        if (rx_ready()) { serve(); ls = 255; return; }
    }
    lcd_done();
}

// ---- 3 status
static void p_status(int full) {
    static uint8_t lb = 254, lf = 255;
    if (full) { clear(BLACK); text_c(6, "AK820 PRO", 1, GREY, BLACK); }
    if (full || refresh) { lb = 254; lf = 255; }
    if (i_bat != lb) {
        lb = i_bat; char s[6];
        if (i_bat > 100) { s[0] = '-'; s[1] = '-'; s[2] = 0; } else { num_plain(s, i_bat); }
        int n = 0; while (s[n]) n++; s[n] = '%'; s[n + 1] = 0;
        rect(0, 24, 128, 28, BLACK);
        text_c(24, s, 4, i_bat <= 20 ? RGB(230, 60, 50) : i_bat <= 50 ? RGB(240, 190, 40) : RGB(90, 210, 90), BLACK);
        int w = i_bat > 100 ? 0 : (i_bat * 107u) >> 7;               // of 84 px
        rect(22, 58, 84, 8, DIM); rect(22, 58, w, 8, ORANGE);
    }
    if (i_flags != lf) {
        lf = i_flags;
        text(10, 80, "CAPS", 2, i_flags & 1 ? ORANGE : DIM, BLACK);
        text(76, 80, "WIN", 2, i_flags & 2 ? ORANGE : DIM, BLACK);
        rect(0, 104, 128, 16, BLACK);
        text_c(106, i_flags & 4 ? "USB" : i_flags & 8 ? "BLUETOOTH" : "NOT CONNECTED", 1, GREY, BLACK);
    }
}

// ---- 4 typing speed
static void p_typing(int full) {
    static uint16_t lk = 65535, lt = 65535;
    if (full) { clear(BLACK); text_c(8, "TYPING SPEED", 1, GREY, BLACK); text_c(76, "KEYS / MIN", 1, GREY, BLACK); }
    if (full || refresh) { lk = lt = 65535; }
    char s[12];
    if (i_kpm != lk) { lk = i_kpm; num_plain(s, i_kpm); rect(0, 30, 128, 40, BLACK); text_c(32, s, 5, ORANGE, BLACK); }
    if (i_today != lt) { lt = i_today; char t[20] = "TODAY "; num_plain(t + 6, i_today); rect(0, 100, 128, 10, BLACK); text_c(100, t, 1, WHITE, BLACK); }
}

// ---- 5 matrix rain (21 columns x 16 rows of 6x8 cells)
static uint8_t m_head[21], m_spd[21], m_cnt[21];
static void cell(int cx, int cy, char ch, uint16_t c) { char s[2] = {ch, 0}; text(cx * 6 + 1, cy * 8, s, 1, c, BLACK); }
static void p_matrix(int full) {
    static uint32_t next;
    if (full) { clear(BLACK); for (int i = 0; i < 21; i++) { m_head[i] = rnd() & 15; m_spd[i] = 1 + (rnd() & 3); m_cnt[i] = 0; } }
    if ((int32_t)(now() - next) < 0) return;
    next = now() + 40 * MS;
    static const char CH[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (int i = 0; i < 21; i++) {
        if (++m_cnt[i] < m_spd[i]) continue;
        m_cnt[i] = 0;
        int h = m_head[i];
        if (h < 16) cell(i, h, CH[rnd() & 31], RGB(40, 170, 60));          // old head -> green
        int tail = h - 9;
        if (tail >= 0 && tail < 16) cell(i, tail, ' ', BLACK);
        h++;
        if (h > 26) { h = 0; m_spd[i] = 1 + (rnd() & 3); }
        m_head[i] = h;
        if (h < 16) cell(i, h, CH[rnd() & 31], RGB(200, 255, 200));        // new head: bright
        if (poll()) return;
    }
}

// ---- 6 plasma (64x64 computed, shown 2x2)
static void p_plasma(int full) {
    static uint8_t t;
    (void)full;
    t += 3;
    uint16_t row[64];
    window(0, 0, 127, 127);
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            int v = SIN[(uint8_t)(x * 4 + t)] + SIN[(uint8_t)(y * 5 - t * 2)] + SIN[(uint8_t)((x + y) * 3 + t)] + SIN[(uint8_t)(x * 2 - y * 3 - t)];
            row[x] = wheel((uint8_t)((v >> 1) + t));
        }
        for (int k = 0; k < 2; k++) for (int x = 0; x < 64; x++) { px(row[x]); px(row[x]); }
        CUT();
    }
    lcd_done();
}

// ---- 7 starfield
#define NST 48
static int16_t s_x[NST], s_y[NST]; static uint8_t s_z[NST], s_px[NST], s_py[NST], s_on[NST];
static uint16_t recip[64];
static void star_new(int i) { s_x[i] = (int16_t)(rnd() & 255) - 128; s_y[i] = (int16_t)(rnd() & 255) - 128; s_z[i] = 40 + (rnd() & 15); }
static void p_stars(int full) {
    static uint32_t next;
    if (full) { clear(BLACK); for (int i = 0; i < NST; i++) { star_new(i); s_z[i] = 4 + (rnd() & 51); s_on[i] = 0; } }
    if ((int32_t)(now() - next) < 0) return;
    next = now() + 30 * MS;
    for (int i = 0; i < NST; i++) {
        if (s_on[i]) rect(s_px[i], s_py[i], 2, 2, BLACK);
        s_on[i] = 0;
        if (s_z[i] <= 2) star_new(i); else s_z[i] -= 1;
        int sx = 64 + ((s_x[i] * (int)recip[s_z[i]]) >> 9), sy = 64 + ((s_y[i] * (int)recip[s_z[i]]) >> 9);
        if (sx < 0 || sy < 0 || sx > 126 || sy > 126) { star_new(i); continue; }
        uint8_t b = s_z[i] < 12 ? 255 : s_z[i] < 25 ? 170 : 90;
        rect(sx, sy, s_z[i] < 12 ? 2 : 1, s_z[i] < 12 ? 2 : 1, RGB(b, b, b));
        s_px[i] = sx; s_py[i] = sy; s_on[i] = 1;
    }
    poll();
}

// ---- 8 game of life (32x32, 4 px cells)
static uint32_t g_a[32], g_b[32];
static uint16_t g_gen;
static void life_seed(void) { for (int y = 0; y < 32; y++) g_a[y] = rnd() & rnd(); g_gen = 0; }
static void p_life(int full) {
    static uint32_t next;
    if (full) { clear(BLACK); life_seed(); for (int y = 0; y < 32; y++) g_b[y] = 0; }
    if (refresh) { refresh = 0; for (int y = 0; y < 32; y++) g_b[y] = ~g_a[y]; }   // force redraw
    if ((int32_t)(now() - next) < 0) return;
    next = now() + 120 * MS;
    // draw differences between shown (g_b) and current (g_a)
    for (int y = 0; y < 32; y++) {
        uint32_t d = g_a[y] ^ g_b[y];
        for (int x = 0; d && x < 32; x++) if ((d >> x) & 1) rect(x * 4, y * 4, 3, 3, (g_a[y] >> x) & 1 ? wheel((x + y) * 4 + (g_gen >> 1)) : BLACK);
        if (poll()) return;
    }
    uint32_t nxt[32]; int alive = 0, same = 1;
    for (int y = 0; y < 32; y++) {
        uint32_t up = g_a[(y + 31) & 31], dn = g_a[(y + 1) & 31], mid = g_a[y], r = 0;
        for (int x = 0; x < 32; x++) {
            int n = 0;
            for (int k = -1; k <= 1; k++) {
                int xx = (x + k) & 31;
                n += (up >> xx) & 1; n += (dn >> xx) & 1; if (k) n += (mid >> xx) & 1;
            }
            int on = (mid >> x) & 1;
            if (n == 3 || (on && n == 2)) r |= 1u << x;
        }
        nxt[y] = r; alive |= r != 0; same &= r == g_a[y];
    }
    for (int y = 0; y < 32; y++) { g_b[y] = g_a[y]; g_a[y] = nxt[y]; }
    if (!alive || same || ++g_gen > 600) { life_seed(); }
}

// ---- 9 own image / GIF from the SPI flash
static void p_custom(int full) {
    static uint8_t n, f;
    static uint32_t next;
    static uint16_t delay[120];
    if (full || refresh) {
        uint8_t h[8];
        fl_read(0, h, 8);
        n = (h[0] == 'A' && h[1] == 'K' && h[2] == 'I' && h[3] == 'M') ? h[4] : 0;
        if (n > 120) n = 0;
        if (n) { uint8_t d[240]; fl_read(8, d, n * 2); for (int i = 0; i < n; i++) delay[i] = d[2 * i] | d[2 * i + 1] << 8; }
        f = 0; next = now();
        if (!n && full) { clear(BLACK); text_c(52, "NO IMAGE YET", 1, GREY, BLACK); text_c(66, "UPLOAD ONE IN THE", 1, GREY, BLACK); text_c(76, "SETTINGS PAGE", 1, GREY, BLACK); }
        refresh = 0;
    }
    if (!n || (int32_t)(now() - next) < 0) return;
    uint32_t base = 0x1000u + (uint32_t)f * 0x8000u;
    uint8_t row[256];
    window(0, 0, 127, 127);
    lcd_done();
    for (int y = 0; y < 128; y++) {
        fl_read(base + y * 256, row, 256);
        LCS(0); DC(1);                                            // continue the RAM write
        for (int i = 0; i < 256; i++) spi_w(row[i]);
        lcd_done();
        CUT();
    }
    next = now() + (delay[f] < 20 ? 20 : delay[f]) * MS;
    if (++f >= n) f = 0;
}

static void p_uploading(int full) {
    if (!full) return;
    clear(BLACK); text_c(52, "UPLOADING", 2, ORANGE, BLACK); text_c(76, "KEEP THE CABLE IN", 1, GREY, BLACK);
}

// ================================================================ main
void app_main(void) {
    R32(F_ACTLR) = 1;
    R32(RCC_APB2PCEN) |= 0x3Du;
    R32(RCC_CFGR0) &= ~0xF0u;                                    // 24 MHz
    R32(STK_CTLR) = 0x5;
    __asm__ volatile("csrw mtvec, %0" :: "r"((uint32_t)trap));   // direct mode
    // independent watchdog: ~2 s without poll() -> reset (LSI ~128 kHz / 128, reload 2000)
    R32(IWDG_KR) = 0x5555; R32(IWDG_KR + 4) = 5; R32(IWDG_KR + 8) = 2000; R32(IWDG_KR) = 0xAAAA; R32(IWDG_KR) = 0xCCCC;
    BL(0); cfg(GPIOC, 0, 0x3);
    RST(1); cfg(GPIOC, 3, 0x3);
    LCS(1); cfg(GPIOC, 4, 0x3);
    DC(1); cfg(GPIOD, 0, 0x3);
    FCS(1); cfg(GPIOD, 2, 0x3);
    pset(GPIOC, 7, 1); cfg(GPIOC, 7, 0x8);                       // MISO pull-up input
    pset(GPIOD, LPIN, 1); cfg(GPIOD, LPIN, 0x8);                 // link: pull-up input
    uart_init();
    // hardware SPI if the flash answers through it (PUYA = 0x85), else bit-bang
    spi_mode(1); fl_id();
    if (jedec[0] != 0x85) { spi_mode(0); fl_id(); }
    for (int z = 1; z < 64; z++) recip[z] = udiv(65536, z, 0) >> 3;   // starfield projection
    rnd_s ^= now();
    lcd_init();
    p_logo(1);
    BL(1);

    uint8_t shown = 255;
    for (;;) {
        if (poll()) {}
        if (uploading) { if (dirty) { dirty = 0; p_uploading(1); } continue; }
        int full = dirty || shown != mode;
        if (full) { dirty = 0; shown = mode; refresh = 0; }
        switch (mode) {
        case 0: p_clock(full); break;
        case 1: p_logo(full); break;
        case 2: p_analog(full); break;
        case 3: p_status(full); break;
        case 4: p_typing(full); break;
        case 5: p_matrix(full); break;
        case 6: p_plasma(full); break;
        case 7: p_stars(full); break;
        case 8: p_life(full); break;
        case 9: p_custom(full); break;
        }
        if (mode != 8 && mode != 9) refresh = 0;
    }
}

__attribute__((naked, section(".text.start"))) void _start(void) {
    __asm__ volatile(".option push\n .option norvc\n j 1f\n .option pop\n"
                     "1: csrci mstatus, 8\n li sp, 0x20001FF0\n"
                     "la a0, _sidata\n la a1, _sdata\n la a2, _edata\n"                       // .data from flash
                     "4: bgeu a1, a2, 5f\n lw a3, 0(a0)\n sw a3, 0(a1)\n addi a0, a0, 4\n addi a1, a1, 4\n j 4b\n"
                     "5: la a0, _sbss\n la a1, _ebss\n2: bgeu a0, a1, 3f\n sw zero, 0(a0)\n addi a0, a0, 4\n j 2b\n"
                     "3: j app_main\n");
}
