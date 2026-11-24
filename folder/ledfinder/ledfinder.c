// LED finder for the AK820 Pro main chip U7 (WCH CH584). Installed with tools/update.ps1.
// The per-key LEDs are addressable RGB ("ARGB", WS2812-style, one data line) and their supply
// was 0 V with the original firmware gone, so something switches the LED power. This firmware
// tries every free U7 pin as the power switch (driven high, then low) and, at the same time,
// sends a dim-white WS2812 stream on all the other free pins. Each state lasts 3 s and is
// reported over the USB serial port (tools/kf_serial.ps1): note the state where the LEDs light.
#include "CH58x_common.h"

#ifndef LEDFINDER_PHASE
#define LEDFINDER_PHASE 25  // 1-16: LED search (solved in 16), 17-19: screen search, 20 = indicator + battery finder, 21 = U8 UART ping
#endif

extern uint8_t DevConfig;
extern volatile uint8_t CdcDTR;
extern uint8_t EP0_Databuf[], EP1_Databuf[], EP2_Databuf[], EP3_Databuf[];
void iap_flag_clear(void);

// Free pins: not key matrix (PA7-PA15, PB4-PB9, PB12-PB17), knob (PB0/1), USB (PB10/11).
// PB20-PB23 are left out: they toggle on their own (likely driven by another chip).
#define NPA 16
static const uint8_t CAND[] = {0, 1, 2, 3, 4, 5, 6, NPA + 2, NPA + 3, NPA + 18, NPA + 19};
#define NCAND (sizeof(CAND) / sizeof(CAND[0]))

static void say(const char *s) {
    if (!DevConfig || !CdcDTR) return;
    uint32_t n = 0; while (s[n]) n++;
    while (n) {
        uint32_t k = n > 64 ? 64 : n;
        for (int t = 0; t < 500 && (R8_UEP2_CTRL & MASK_UEP_T_RES) == UEP_T_RES_ACK; t++) DelayUs(100);
        if ((R8_UEP2_CTRL & MASK_UEP_T_RES) == UEP_T_RES_ACK) return;
        memcpy(pEP2_IN_DataBuf, s, k); DevEP2_IN_Deal(k); s += k; n -= k;
    }
}
static char *pin_name(int i, char *s) {
    int n = i < NPA ? i : i - NPA;
    s[0] = 'P'; s[1] = i < NPA ? 'A' : 'B';
    int k = 2; if (n >= 10) s[k++] = '0' + n / 10; s[k++] = '0' + n % 10; s[k] = 0;
    return s;
}

// ---- WS2812 bit-bang timed with SysTick (counts HCLK = 62.4 MHz -> 16 ns per tick)
static uint32_t t0h = 22, t1h = 50, tbit = 78;  // 0.35 / 0.80 / 1.25 us at 62.4 MHz; phase 5 calibrates
#define T0H t0h
#define T1H t1h
#define TBIT tbit
__HIGH_CODE static void ws_frame(uint32_t ma, uint32_t mb, int nleds, uint8_t level) {
    uint32_t a_hi = R32_PA_OUT | ma, a_lo = R32_PA_OUT & ~ma;
    uint32_t b_hi = R32_PB_OUT | mb, b_lo = R32_PB_OUT & ~mb;
    uint32_t irq;
    SYS_DisableAllIrq(&irq);
    for (int l = 0; l < nleds; l++) {
        for (int c = 0; c < 3; c++) {                 // G, R, B
            for (int b = 7; b >= 0; b--) {
                uint32_t t0 = SysTick->CNTL;
                R32_PA_OUT = a_hi; R32_PB_OUT = b_hi;
                uint32_t th = ((level >> b) & 1) ? T1H : T0H;
                while (SysTick->CNTL - t0 < th) {}
                R32_PA_OUT = a_lo; R32_PB_OUT = b_lo;
                while (SysTick->CNTL - t0 < TBIT) {}
            }
        }
    }
    SYS_RecoverIrq(irq);
}

// Drive style for ws_frame_rgb (phase 6): 0 = push-pull, 1 = inverted (for an inverting
// transistor level shifter: pin idles HIGH, pulses LOW), 2 = open-drain (low / released to input).
static uint8_t ws_mode;

// Same, but with an explicit colour per LED (WS2812 byte order G, R, B).
__HIGH_CODE static void ws_frame_rgb(uint32_t ma, uint32_t mb, int nleds, uint8_t g, uint8_t r, uint8_t bl) {
    volatile uint32_t *pa = &R32_PA_OUT, *pb = &R32_PB_OUT;
    if (ws_mode == 2) { R32_PA_CLR = ma; R32_PB_CLR = mb; pa = &R32_PA_DIR; pb = &R32_PB_DIR; }
    uint32_t a_hi = *pa | ma, a_lo = *pa & ~ma;
    uint32_t b_hi = *pb | mb, b_lo = *pb & ~mb;
    if (ws_mode) { uint32_t t = a_hi; a_hi = a_lo; a_lo = t; t = b_hi; b_hi = b_lo; b_lo = t; }  // OD: DIR=0 is "high"
    uint8_t col[3] = {g, r, bl};
    uint32_t irq;
    SYS_DisableAllIrq(&irq);
    for (int l = 0; l < nleds; l++) {
        for (int c = 0; c < 3; c++) {
            for (int b = 7; b >= 0; b--) {
                uint32_t t0 = SysTick->CNTL;
                *pa = a_hi; *pb = b_hi;
                uint32_t th = ((col[c] >> b) & 1) ? T1H : T0H;
                while (SysTick->CNTL - t0 < th) {}
                *pa = a_lo; *pb = b_lo;
                while (SysTick->CNTL - t0 < TBIT) {}
            }
        }
    }
    SYS_RecoverIrq(irq);
}


// ---- phase 6 helpers: line buffer for say(), and a logic capture of the U8 link pins PB20-PB23
static char ob[600]; static int on;
static void ps(const char *s) { while (*s && on < 590) ob[on++] = *s++; }
static void pn(uint32_t v) { char d[11]; int k = 0; do d[k++] = '0' + v % 10; while (v /= 10); while (k && on < 590) ob[on++] = d[--k]; }
static void ph(uint8_t v) { const char *x = "0123456789abcdef"; if (on < 588) { ob[on++] = x[v >> 4]; ob[on++] = x[v & 15]; ob[on++] = ' '; } }
static void flush(void) { ob[on] = 0; say(ob); on = 0; }

#define NE 3000
static uint32_t et[NE], ev[NE], eva[NE], pt[NE], cap_a0, cap_b0;
static uint8_t cap_keep_irq;   // phase 18: let USB run during a long capture (a few us of jitter)
// Logic capture of the PA/PB pins in ma/mb for `span` SysTick ticks (IRQs off, USB just NAKs).
static int capture(uint32_t ma, uint32_t mb, uint32_t span) {
    uint32_t irq, la = R32_PA_PIN & ma, lb = R32_PB_PIN & mb; int n = 0;
    cap_a0 = la; cap_b0 = lb;
    if (!cap_keep_irq) SYS_DisableAllIrq(&irq);
    uint32_t t0 = SysTick->CNTL;
    while (n < NE && SysTick->CNTL - t0 < span) {
        uint32_t va = R32_PA_PIN & ma, vb = R32_PB_PIN & mb;
        if (va != la || vb != lb) { et[n] = SysTick->CNTL; eva[n] = va; ev[n] = vb; n++; la = va; lb = vb; }
    }
    if (!cap_keep_irq) SYS_RecoverIrq(irq);
    return n;
}
// Pin p (NPA numbering) in the last capture: edge count, start level, shortest pulse, first
// widths (us x10) and an 8N1 decode with bit = `bit` ticks (0: shortest pulse).
// Silent when the pin had no edges and !verbose. Returns the edge count.
static int report_pin(int p, int n, uint32_t tpu, uint32_t bit, int verbose) {
    uint32_t m = 1u << (p < NPA ? p : p - NPA), *val = p < NPA ? eva : ev, prev = p < NPA ? cap_a0 : cap_b0, mn = ~0u;
    int c = 0, idle = (prev & m) != 0;
    for (int i = 0; i < n; i++) { if ((val[i] ^ prev) & m) pt[c++] = et[i]; prev = val[i]; }
    if (!c && !verbose) return 0;
    for (int i = 1; i < c; i++) if (pt[i] - pt[i - 1] < mn) mn = pt[i] - pt[i - 1];
    if (bit) mn = bit;
    char nm[8];
    ps("  "); ps(pin_name(p, nm)); ps(": "); pn(c); ps(" edges, starts "); ps(idle ? "H" : "L");
    if (c > 1) {
        ps(", bit "); pn(mn * 10 / tpu); ps(" us/10 (~"); pn(tpu * 1000000 / mn); ps(" baud)\n   widths:");
        for (int i = 1; i < c && i < 24; i++) { ps(" "); pn((pt[i] - pt[i - 1]) * 10 / tpu); }
        if (idle) {   // UART decode: start bit = falling edge, sample mid-bit
            ps("\n   uart:");
            int j = 0, bytes = 0;
            while (bytes < 40) {
                while (j < c && (j & 1)) j++;   // even-index edges are falling (line starts high)
                if (j >= c) break;
                uint32_t ts = pt[j]; uint8_t b = 0;
                for (int k = 0; k < 8; k++) {
                    uint32_t t = ts + mn * (3 + 2 * k) / 2; int e = j;
                    while (e < c && pt[e] <= t) e++;
                    if (!(e & 1)) b |= 1 << k;   // even count of edges so far -> high
                }
                ph(b); bytes++;
                uint32_t stop = ts + mn * 19 / 2;
                while (j < c && pt[j] <= stop) j++;
            }
        }
    }
    ps("\n"); flush();
    return c;
}
static void sniff(uint32_t tpu) {   // phase 6: 300 ms of PB20-PB23
    GPIOB_ModeCfg(0xFu << 20, GPIO_ModeIN_Floating);
    mDelaymS(2);
    int n = capture(0, 0xFu << 20, 300000 * tpu);
    for (int p = 20; p < 24; p++) report_pin(NPA + p, n, tpu, 0, 1);
    ps("capture: "); pn(n); ps(" edges in 300 ms\n"); flush();
}

// 8N1 UART bit-bang on pin p (left as output, idle high).
__HIGH_CODE static void uart_tx(int p, uint32_t bt, const uint8_t *d, int len) {
    volatile uint32_t *out = p < NPA ? &R32_PA_OUT : &R32_PB_OUT, *dir = p < NPA ? &R32_PA_DIR : &R32_PB_DIR;
    uint32_t m = 1u << (p < NPA ? p : p - NPA), irq;
    *out |= m; *dir |= m;
    DelayUs(200);                                     // idle high before the first start bit
    SYS_DisableAllIrq(&irq);
    uint32_t t = SysTick->CNTL;
    for (int i = 0; i < len; i++) {
        uint32_t f = ((uint32_t)d[i] << 1) | 0x200;   // start 0, 8 data bits LSB first, stop 1
        for (int b = 0; b < 10; b++) {
            if ((f >> b) & 1) *out |= m; else *out &= ~m;
            t += bt;
            while ((int32_t)(SysTick->CNTL - t) < 0) {}
        }
    }
    SYS_RecoverIrq(irq);
}

// Phase 16 sender: PA0, idle low. Bit i is HIGH for h0/h1 ticks, the bit slot is p0/p1 ticks long.
// All timing is against absolute SysTick deadlines and the next bit is prepared while the line is
// low, so the HIGH time is exact (phase 15 computed inside the HIGH time and shortened it).
__HIGH_CODE static void pa0_frame(const uint8_t *grb, int nbytes, uint32_t h0, uint32_t p0, uint32_t h1, uint32_t p1) {
    const uint32_t hi = R32_PA_OUT | 1u, lo = R32_PA_OUT & ~1u;
    uint32_t irq; SYS_DisableAllIrq(&irq);
    uint32_t t = SysTick->CNTL + 64;
    for (int j = 0; j < nbytes; j++) {
        for (int b = 7; b >= 0; b--) {
            uint32_t one = (grb[j] >> b) & 1, h = one ? h1 : h0, p = one ? p1 : p0;
            while ((int32_t)(SysTick->CNTL - t) < 0) {}
            R32_PA_OUT = hi;
            while ((int32_t)(SysTick->CNTL - (t + h)) < 0) {}
            R32_PA_OUT = lo;
            t += p;
        }
    }
    SYS_RecoverIrq(irq);
}

int main(void) {
    HSECFG_Capacitance(HSECap_18p);
    SetSysClock(SYSCLK_FREQ);
    iap_flag_clear();
    SysTick->CTLR = SysTick_CTLR_STCLK | SysTick_CTLR_STE;   // free-running, HCLK

#if LEDFINDER_PHASE == 18
    // Phase 18: phase 17 found no SPI flash on U7's free pins and no backlight pin, so the screen
    // most likely hangs on U8. Keyfinder logs: U8 holds PB20-PB23 LOW for ~20 s after power-up,
    // then HIGH - maybe waiting for U7. Record PB20-23 (+ the held-low PA6, PB2, PB19)
    // for 25 s from reset, then report once the serial log is open.
    // (USB is started first and keeps running, so the PC sees the keyboard right away.)
    pEP0_RAM_Addr = EP0_Databuf; pEP1_RAM_Addr = EP1_Databuf;
    pEP2_RAM_Addr = EP2_Databuf; pEP3_RAM_Addr = EP3_Databuf;
    USB_DeviceInit();
    PFIC_EnableIRQ(USB_IRQn);
    const uint32_t CMA = 1u << 6, CMB = (0xFu << 20) | (1u << 2) | (1u << 19);
    uint32_t tpu18 = GetSysClock() / 1000000;
    cap_keep_irq = 1;
    int n18 = capture(CMA, CMB, 25000000u * tpu18);
    cap_keep_irq = 0;
    uint32_t t18 = SysTick->CNTL;                       // capture end (for relative times)
#else
    pEP0_RAM_Addr = EP0_Databuf; pEP1_RAM_Addr = EP1_Databuf;
    pEP2_RAM_Addr = EP2_Databuf; pEP3_RAM_Addr = EP3_Databuf;
    USB_DeviceInit();
    PFIC_EnableIRQ(USB_IRQn);
#endif

    uint32_t all_a = 0, all_b = 0;
    for (unsigned i = 0; i < NCAND; i++) {
        if (CAND[i] < NPA) all_a |= 1u << CAND[i]; else all_b |= 1u << (CAND[i] - NPA);
    }
    while (!CdcDTR) mDelaymS(10);                    // wait until the PC opens the serial port
    mDelaymS(300);
    say("ledfinder ready - watch the keyboard LEDs\n");

#if LEDFINDER_PHASE == 25
    // Phase 25: talk to the U8 bootloader (firmware/u8/u8boot.c) over the one-wire link:
    // sync 7F (expect 79) and ping 5A 'P' 0 0 0 0 sum (expect 79 42 01). Same open-drain TX as 24.
    {
        uint32_t t0 = SysTick->CNTL; mDelaymS(100); uint32_t tpu = (SysTick->CNTL - t0) / 100000;
        if (!tpu) tpu = 1;
        const uint32_t M = 0xFu << 20, txm = 1u << 22, bit = GetSysClock() / 115200;
        GPIOB_SetBits(M); GPIOB_ModeCfg(M, GPIO_ModeIN_PU); R32_PB_CLR = txm;
        static const uint8_t SYNC[] = {0x7F}, PING[] = {0x5A, 'P', 0, 0, 0, 0, 'P'};
        for (uint32_t n = 0;; n++) {
            const uint8_t *f = (n & 1) ? PING : SYNC; int fl = (n & 1) ? sizeof(PING) : sizeof(SYNC);
            uint32_t irq; SYS_DisableAllIrq(&irq);
            uint32_t t = SysTick->CNTL;
            for (int i = 0; i < fl; i++) {
                uint32_t w = 0x200u | ((uint32_t)f[i] << 1);
                for (int k = 0; k < 10; k++) {
                    if (w & (1u << k)) R32_PB_DIR &= ~txm; else R32_PB_DIR |= txm;
                    t += bit; while ((int32_t)(SysTick->CNTL - t) < 0);
                }
            }
            R32_PB_DIR &= ~txm;
            SYS_RecoverIrq(irq);
            int e = capture(0, 1u << 21, 4000 * tpu);
            ps((n & 1) ? "ping ->" : "sync ->");
            if (!e) { ps(" nothing\n"); flush(); }
            else { flush(); report_pin(NPA + 21, e, tpu, bit, 1); }
            mDelaymS(500);
        }
    }
#endif
#if LEDFINDER_PHASE == 24
    // Phase 24: one-wire link test. U8 runs research/u8-probe/echo.c (Pico): it answers every byte
    // on PD6 with byte ^ 0xA5. Here: PB21-23 (shorted) pull-up inputs, send byte n on PB22 as
    // open-drain 115200 8N1, then listen 3 ms and decode. "ok" = U8 answered correctly.
    {
        uint32_t t0 = SysTick->CNTL; mDelaymS(100); uint32_t tpu = (SysTick->CNTL - t0) / 100000;
        if (!tpu) tpu = 1;
        const uint32_t M = 0xFu << 20, txm = 1u << 22, bit = GetSysClock() / 115200;
        GPIOB_SetBits(M); GPIOB_ModeCfg(M, GPIO_ModeIN_PU); R32_PB_CLR = txm;   // latch low, pin input
        uint32_t okc = 0;
        for (uint32_t n = 0;; n++) {
            uint8_t b = n & 0xFF;
            uint32_t irq; SYS_DisableAllIrq(&irq);
            uint32_t w = 0x200u | ((uint32_t)b << 1), t = SysTick->CNTL;
            for (int k = 0; k < 10; k++) {
                if (w & (1u << k)) R32_PB_DIR &= ~txm; else R32_PB_DIR |= txm;
                t += bit; while ((int32_t)(SysTick->CNTL - t) < 0);
            }
            R32_PB_DIR &= ~txm;
            SYS_RecoverIrq(irq);
            int e = capture(0, 1u << 21, 3000 * tpu);
            // decode one 8N1 byte from the captured edges of PB21
            int got = -1;
            if (e) {
                uint32_t ts = et[0], prev = cap_b0 & (1u << 21); uint8_t v = 0; int j = 0;
                for (int k = 0; k < 8; k++) {
                    uint32_t at = ts + bit * (3 + 2 * k) / 2;
                    while (j < e && et[j] <= at) { prev = ev[j] & (1u << 21); j++; }
                    if (prev) v |= 1 << k;
                }
                got = v;
            }
            int ok = got == (b ^ 0xA5);
            okc += ok;
            ps("sent "); ph(b); ps("-> "); if (got < 0) ps("nothing"); else ph(got);
            ps(ok ? " ok" : " BAD"); ps(" ("); pn(okc); ps("/"); pn(n + 1); ps(" ok)\n"); flush();
            mDelaymS(300);
        }
    }
#endif
#if LEDFINDER_PHASE == 23
    // Phase 23: phase 22 saw every U8 burst on all of PB20-PB23 -> are they shorted together?
    // Drive one pin (push-pull) HIGH with the others pull-down, then LOW with the others pull-up,
    // and print what the others read. "follows" on both = connected (bridge / same net).
    {
        const uint32_t M = 0xFu << 20;
        for (uint32_t round = 1;; round++) {
            ps("--- round "); pn(round); ps("\n"); flush();
            for (int d = 20; d < 24; d++) {
                char nm[8];
                uint32_t dm = 1u << d, om = M & ~dm;
                GPIOB_ModeCfg(om, GPIO_ModeIN_PD); GPIOB_SetBits(dm); GPIOB_ModeCfg(dm, GPIO_ModeOut_PP_5mA);
                mDelaymS(5); uint32_t hi = R32_PB_PIN & om;
                GPIOB_ModeCfg(om, GPIO_ModeIN_PU); GPIOB_ResetBits(dm);
                mDelaymS(5); uint32_t lo = R32_PB_PIN & om;
                GPIOB_ModeCfg(dm, GPIO_ModeIN_PD);
                ps("  drive "); ps(pin_name(NPA + d, nm)); ps(":");
                for (int o = 20; o < 24; o++) if (o != d) {
                    int fh = (hi >> o) & 1, fl = !((lo >> o) & 1);
                    ps(" "); ps(pin_name(NPA + o, nm)); ps(fh && fl ? "=follows" : fh ? "=high-only" : fl ? "=low-only" : "=no");
                }
                ps("\n"); flush();
            }
            GPIOB_ModeCfg(M, GPIO_ModeIN_PD);
            mDelaymS(2000);
        }
    }
#endif
#if LEDFINDER_PHASE == 22
    // Phase 22: map the U7<->U8 link. U8 runs research/u8-probe/linkscan.c (loaded with the Pico):
    // each free U8 pin sends a burst of (index + 1) pulses of 1 ms. Here PB20-PB23 are pull-down
    // inputs; every 3 s print the pulse count of each burst seen per pin: "PB22: 30" = U8 index 29.
    {
        uint32_t t0 = SysTick->CNTL; mDelaymS(100); uint32_t tpu = (SysTick->CNTL - t0) / 100000;
        if (!tpu) tpu = 1;
        const uint32_t M = 0xFu << 20;
        GPIOB_ModeCfg(M, GPIO_ModeIN_PD);
        cap_keep_irq = 1;
        for (uint32_t round = 1;; round++) {
            int n = capture(0, M, 3000000u * tpu);
            ps("--- round "); pn(round); ps(" ("); pn(n); ps(" edges)\n"); flush();
            for (int p = 20; p < 24; p++) {
                uint32_t m = 1u << p, prev = cap_b0 & m, last = 0; int cnt = 0, any = 0;
                char nm[8];
                ps("  "); ps(pin_name(NPA + p, nm)); ps(":");
                for (int i = 0; i < n; i++) {
                    uint32_t v = ev[i] & m;
                    if (v == prev) continue;
                    prev = v;
                    if (!v) continue;                                   // count rising edges
                    if (cnt && et[i] - last > 20000 * tpu) { ps(" "); pn(cnt); cnt = 0; }
                    cnt++; last = et[i]; any = 1;
                }
                if (cnt) { ps(" "); pn(cnt); }
                if (!any) ps(" -");
                ps("\n"); flush();
            }
        }
    }
#endif
#if LEDFINDER_PHASE == 21
    // Phase 21: is U8 (CH32V006, probably the screen chip) waiting for commands from U7?
    // HFD's other AK820 boards talk MCU<->module as [CMD, data.., SUM] 8N1 at 115200. Send probe
    // frames as software UART on each PB20-PB23 pin (open-drain: low = drive, high = pull-up, so
    // we never fight U8 if it drives that pin), at 13 baud rates, then listen 30 ms on all four.
    {
        uint32_t t0 = SysTick->CNTL; mDelaymS(100); uint32_t tpu = (SysTick->CNTL - t0) / 100000;
        if (!tpu) tpu = 1;
        const uint32_t M = 0xFu << 20;
        static const uint32_t BAUD[] = {9600, 19200, 38400, 57600, 76800, 115200, 230400, 250000,
                                        460800, 500000, 921600, 1000000, 2000000};
        static const uint8_t PROBE[] = {
            0xA6, 0x53, 0xF9,  0x61, 0x0D, 0x0A,  0xA6, 0x31, 0xD7,       // HFD module frames
            0x55, 0xAA, 0x01, 0x00, 0x00, 0x01,  0xAA, 0x55, 0x10, 0x00, 0x10,
            0x5A, 0xA5, 0x00, 0x01, 0x01,  0x04, 0x18, 0x00,  0x7F, 0x00, 0xFF, 0x01, 0x80};
        for (uint32_t round = 1;; round++) {
            int answers = 0;
            ps("--- round "); pn(round); ps(": UART ping PB20-23\n"); flush();
            for (int tx = 20; tx < 24; tx++) for (unsigned b = 0; b < sizeof(BAUD) / sizeof(BAUD[0]); b++) {
                uint32_t bit = GetSysClock() / BAUD[b], txm = 1u << tx, irq;
                GPIOB_ModeCfg(M, GPIO_ModeIN_PU); R32_PB_CLR = txm;   // output latch low, pin still input
                mDelaymS(5);
                SYS_DisableAllIrq(&irq);
                uint32_t t = SysTick->CNTL;
                for (unsigned i = 0; i < sizeof(PROBE); i++) {
                    uint32_t w = 0x200u | ((uint32_t)PROBE[i] << 1);   // start 0, 8 data LSB first, stop 1
                    for (int k = 0; k < 10; k++) {
                        if (w & (1u << k)) R32_PB_DIR &= ~txm; else R32_PB_DIR |= txm;
                        t += bit; while ((int32_t)(SysTick->CNTL - t) < 0);
                    }
                    t += bit; while ((int32_t)(SysTick->CNTL - t) < 0);   // 2nd stop bit gap
                }
                R32_PB_DIR &= ~txm;
                SYS_RecoverIrq(irq);
                int n = capture(0, M, 30000 * tpu);
                if (n) {
                    char nm[8];
                    answers++;
                    ps("ANSWER after TX on "); ps(pin_name(NPA + tx, nm)); ps(" @ "); pn(BAUD[b]); ps(" baud:\n"); flush();
                    for (int p = 20; p < 24; p++) report_pin(NPA + p, n, tpu, 0, 0);
                }
            }
            ps("round "); pn(round); ps(" done: "); pn(answers); ps(" answers (0 = U8 ignores all of it)\n"); flush();
            mDelaymS(2000);
        }
    }
#endif
#if LEDFINDER_PHASE == 20
    // Phase 20: (1) battery: ADC on the free analog pins PA1(A8) PA2(A7) PA3(A6) PA4(A0) PA5(A1)
    // PA6(A10), 64 samples each, -12 dB gain (range up to VDD). A divider from the battery reads
    // steady; an unused pin drifts. (2) LED walk: chain LEDs 0..84 lit one at a time (white),
    // to find where the C / W / battery indicators sit in the chain.
    {
        uint32_t t0 = SysTick->CNTL; mDelaymS(100); uint32_t tpu = (SysTick->CNTL - t0) / 100000;
        if (!tpu) tpu = 1;
        GPIOA_ModeCfg(all_a, GPIO_ModeIN_PU); GPIOB_ModeCfg(all_b | (0xFu << 20), GPIO_ModeIN_PU);
        R32_PA_CLR = 1u; R32_PA_DIR |= 1u;                 // PA0 = LED chain, idle low
        static const uint8_t AD[][2] = {{1, 8}, {2, 7}, {3, 6}, {4, 0}, {5, 1}, {6, 10}};   // pin, channel
        static uint8_t grb[120 * 3];
        const uint32_t h0 = 100 * tpu / 1000, h1 = 1000 * tpu / 1000, p = 2500 * tpu / 1000;
        for (uint32_t round = 1;; round++) {
            ps("--- round "); pn(round); ps(": ADC raw (0-4095, -12 dB) min/avg/max\n"); flush();
            ADC_ExtSingleChSampInit(SampleFreq_8_or_4, ADC_PGA_1_4);
            for (unsigned i = 0; i < sizeof(AD) / sizeof(AD[0]); i++) {
                char nm[8];
                GPIOA_ModeCfg(1u << AD[i][0], GPIO_ModeIN_Floating);
                ADC_ChannelCfg(AD[i][1]);
                mDelaymS(2);
                uint32_t mn = 4095, mx = 0, sum = 0;
                for (int k = 0; k < 64; k++) { uint32_t v = ADC_ExcutSingleConver(); sum += v; if (v < mn) mn = v; if (v > mx) mx = v; DelayUs(200); }
                GPIOA_ModeCfg(1u << AD[i][0], GPIO_ModeIN_PU);
                ps("  "); ps(pin_name(AD[i][0], nm)); ps(": "); pn(mn); ps(" / "); pn(sum / 64); ps(" / "); pn(mx); ps("\n"); flush();
            }
            say("LED walk: tell me the number where the BATTERY ICON lights up\n");
            for (int l = 0; l < 85; l++) {
                for (int j = 0; j < 120 * 3; j++) grb[j] = 0;
                grb[l * 3] = grb[l * 3 + 1] = grb[l * 3 + 2] = 0x30;
                pa0_frame(grb, 120 * 3, h0, p, h1, p);
                ps("LED "); pn(l); ps("\n"); flush();
                mDelaymS(700);
            }
        }
    }
#endif
#if LEDFINDER_PHASE == 19
    // Phase 19: wider screen search (phase 17 found nothing, phase 18: U8 silent from power-up).
    // (1) flash JEDEC search with: CS on any free pin OR none (CS tied low on the board), MISO also
    //     on the held-low pins (a selected flash drives SO), and SPI0's own pins on the key matrix
    //     (PA13/14/15 + PA12, remap PB13/14/15 + PB12) in case the bus is shared with the matrix.
    // (2) backlight: every free pin (not PA0 = LEDs) HIGH 3 s, then LOW 3 s - watch the SCREEN.
    {
        static const uint8_t FR[] = {1, 2, 3, 4, 5, 6, NPA + 2, NPA + 3, NPA + 18, NPA + 19};
        static const uint8_t SPI0[2][4] = {{12, 13, 14, 15}, {NPA + 12, NPA + 13, NPA + 14, NPA + 15}};   // CS SCK MOSI MISO
        #define PM(p) (1u << ((p) < NPA ? (p) : (p) - NPA))
        #define PSET(p, v) do { if ((p) < NPA) { if (v) R32_PA_OUT |= PM(p); else R32_PA_CLR = PM(p); } \
                                else { if (v) R32_PB_OUT |= PM(p); else R32_PB_CLR = PM(p); } } while (0)
        #define PGET(p) (((p) < NPA ? R32_PA_PIN : R32_PB_PIN) & PM(p) ? 1 : 0)
        #define POUT(p) do { if ((p) < NPA) R32_PA_DIR |= PM(p); else R32_PB_DIR |= PM(p); } while (0)
        uint32_t ma = 0xF000u, mb = 0xF000u;                       // SPI0 pins on the matrix
        for (unsigned i = 0; i < sizeof(FR); i++) { if (FR[i] < NPA) ma |= PM(FR[i]); else mb |= PM(FR[i]); }
        int found = 0;
        // one try: cs < 0 = no CS line; prints the combo if the ID is not all 00 / FF
        #define TRY(cs, sck, mosi, miso) do { \
            GPIOA_ModeCfg(ma, GPIO_ModeIN_PU); GPIOB_ModeCfg(mb, GPIO_ModeIN_PU); \
            if ((cs) >= 0) { PSET(cs, 1); POUT(cs); } PSET(sck, 0); PSET(mosi, 0); POUT(sck); POUT(mosi); \
            DelayUs(5); uint8_t id_[3] = {0, 0, 0}; \
            for (int pass_ = 0; pass_ < 2; pass_++) { \
                if ((cs) >= 0) PSET(cs, 0); DelayUs(1); \
                for (int n_ = 0; n_ < (pass_ ? 4 : 1); n_++) { \
                    uint8_t out_ = n_ ? 0 : (pass_ ? 0x9F : 0xAB), in_ = 0; \
                    for (int b_ = 7; b_ >= 0; b_--) { PSET(mosi, (out_ >> b_) & 1); DelayUs(1); PSET(sck, 1); DelayUs(1); \
                        in_ |= PGET(miso) << b_; PSET(sck, 0); } \
                    if (pass_ && n_) id_[n_ - 1] = in_; } \
                if ((cs) >= 0) PSET(cs, 1); DelayUs(50); } \
            if (!((id_[0] & id_[1] & id_[2]) == 0xFF || (id_[0] | id_[1] | id_[2]) == 0) && found < 30) { char nm_[8]; found++; \
                ps("  FLASH? CS="); ps((cs) >= 0 ? pin_name(cs, nm_) : "none"); ps(" SCK="); ps(pin_name(sck, nm_)); \
                ps(" MOSI="); ps(pin_name(mosi, nm_)); ps(" MISO="); ps(pin_name(miso, nm_)); \
                ps(" id "); ph(id_[0]); ph(id_[1]); ph(id_[2]); ps("\n"); flush(); } } while (0)
        for (uint32_t round = 1;; round++) {
            found = 0;
            ps("--- round "); pn(round); ps(": flash search (free pins, CS or none)\n"); flush();
            const int N = sizeof(FR);
            for (int k = 0; k < N; k++) for (int o = 0; o < N; o++) for (int i = 0; i < N; i++) {
                if (k == o || k == i || o == i) continue;
                TRY(-1, FR[k], FR[o], FR[i]);
                for (int c = 0; c < N; c++) if (c != k && c != o && c != i) TRY(FR[c], FR[k], FR[o], FR[i]);
            }
            ps("  flash search: SPI0 pins on the key matrix\n"); flush();
            for (int v = 0; v < 2; v++) {
                TRY(SPI0[v][0], SPI0[v][1], SPI0[v][2], SPI0[v][3]);
                TRY(-1, SPI0[v][1], SPI0[v][2], SPI0[v][3]);
                for (int c = 0; c < N; c++) TRY(FR[c], SPI0[v][1], SPI0[v][2], SPI0[v][3]);
            }
            GPIOA_ModeCfg(ma, GPIO_ModeIN_PU); GPIOB_ModeCfg(mb, GPIO_ModeIN_PU);
            ps("  search done, "); pn(found); ps(" answers\n"); flush();
            unsigned s = 0;
            for (int i = 0; i < N; i++) for (int lvl = 1; lvl >= 0; lvl--) {
                char nm[8];
                ps("state "); pn(++s); ps(": "); ps(pin_name(FR[i], nm)); ps(lvl ? " HIGH" : " LOW"); ps(" - is the SCREEN lit?\n"); flush();
                PSET(FR[i], lvl); POUT(FR[i]);
                mDelaymS(3000);
                if (FR[i] < NPA) R32_PA_DIR &= ~PM(FR[i]); else R32_PB_DIR &= ~PM(FR[i]);
                mDelaymS(500);
            }
        }
    }
#endif
#if LEDFINDER_PHASE == 18
    {
        static const uint8_t PIN[] = {NPA + 20, NPA + 21, NPA + 22, NPA + 23, 6, NPA + 2, NPA + 19};
        uint32_t start = t18 - 25000000u * tpu18;     // capture start
        char nm[8];
        ps("power-up capture: "); pn(n18); ps(n18 >= NE ? " edges (buffer FULL)\n" : " edges in 25 s\n"); flush();
        ps("levels at start: PA6="); pn(!!(cap_a0 & (1u << 6)));
        for (int q = 20; q < 24; q++) { ps(" PB"); pn(q); ps("="); pn(!!(cap_b0 & (1u << q))); }
        ps(" PB2="); pn(!!(cap_b0 & 4)); ps(" PB19="); pn(!!(cap_b0 & (1u << 19))); ps("\n"); flush();
        uint32_t pa = cap_a0, pb = cap_b0;
        for (int i = 0; i < n18 && i < 80; i++) {     // timeline of the first 80 changes
            ps("  t="); pn((et[i] - start) / (tpu18 * 1000)); ps(" ms:");
            for (unsigned q = 0; q < sizeof(PIN); q++) {
                int p = PIN[q]; uint32_t m = 1u << (p < NPA ? p : p - NPA);
                uint32_t was = (p < NPA ? pa : pb) & m, now = (p < NPA ? eva[i] : ev[i]) & m;
                if (was != now) { ps(" "); ps(pin_name(p, nm)); ps(now ? " hi" : " lo"); }
            }
            ps("\n"); flush();
            pa = eva[i]; pb = ev[i];
        }
        for (unsigned q = 0; q < sizeof(PIN); q++) report_pin(PIN[q], n18, tpu18, 0, 1);
        say("done - unplug when you have the log\n");
        for (;;) mDelaymS(1000);
    }
#endif
#if LEDFINDER_PHASE == 17
    // Phase 17: screen finder. The board has a GC9107 LCD and a PUYA SPI flash (U9, screen
    // pictures). CH584's only SPI (SPI0) sits on key-matrix pins, so LCD + flash hang either on
    // U7's free pins (bit-banged) or on U8. Free pins: PA1-PA5, PB3, PB18 float (pu/pd follow the
    // pull = chip inputs or nothing); PA6, PB2, PB19 are held low (like PA0 = transistor base?).
    // (1) automatic: every (CS, SCK, MOSI, MISO) among the 7 floating pins: 0xAB (wake) + 0x9F
    //     (JEDEC ID); print answers that are not all 00 / FF.
    // (2) backlight: each held-low pin HIGH for 4 s - watch the screen.
    {
        static const uint8_t F[] = {1, 2, 3, 4, 5, NPA + 3, NPA + 18}, H[] = {6, NPA + 2, NPA + 19};
        #define PM(p) (1u << ((p) < NPA ? (p) : (p) - NPA))
        #define PSET(p, v) do { if ((p) < NPA) { if (v) R32_PA_OUT |= PM(p); else R32_PA_CLR = PM(p); } \
                                else { if (v) R32_PB_OUT |= PM(p); else R32_PB_CLR = PM(p); } } while (0)
        #define PGET(p) (((p) < NPA ? R32_PA_PIN : R32_PB_PIN) & PM(p) ? 1 : 0)
        #define POUT(p) do { if ((p) < NPA) R32_PA_DIR |= PM(p); else R32_PB_DIR |= PM(p); } while (0)
        uint32_t fa = 0, fb = 0;
        for (unsigned i = 0; i < sizeof(F); i++) { if (F[i] < NPA) fa |= PM(F[i]); else fb |= PM(F[i]); }
        for (uint32_t round = 1;; round++) {
            ps("--- round "); pn(round); ps(": SPI flash search\n"); flush();
            int found = 0;
            for (unsigned c = 0; c < sizeof(F); c++) for (unsigned k = 0; k < sizeof(F); k++)
            for (unsigned o = 0; o < sizeof(F); o++) for (unsigned i = 0; i < sizeof(F); i++) {
                if (c == k || c == o || c == i || k == o || k == i || o == i) continue;
                int cs = F[c], sck = F[k], mosi = F[o], miso = F[i];
                GPIOA_ModeCfg(fa, GPIO_ModeIN_PU); GPIOB_ModeCfg(fb, GPIO_ModeIN_PU);   // WP/HOLD on a GPIO stay high
                PSET(cs, 1); PSET(sck, 0); PSET(mosi, 0); POUT(cs); POUT(sck); POUT(mosi);
                DelayUs(5);
                uint8_t id[3] = {0, 0, 0};
                for (int pass = 0; pass < 2; pass++) {
                    static const uint8_t CMD[2] = {0xAB, 0x9F};
                    PSET(cs, 0); DelayUs(1);
                    for (int n = 0; n < (pass ? 4 : 1); n++) {
                        uint8_t out = n ? 0 : CMD[pass], in = 0;
                        for (int b = 7; b >= 0; b--) {
                            PSET(mosi, (out >> b) & 1); DelayUs(1);
                            PSET(sck, 1); DelayUs(1);
                            in |= PGET(miso) << b;
                            PSET(sck, 0);
                        }
                        if (pass && n) id[n - 1] = in;
                    }
                    PSET(cs, 1); DelayUs(pass ? 1 : 50);
                }
                if ((id[0] == 0xFF && id[1] == 0xFF && id[2] == 0xFF) || (id[0] | id[1] | id[2]) == 0) continue;
                char nm[8];
                ps("  FLASH? CS="); ps(pin_name(cs, nm)); ps(" SCK="); ps(pin_name(sck, nm));
                ps(" MOSI="); ps(pin_name(mosi, nm)); ps(" MISO="); ps(pin_name(miso, nm));
                ps(" id "); ph(id[0]); ph(id[1]); ph(id[2]); ps("\n"); flush();
                if (++found > 20) break;
            }
            GPIOA_ModeCfg(fa, GPIO_ModeIN_PU); GPIOB_ModeCfg(fb, GPIO_ModeIN_PU);
            ps("  search done, "); pn(found); ps(" answers\n"); flush();
            for (unsigned i = 0; i < sizeof(H); i++) {
                char nm[8];
                ps("state "); pn(i + 1); ps(": "); ps(pin_name(H[i], nm)); ps(" HIGH - is the SCREEN lit?\n"); flush();
                PSET(H[i], 1); POUT(H[i]);
                mDelaymS(4000);
                PSET(H[i], 0); mDelaymS(1000);
                if (H[i] < NPA) R32_PA_DIR &= ~PM(H[i]); else R32_PB_DIR &= ~PM(H[i]);
            }
        }
    }
#endif
#if LEDFINDER_PHASE == 16
    // Phase 16: phase 15 went all dark - its loop computed each bit INSIDE the high time, so the
    // pulses came out shorter than set. Now pa0_frame() (RAM, absolute deadlines). Per round:
    // state 1 = 24 ones (Esc white), state 2 = 24 zeros (Esc off), then per 0-pulse length:
    // Esc RED / F1 GREEN / F2 BLUE, and all keys R,G,B repeating. 1 = high 1000 ns, slot 2.5 us.
    {
        uint32_t t0 = SysTick->CNTL; mDelaymS(100); uint32_t tpu = (SysTick->CNTL - t0) / 100000;
        if (!tpu) tpu = 1;
        GPIOA_ModeCfg(all_a, GPIO_ModeIN_PU); GPIOB_ModeCfg(all_b | (0xFu << 20), GPIO_ModeIN_PU);
        R32_PA_CLR = 1u; R32_PA_DIR |= 1u;               // PA0 idle LOW
        static uint8_t grb[120 * 3];
        static const uint16_t H0[] = {100, 60, 150, 200};
        const uint32_t h1 = 1000 * tpu / 1000, p = 2500 * tpu / 1000;
        for (;;) {
            static const uint8_t ONES[3] = {0xFF, 0xFF, 0xFF}, ZEROS[3] = {0, 0, 0};
            say("state 1: 24 ones -> Esc WHITE\n");
            pa0_frame(ONES, 3, 100 * tpu / 1000, p, h1, p); mDelaymS(2500);
            say("state 2: 24 zeros (100 ns) -> Esc OFF\n");
            pa0_frame(ZEROS, 3, 100 * tpu / 1000, p, h1, p); mDelaymS(2500);
            unsigned s = 3;
            for (unsigned k = 0; k < sizeof(H0) / sizeof(H0[0]); k++) {
                for (int v = 0; v < 2; v++, s++) {
                    for (int j = 0; j < 120 * 3; j++) grb[j] = 0;
                    if (v == 0) { grb[1] = 0x40; grb[3] = 0x40; grb[8] = 0x40; }   // Esc R, F1 G, F2 B (G,R,B order)
                    else for (int l = 0; l < 120; l++) grb[l * 3 + (l % 3 == 0 ? 1 : l % 3 == 1 ? 0 : 2)] = 0x30;
                    ps("state "); pn(s); ps(": 0-pulse "); pn(H0[k]); ps(" ns - ");
                    ps(v == 0 ? "Esc RED, F1 GREEN, F2 BLUE, rest OFF\n" : "ALL keys red/green/blue repeating\n"); flush();
                    for (int f = 0; f < 3; f++) { pa0_frame(grb, 120 * 3, H0[k] * tpu / 1000, p, h1, p); mDelaymS(1000); }
                }
            }
            say("round done - starting again\n");
        }
    }
#endif
#if LEDFINDER_PHASE == 15
    // Phase 15: phase 14 state 1 (high 100 ns, low 1150 ns, SysTick-timed plain stores) turned Esc
    // OFF = a 0; 200 ns and longer = 1 (something stretches pulses ~+400 ns). Now real frames:
    // 0 = high h0 / low 1150 ns, 1 = high 600 / low 650 ns. Esc RED, F1 GREEN, F2 BLUE, rest off.
    {
        uint32_t t0 = SysTick->CNTL; mDelaymS(100); uint32_t tpu = (SysTick->CNTL - t0) / 100000;
        if (!tpu) tpu = 1;
        GPIOA_ModeCfg(all_a, GPIO_ModeIN_PU); GPIOB_ModeCfg(all_b | (0xFu << 20), GPIO_ModeIN_PU);
        const uint32_t m = 1u << 0;
        R32_PA_CLR = m; R32_PA_DIR |= m;                 // idle LOW
        const uint32_t hi = R32_PA_OUT | m, lo = R32_PA_OUT & ~m;
        static uint8_t grb[120 * 3];
        static const uint16_t H0[] = {100, 70, 130};
        for (;;) {
            for (unsigned k = 0; k < sizeof(H0) / sizeof(H0[0]); k++) {
                for (int v = 0; v < 2; v++) {
                    for (int j = 0; j < 120 * 3; j++) grb[j] = 0;
                    if (v == 0) { grb[1] = 0x40; grb[3] = 0x40; grb[8] = 0x40; }   // Esc R, F1 G, F2 B
                    else for (int l = 0; l < 120; l++) grb[l * 3 + (l % 3 == 0 ? 1 : l % 3 == 1 ? 0 : 2)] = 0x30; // R,G,B repeating
                    uint32_t h0 = H0[k] * tpu / 1000, g0 = 1150 * tpu / 1000, h1 = 600 * tpu / 1000, g1 = 650 * tpu / 1000;
                    ps("state "); pn(k * 2 + v + 1); ps(": 0-pulse "); pn(H0[k]); ps(" ns - ");
                    ps(v == 0 ? "Esc RED, F1 GREEN, F2 BLUE, rest OFF\n" : "ALL keys red/green/blue repeating\n"); flush();
                    for (int f = 0; f < 3; f++) {
                        uint32_t irq; SYS_DisableAllIrq(&irq);
                        for (int j = 0; j < 120 * 3; j++) for (int b = 7; b >= 0; b--) {
                            uint32_t ts = SysTick->CNTL, one = (grb[j] >> b) & 1, h = one ? h1 : h0, g = one ? g1 : g0;
                            R32_PA_OUT = hi; while (SysTick->CNTL - ts < h) {}
                            R32_PA_OUT = lo; while (SysTick->CNTL - ts < h + g) {}
                        }
                        SYS_RecoverIrq(irq);
                        mDelaymS(1000);
                    }
                }
            }
            say("round done - starting again\n");
        }
    }
#endif
#if LEDFINDER_PHASE == 14
    // Phase 14: phase 13 CONFIRMED a chain on PA0 starting at Esc: 24 long pulses -> only Esc white,
    // 48 -> Esc + F1. But no 0-pulse (400..1000 ns, 2.6 us period) ever read as 0. Per state:
    // make Esc white (24 long pulses), then send 24 pulses of ONE shape (high h, then low g).
    // Esc goes DARK = that shape is a 0. Stays white = it is a 1 or never arrives.
    {
        uint32_t t0 = SysTick->CNTL; mDelaymS(100); uint32_t tpu = (SysTick->CNTL - t0) / 100000;
        if (!tpu) tpu = 1;
        GPIOA_ModeCfg(all_a, GPIO_ModeIN_PU); GPIOB_ModeCfg(all_b | (0xFu << 20), GPIO_ModeIN_PU);
        const uint32_t m = 1u << 0;
        R32_PA_CLR = m; R32_PA_DIR |= m;                 // idle LOW
        const uint32_t hi = R32_PA_OUT | m, lo = R32_PA_OUT & ~m;
        static const uint16_t SH[][2] = {   // {high ns, low ns}
            {100, 1150}, {200, 1050}, {300, 950}, {400, 850}, {500, 750}, {600, 650},
            {800, 450}, {1000, 250}, {1200, 150}, {1500, 80}, {300, 300}, {300, 3000},
        };
        #define PULSES(n, h, g) do { uint32_t irq_; SYS_DisableAllIrq(&irq_); \
            for (int i_ = 0; i_ < (n); i_++) { uint32_t ts_ = SysTick->CNTL; \
                R32_PA_OUT = hi; while (SysTick->CNTL - ts_ < (h)) {} \
                R32_PA_OUT = lo; while (SysTick->CNTL - ts_ < (h) + (g)) {} } \
            SYS_RecoverIrq(irq_); } while (0)
        for (;;) {
            for (unsigned k = 0; k < sizeof(SH) / sizeof(SH[0]); k++) {
                PULSES(24, 2 * tpu, 2 * tpu);            // Esc white
                mDelaymS(1500);
                ps("state "); pn(k + 1); ps(": 24 pulses high "); pn(SH[k][0]); ps(" ns / low "); pn(SH[k][1]); ps(" ns - Esc dark = this is a 0\n"); flush();
                PULSES(24, SH[k][0] * tpu / 1000, SH[k][1] * tpu / 1000);
                mDelaymS(3000);
            }
            say("round done - starting again\n");
        }
    }
#endif
#if LEDFINDER_PHASE == 13
    // Phase 13: phase 9's last PA0 burst = 47 high pulses, each >= 0.5 us -> Esc white + F1 yellow:
    // every pulse that ARRIVES counts as a 1. Phases 10-12 used 0-pulses <= 350 ns: if those are
    // too short to get through the buffer they vanish and only the 1s arrive = all white.
    // So: (1) count test - exactly 24, then 48 long pulses on a dark chain (expect Esc, then
    // Esc+F1 white only); (2) sweep the 0-pulse 400..1000 ns with 1-pulse 1.6 us, 2.6 us period.
    {
        uint32_t t0 = SysTick->CNTL; mDelaymS(100); uint32_t tpu = (SysTick->CNTL - t0) / 100000;
        if (!tpu) tpu = 1;
        GPIOA_ModeCfg(all_a, GPIO_ModeIN_PU); GPIOB_ModeCfg(all_b | (0xFu << 20), GPIO_ModeIN_PU);
        const uint32_t m = 1u << 0;
        R32_PA_CLR = m; R32_PA_DIR |= m;                 // idle LOW, as in phase 9
        uint32_t hi = R32_PA_OUT | m, lo = R32_PA_OUT & ~m;
        static uint8_t grb[120 * 3];
        grb[1] = 0x40; grb[3] = 0x40; grb[8] = 0x40;    // Esc red, F1 green, F2 blue (G,R,B order)
        for (int npulse = 24; npulse <= 48; npulse += 24) {
            ps("state "); pn(npulse / 24); ps(": exactly "); pn(npulse); ps(" long pulses (expect "); ps(npulse == 24 ? "ONLY Esc white)\n" : "Esc + F1 white)\n"); flush();
            uint32_t irq; SYS_DisableAllIrq(&irq);
            for (int i = 0; i < npulse; i++) {
                uint32_t ts = SysTick->CNTL;
                R32_PA_OUT = hi; while (SysTick->CNTL - ts < 2 * tpu) {}
                R32_PA_OUT = lo; while (SysTick->CNTL - ts < 4 * tpu) {}
            }
            SYS_RecoverIrq(irq);
            mDelaymS(5000);
        }
        for (;;) {
            for (uint32_t h0n = 400, s = 3; h0n <= 1000; h0n += 50, s++) {
                uint32_t h0 = h0n * tpu / 1000, h1 = 1600 * tpu / 1000, tb = 2600 * tpu / 1000;
                ps("state "); pn(s); ps(": 0-pulse "); pn(h0n); ps(" ns, 1-pulse 1600 ns\n"); flush();
                for (int f = 0; f < 30; f++) {
                    uint32_t irq; SYS_DisableAllIrq(&irq);
                    for (int j = 0; j < 120 * 3; j++) for (int b = 7; b >= 0; b--) {
                        uint32_t ts = SysTick->CNTL;
                        R32_PA_OUT = hi;
                        uint32_t th = (grb[j] >> b) & 1 ? h1 : h0;
                        while (SysTick->CNTL - ts < th) {}
                        R32_PA_OUT = lo;
                        while (SysTick->CNTL - ts < tb) {}
                    }
                    SYS_RecoverIrq(irq);
                    mDelaymS(60);
                }
                mDelaymS(800);
            }
            say("round done - starting again\n");
        }
    }
#endif
#if LEDFINDER_PHASE == 12
    // Phase 12: phase 11 (0-pulses down to ~16 ns) still all white. Datasheet: PA0 = LED0 ("LED
    // screen" serial data). Census pu=0/pd=0 = PA0 feeds an NPN base through a resistor (Vbe clamp).
    // Model: inverting NPN level shifter that turns OFF late (storage time ts). LED line high time =
    // PA0 low time - ts, so short pulses vanish and only 1s survive (= white). Compensate: PA0
    // idles HIGH, a bit = PA0 LOW for (350 or 850 ns) + ts, then HIGH long enough. Sweep ts.
    {
        uint32_t t0 = SysTick->CNTL; mDelaymS(100); uint32_t tpu = (SysTick->CNTL - t0) / 100000;
        if (!tpu) tpu = 1;
        GPIOA_ModeCfg(all_a, GPIO_ModeIN_PU); GPIOB_ModeCfg(all_b | (0xFu << 20), GPIO_ModeIN_PU);
        static uint8_t grb[120 * 3];
        grb[1] = 0x40; grb[3] = 0x40; grb[8] = 0x40;    // Esc red, F1 green, F2 blue (G,R,B order)
        static const uint16_t TS[] = {300, 450, 600, 800, 1000, 1300};   // ns
        const uint32_t m = 1u << 0;
        R32_PA_OUT |= m; R32_PA_DIR |= m;               // idle HIGH = transistor on = LED line low
        for (;;) {
            for (unsigned k = 0; k < sizeof(TS) / sizeof(TS[0]); k++) {
                uint32_t h0 = (350 + TS[k]) * tpu / 1000, h1 = (850 + TS[k]) * tpu / 1000, tb = h1 + 600 * tpu / 1000;
                ps("state "); pn(k + 1); ps(": inverted, transistor delay "); pn(TS[k]); ps(" ns\n"); flush();
                uint32_t hi = R32_PA_OUT | m, lo = R32_PA_OUT & ~m;
                for (int f = 0; f < 60; f++) {
                    uint32_t irq; SYS_DisableAllIrq(&irq);
                    for (int j = 0; j < 120 * 3; j++) for (int b = 7; b >= 0; b--) {
                        uint32_t ts = SysTick->CNTL;
                        R32_PA_OUT = lo;
                        uint32_t th = (grb[j] >> b) & 1 ? h1 : h0;
                        while (SysTick->CNTL - ts < th) {}
                        R32_PA_OUT = hi;
                        while (SysTick->CNTL - ts < tb) {}
                    }
                    SYS_RecoverIrq(irq);
                    mDelaymS(40);                             // latch (LED line low)
                }
                mDelaymS(1000);
            }
            say("round done - starting again\n");
        }
    }
#endif
#if LEDFINDER_PHASE == 11
    // Phase 11: PA0 is the LED data line, but every pulse we sent read as a 1 (phase 9: a 47-pulse
    // UART burst gave Esc white + F1 yellow = 24 + 23 one-bits; phase 10: all-white with any timing
    // or polarity). Real 0-pulses were >= ~150 ns (read-modify-write + SysTick polling). Here the
    // 0-pulse is two plain stores with 0..16 nops between (16 ns per cycle), 1-pulse 640 ns.
    {
        uint32_t t0 = SysTick->CNTL; mDelaymS(100); uint32_t tpu = (SysTick->CNTL - t0) / 100000;
        if (!tpu) tpu = 1;
        GPIOA_ModeCfg(all_a, GPIO_ModeIN_PU); GPIOB_ModeCfg(all_b | (0xFu << 20), GPIO_ModeIN_PU);
        static uint8_t grb[120 * 3];
        grb[1] = 0x40; grb[3] = 0x40; grb[8] = 0x40;    // Esc red, F1 green, F2 blue (G,R,B order)
        static const uint8_t NOPS[] = {0, 1, 2, 4, 8, 16};
        const uint32_t m = 1u << 0, h1 = 640 * tpu / 1000, tb = 1250 * tpu / 1000;
        R32_PA_CLR = m; R32_PA_DIR |= m;
        for (;;) {
            for (unsigned k = 0; k < sizeof(NOPS); k++) {
                ps("state "); pn(k + 1); ps(": 0-pulse = 2 stores + "); pn(NOPS[k]); ps(" nops (~"); pn(16 * (NOPS[k] + 1)); ps(" ns)\n"); flush();
                uint32_t hi = R32_PA_OUT | m, lo = R32_PA_OUT & ~m;
                for (int f = 0; f < 60; f++) {
                    uint32_t irq; SYS_DisableAllIrq(&irq);
                    for (int j = 0; j < 120 * 3; j++) for (int b = 7; b >= 0; b--) {
                        uint32_t ts = SysTick->CNTL;
                        if ((grb[j] >> b) & 1) {
                            R32_PA_OUT = hi; while (SysTick->CNTL - ts < h1) {} R32_PA_OUT = lo;
                        } else {
                            switch (NOPS[k]) {   // fixed nop runs so the pulse length is exact
                            case 0: R32_PA_OUT = hi; R32_PA_OUT = lo; break;
                            case 1: R32_PA_OUT = hi; __asm__ volatile("nop"); R32_PA_OUT = lo; break;
                            case 2: R32_PA_OUT = hi; __asm__ volatile("nop\nnop"); R32_PA_OUT = lo; break;
                            case 4: R32_PA_OUT = hi; __asm__ volatile("nop\nnop\nnop\nnop"); R32_PA_OUT = lo; break;
                            case 8: R32_PA_OUT = hi; __asm__ volatile(".rept 8\nnop\n.endr"); R32_PA_OUT = lo; break;
                            default: R32_PA_OUT = hi; __asm__ volatile(".rept 16\nnop\n.endr"); R32_PA_OUT = lo; break;
                            }
                        }
                        while (SysTick->CNTL - ts < tb) {}
                    }
                    SYS_RecoverIrq(irq);
                    mDelaymS(40);                             // latch
                }
                mDelaymS(1000);
            }
            say("round done - starting again\n");
        }
    }
#endif
#if LEDFINDER_PHASE == 10
    // Phase 10: phase 9 left Esc WHITE and F1 YELLOW (all else dark) = ~40 one-bits reached an
    // addressable chain that starts at Esc. Census: PA0, PA6, PB2, PB19 are held LOW externally
    // (pu=0 pd=0) - what a data line with a pull-down looks like. Pin setup exactly as phase 9 (all
    // free pins pull-up inputs). On each of those 4 pins try 5 bit timings with Esc=RED,
    // F1=GREEN, F2=BLUE, rest off. The working pin+timing shows exactly that.
    {
        uint32_t t0 = SysTick->CNTL; mDelaymS(100); uint32_t tpu = (SysTick->CNTL - t0) / 100000;
        if (!tpu) tpu = 1;
        uint32_t la = all_a, lb = all_b | (0xFu << 20);
        GPIOA_ModeCfg(la, GPIO_ModeIN_PU); GPIOB_ModeCfg(lb, GPIO_ModeIN_PU);
        // Result v1 (PA6/PA0/PB2/PB19 x 5 timings): state 6 = PA0 + WS2812 timing lit ALL keys
        // WHITE = every bit read as 1 -> PA0 is the data pin and our "0" pulses look too long at
        // the LEDs (pulse stretched by a buffer?). v2: PA0 only, shorter and shorter high times.
        // v2 result: all six (0-pulse down to 60 ns) stayed WHITE -> not stretching; the line is most
        // likely INVERTED (buffer between PA0 and the LEDs). v3: PA0 inverted = idles HIGH, a bit
        // is a LOW pulse. (Phase 6 tried inverted but held all other free pins LOW: never lit.)
        static const uint8_t PIN[] = {0};
        static const struct { const char *name; uint16_t h0, h1, bit; } T[] = {   // ns
            {"INVERTED WS2812 0.35/0.8/1.25us", 350, 800, 1250},
            {"INVERTED SK6812 0.3/0.6/1.2us", 300, 600, 1200},
            {"INVERTED 0.25/0.7/1.25us", 250, 700, 1250},
            {"INVERTED 0.4/0.9/1.4us", 400, 900, 1400},
            {"INVERTED slow 0.5/1.2/2.5us", 500, 1200, 2500},
        };
        static uint8_t grb[120 * 3];
        grb[1] = 0x40; grb[3] = 0x40; grb[8] = 0x40;    // Esc red, F1 green, F2 blue (G,R,B order)
        for (unsigned s = 1;; ) {
            for (unsigned i = 0; i < sizeof(PIN); i++) {
                int p = PIN[i]; char nm[8];
                volatile uint32_t *out = p < NPA ? &R32_PA_OUT : &R32_PB_OUT, *dir = p < NPA ? &R32_PA_DIR : &R32_PB_DIR;
                uint32_t m = 1u << (p < NPA ? p : p - NPA);
                for (unsigned k = 0; k < sizeof(T) / sizeof(T[0]); k++, s++) {
                    uint32_t h0 = T[k].h0 * tpu / 1000, h1 = T[k].h1 * tpu / 1000, tb = T[k].bit * tpu / 1000;
                    ps("state "); pn(s); ps(": "); ps(pin_name(p, nm)); ps(" "); ps(T[k].name); ps("\n"); flush();
                    *out |= m; *dir |= m;                     // idle HIGH (= LED line low)
                    for (int f = 0; f < 60; f++) {
                        uint32_t irq; SYS_DisableAllIrq(&irq);
                        for (int j = 0; j < 120 * 3; j++) for (int b = 7; b >= 0; b--) {
                            uint32_t ts = SysTick->CNTL;
                            *out &= ~m;
                            uint32_t th = (grb[j] >> b) & 1 ? h1 : h0;
                            while (SysTick->CNTL - ts < th) {}
                            *out |= m;
                            while (SysTick->CNTL - ts < tb) {}
                        }
                        SYS_RecoverIrq(irq);
                        mDelaymS(40);                         // latch
                    }
                    mDelaymS(500);
                }
            }
            say("round done - starting again\n");
            s = 1;
        }
    }
#endif
#if LEDFINDER_PHASE == 9
    // Phase 9: the LEDs have power (~3 V on the "+" pad) but stay dark and U7 never lit them by
    // itself, so U8 most likely runs them and waits for a command. Its link (PB20-PB23) is silent.
    // On every free pin: UART probe bytes at common baud rates, a 20 ms break, and keyfinder-style
    // 3 us low pulses (the keyfinder ran before the one-off white); after each, listen on all pins
    // (the sender too, for single-wire links) and print anything that answers.
    {
        uint32_t t0 = SysTick->CNTL; mDelaymS(100); uint32_t tpu = (SysTick->CNTL - t0) / 100000;
        if (!tpu) tpu = 1;
        static const uint8_t PIN[] = {NPA + 20, NPA + 21, NPA + 22, NPA + 23, 0, 1, 2, 3, 4, 5, 6, NPA + 2, NPA + 3, NPA + 18, NPA + 19};
        static const uint32_t BAUD[] = {9600, 19200, 38400, 57600, 115200, 230400, 250000, 460800, 500000, 921600, 1000000, 2000000};
        static const uint8_t MSG[] = {0x55, 0xAA, 0x00, 0x01, 0xFF, 0xA5, 0x5A, 0x7E, 0x01, 0x02, 0x03, 0x04, 0xAA, 0x55, 0x0D, 0x0A};
        const unsigned NP = sizeof(PIN);
        uint32_t la = all_a, lb = all_b | (0xFu << 20);
        GPIOA_ModeCfg(la, GPIO_ModeIN_Floating); GPIOB_ModeCfg(lb, GPIO_ModeIN_Floating);
        mDelaymS(5);
        say("census (pu/pd = level read with pull-up/pull-down; 1/1 or 0/0 = driven by something)\n");
        for (unsigned i = 0; i < NP; i++) {
            int p = PIN[i]; uint32_t m = 1u << (p < NPA ? p : p - NPA); char nm[8]; int pu, pd;
            if (p < NPA) { GPIOA_ModeCfg(m, GPIO_ModeIN_PU); mDelaymS(2); pu = !!(R32_PA_PIN & m); GPIOA_ModeCfg(m, GPIO_ModeIN_PD); mDelaymS(2); pd = !!(R32_PA_PIN & m); GPIOA_ModeCfg(m, GPIO_ModeIN_Floating); }
            else { GPIOB_ModeCfg(m, GPIO_ModeIN_PU); mDelaymS(2); pu = !!(R32_PB_PIN & m); GPIOB_ModeCfg(m, GPIO_ModeIN_PD); mDelaymS(2); pd = !!(R32_PB_PIN & m); GPIOB_ModeCfg(m, GPIO_ModeIN_Floating); }
            ps("  "); ps(pin_name(p, nm)); ps(" pu="); pn(pu); ps(" pd="); pn(pd); ps("\n"); flush();
        }
        GPIOA_ModeCfg(la, GPIO_ModeIN_PU); GPIOB_ModeCfg(lb, GPIO_ModeIN_PU);   // floating pins (PB3) would fake answers
        mDelaymS(5);
        for (uint32_t round = 1;; round++) {
            ps("--- round "); pn(round); ps("\n"); flush();
            for (unsigned i = 0; i < NP; i++) {
                int p = PIN[i]; char nm[8];
                volatile uint32_t *out = p < NPA ? &R32_PA_OUT : &R32_PB_OUT, *dir = p < NPA ? &R32_PA_DIR : &R32_PB_DIR;
                uint32_t m = 1u << (p < NPA ? p : p - NPA);
                ps("state "); pn(i + 1); ps(": talking on "); ps(pin_name(p, nm)); ps("\n"); flush();
                for (unsigned k = 0; k < sizeof(BAUD) / sizeof(BAUD[0]); k++) {
                    uint32_t bt = tpu * 1000000 / BAUD[k];
                    uart_tx(p, bt, MSG, sizeof(MSG));
                    *dir &= ~m;                                   // release: listen on it too
                    int n = capture(la, lb, 50000 * tpu);
                    if (!n) continue;
                    ps(" ANSWER after "); pn(BAUD[k]); ps(" baud on "); ps(pin_name(p, nm)); ps(":\n"); flush();
                    for (unsigned q = 0; q < NP; q++) report_pin(PIN[q], n, tpu, bt, 0);
                }
                *out &= ~m; *dir |= m; mDelaymS(20); *dir &= ~m;  // 20 ms break
                int n = capture(la, lb, 100000 * tpu);
                if (n) { ps(" ANSWER after break on "); ps(pin_name(p, nm)); ps(":\n"); flush();
                         for (unsigned q = 0; q < NP; q++) report_pin(PIN[q], n, tpu, 0, 0); }
                for (int t = 0; t < 2000; t++) { *out &= ~m; *dir |= m; DelayUs(3); *dir &= ~m; DelayUs(997); }
                n = capture(la, lb, 100000 * tpu);
                if (n) { ps(" ANSWER after pulses on "); ps(pin_name(p, nm)); ps(":\n"); flush();
                         for (unsigned q = 0; q < NP; q++) report_pin(PIN[q], n, tpu, 0, 0); }
            }
            say("round done - starting again\n");
        }
    }
#endif
#if LEDFINDER_PHASE == 8
    // Phase 8: the LEDs were steady WHITE only under our keyboard firmware, which scans the matrix
    // (pull-ups, rows/cols pulsed low) and never touches the free pins (they float). Phases 2-7
    // drove every free pin LOW from the start. So: copy the keyboard's pin setup, check white,
    // then nudge ONE free pin at a time (HIGH 3 s, float, LOW 3 s, float) and see which kills it.
    {
        const uint32_t MA = 0xFF80u, MB = 0x3F3F0u | 3u;   // matrix PA7-15, PB4-9, PB12-17 + knob PB0/1
        GPIOA_ModeCfg(MA, GPIO_ModeIN_PU); GPIOB_ModeCfg(MB, GPIO_ModeIN_PU);
        #define SCAN_WAIT(ms) for (uint32_t w_ = 0; w_ < (ms); w_++) { \
            for (int p_ = 0; p_ < 32; p_++) { uint32_t m_ = 1u << p_; \
                if (MA & m_ & ~3u) { R32_PA_CLR = m_; R32_PA_DIR |= m_; DelayUs(3); R32_PA_DIR &= ~m_; DelayUs(8); } \
                if (MB & m_ & ~3u) { R32_PB_CLR = m_; R32_PB_DIR |= m_; DelayUs(3); R32_PB_DIR &= ~m_; DelayUs(8); } } \
            DelayUs(300); }
        say("state 0: keyboard-like, free pins floating (expect WHITE) - 30 s\n");
        for (int k = 0; k < 15; k++) {
            uint32_t a = R32_PA_PIN, b = R32_PB_PIN; char nm[8];
            ps("  levels:");
            for (unsigned i = 0; i < NCAND; i++) { ps(" "); ps(pin_name(CAND[i], nm)); ps((CAND[i] < NPA ? a >> CAND[i] : b >> (CAND[i] - NPA)) & 1 ? "=1" : "=0"); }
            for (int q = 20; q < 24; q++) { ps(" PB"); pn(q); ps((b >> q) & 1 ? "=1" : "=0"); }
            ps("\n"); flush();
            SCAN_WAIT(2000);
        }
        for (unsigned s = 1;; s++) {
            for (unsigned i = 0; i < NCAND; i++) {
                int p = CAND[i]; char nm[8];
                volatile uint32_t *out = p < NPA ? &R32_PA_OUT : &R32_PB_OUT, *dir = p < NPA ? &R32_PA_DIR : &R32_PB_DIR;
                uint32_t m = 1u << (p < NPA ? p : p - NPA);
                ps("state "); pn(i * 2 + 1); ps(": "); ps(pin_name(p, nm)); ps(" HIGH\n"); flush();
                *out |= m; *dir |= m; SCAN_WAIT(3000);
                *dir &= ~m; ps("   floating\n"); flush(); SCAN_WAIT(2000);
                ps("state "); pn(i * 2 + 2); ps(": "); ps(pin_name(p, nm)); ps(" LOW\n"); flush();
                *out &= ~m; *dir |= m; SCAN_WAIT(3000);
                *dir &= ~m; ps("   floating\n"); flush(); SCAN_WAIT(2000);
            }
            say("round done - starting again\n");
        }
    }
#endif
#if LEDFINDER_PHASE == 7
    // Phase 7: white ONLY ever appeared (phases 1+2) when MANY pins toggled in lockstep, whatever
    // the data - never with one pin alone (3-6). That fits a shift-register LED driver (CLK + DATA +
    // LATCH among the candidates, PB19 = active-low output enable): pins rising together shift 1s.
    // Knockout: keep everything toggling but hold ONE pin still. Holding DATA low shifts 0s -> dark.
    {
        const uint32_t P19 = 1u << 19;
        uint32_t ta = all_a, tb = all_b & ~P19;        // toggled set; PB19 stays low (enable)
        R32_PA_CLR = all_a; R32_PB_CLR = all_b;
        GPIOA_ModeCfg(all_a, GPIO_ModeOut_PP_5mA); GPIOB_ModeCfg(all_b, GPIO_ModeOut_PP_5mA);
        unsigned s = 0;
        for (;;) {
            ps("state 0: ALL toggling (expect WHITE)\n"); flush();
            for (int t = 0; t < 300; t++) { ws_frame(ta, tb, 120, 0x00); mDelaymS(5); }
            for (int hi = 0; hi <= 1; hi++) {
                for (unsigned i = 0; i < NCAND; i++) {
                    int p = CAND[i];
                    if (p == NPA + 19) continue;
                    uint32_t ma = p < NPA ? 1u << p : 0, mb = p < NPA ? 0 : 1u << (p - NPA);
                    char nm[8];
                    ps("state "); pn(++s); ps(": all toggling EXCEPT "); ps(pin_name(p, nm));
                    ps(hi ? " held HIGH\n" : " held LOW\n"); flush();
                    if (hi) { R32_PA_OUT |= ma; R32_PB_OUT |= mb; } else { R32_PA_CLR = ma; R32_PB_CLR = mb; }
                    for (int t = 0; t < 300; t++) { ws_frame(ta & ~ma, tb & ~mb, 120, 0x00); mDelaymS(5); }
                    R32_PA_CLR = ma; R32_PB_CLR = mb;
                    ps("   (all toggling again)\n"); flush();
                    for (int t = 0; t < 200; t++) { ws_frame(ta, tb, 120, 0x00); mDelaymS(5); }
                }
            }
            ps("state 99: PB19 HIGH (expect OFF)\n"); flush();
            R32_PB_OUT |= P19;
            for (int t = 0; t < 300; t++) { ws_frame(ta, tb, 120, 0x00); mDelaymS(5); }
            R32_PB_CLR = P19;
            s = 0;
            say("round done - starting again\n");
        }
    }
#endif
#if LEDFINDER_PHASE == 6
    // Phase 6: (1) log what U8 sends on PB20-PB23; (2) the LED data line may sit behind an
    // inverting level shifter (idle high) or need open-drain driving to a 5 V pull-up - phases 2-5
    // only tried plain push-pull. Each candidate in turn: RED 2 s then GREEN 2 s, others low.
    {
        uint32_t t0 = SysTick->CNTL; mDelaymS(100); uint32_t tpu = (SysTick->CNTL - t0) / 100000;
        if (!tpu) tpu = 1;
        t0h = (tpu * 35 + 50) / 100; t1h = (tpu * 80 + 50) / 100; tbit = (tpu * 125 + 50) / 100;
        ps("ticks/us "); pn(tpu); ps("\n"); flush();
        static const char *MN[3] = {"", "INVERTED", "OPEN-DRAIN"};
        for (uint32_t round = 1;; round++) {
            ps("--- round "); pn(round); ps(": U8 link capture\n"); flush();
            sniff(tpu);
            unsigned s = 0;
            for (int mode = 1; mode <= 2; mode++) {
                for (unsigned i = 0; i < NCAND; i++) {
                    int p = CAND[i];
                    uint32_t ma = p < NPA ? 1u << p : 0, mb = p < NPA ? 0 : 1u << (p - NPA);
                    char nm[8];
                    R32_PA_CLR = all_a; R32_PB_CLR = all_b;
                    GPIOA_ModeCfg(all_a, GPIO_ModeOut_PP_5mA); GPIOB_ModeCfg(all_b, GPIO_ModeOut_PP_5mA);
                    if (mode == 1) { R32_PA_OUT |= ma; R32_PB_OUT |= mb; }   // idle high
                    mDelaymS(5);
                    ps("state "); pn(++s); ps(": "); ps(MN[mode]); ps(" on "); ps(pin_name(p, nm));
                    ps(" - RED then GREEN\n"); flush();
                    ws_mode = mode;
                    for (int t = 0; t < 500; t++) { ws_frame_rgb(ma, mb, 120, 0x00, 0x30, 0x00); mDelaymS(1); }
                    for (int t = 0; t < 500; t++) { ws_frame_rgb(ma, mb, 120, 0x30, 0x00, 0x00); mDelaymS(1); }
                    ws_mode = 0;
                }
            }
            R32_PA_CLR = all_a; R32_PB_CLR = all_b;
            GPIOA_ModeCfg(all_a, GPIO_ModeOut_PP_5mA); GPIOB_ModeCfg(all_b, GPIO_ModeOut_PP_5mA);
        }
    }
#endif
#if LEDFINDER_PHASE == 5
    // Phase 5: phase 2 sent all-ZERO frames and the LEDs showed WHITE - what WS2812s do when every
    // bit is read as a 1, i.e. the pulses were too long. So measure SysTick against mDelaymS,
    // scale the bit timing from the measurement, and try one candidate at a time again.
    {
        uint32_t t0 = SysTick->CNTL; mDelaymS(100); uint32_t dt = SysTick->CNTL - t0;
        uint32_t tpu = dt / 100000;                    // SysTick ticks per microsecond
        if (tpu == 0) tpu = 1;
        t0h = (tpu * 35 + 50) / 100; t1h = (tpu * 80 + 50) / 100; tbit = (tpu * 125 + 50) / 100;
        if (t0h == 0) t0h = 1;
        char line[80]; int n = 0;
        const char *a = "SysTick ticks per 100 ms: "; while (*a) line[n++] = *a++;
        char d[12]; int k = 0; uint32_t v = dt; do { d[k++] = '0' + v % 10; v /= 10; } while (v);
        while (k) line[n++] = d[--k];
        a = " -> T0H/T1H/TBIT ticks "; while (*a) line[n++] = *a++;
        uint32_t vals[3] = {t0h, t1h, tbit};
        for (int j = 0; j < 3; j++) { v = vals[j]; k = 0; do { d[k++] = '0' + v % 10; v /= 10; } while (v); while (k) line[n++] = d[--k]; line[n++] = j < 2 ? '/' : '\n'; }
        line[n] = 0; say(line);

        R32_PA_CLR = all_a; R32_PB_CLR = all_b;
        GPIOA_ModeCfg(all_a, GPIO_ModeOut_PP_5mA); GPIOB_ModeCfg(all_b, GPIO_ModeOut_PP_5mA);
        for (;;) {
            for (unsigned i = 0; i < NCAND; i++) {
                int p = CAND[i];
                uint32_t ma = p < NPA ? 1u << p : 0, mb = p < NPA ? 0 : 1u << (p - NPA);
                char nm[8]; n = 0;
                a = "state "; while (*a) line[n++] = *a++;
                if (i + 1 >= 10) line[n++] = '0' + (i + 1) / 10; line[n++] = '0' + (i + 1) % 10;
                a = ": RED on "; while (*a) line[n++] = *a++;
                for (char *q = pin_name(p, nm); *q;) line[n++] = *q++;
                line[n++] = '\n'; line[n] = 0;
                say(line);
                for (int t = 0; t < 600; t++) { ws_frame_rgb(ma, mb, 120, 0x00, 0x30, 0x00); mDelaymS(1); }
                for (int t = 0; t < 200; t++) { ws_frame_rgb(ma, mb, 120, 0x00, 0x00, 0x00); mDelaymS(1); }
            }
            say("round done - starting again\n");
        }
    }
#endif
#if LEDFINDER_PHASE == 4
    // Phase 4: theory from phases 1-3 - PB19 is the WS2812 DATA line (LED power is always on):
    // phase 1 lit white whenever PB19 carried data, phase 2 never sent data on PB19 (LEDs kept
    // the latched white), phase 3 after a power cycle sent no data at all (dark). Confirm with
    // colours on PB19 only.
    {
        const uint32_t D = 1u << 19;
        // As in phases 1/2 (where the LEDs lit): every other candidate actively LOW - one of them
        // is probably an active-low LED power switch. Phase 4 v1 left them floating: stayed dark.
        R32_PA_CLR = all_a; R32_PB_CLR = all_b;
        GPIOA_ModeCfg(all_a, GPIO_ModeOut_PP_5mA); GPIOB_ModeCfg(all_b, GPIO_ModeOut_PP_5mA);
        static const struct { const char *name; uint8_t g, r, b; } C[] = {
            {"state 1: RED\n", 0x00, 0x30, 0x00}, {"state 2: GREEN\n", 0x30, 0x00, 0x00},
            {"state 3: BLUE\n", 0x00, 0x00, 0x30}, {"state 4: WHITE\n", 0x20, 0x20, 0x20},
            {"state 5: OFF\n", 0, 0, 0},
        };
        for (;;) {
            for (unsigned i = 0; i < sizeof(C) / sizeof(C[0]); i++) {
                say(C[i].name);
                for (int t = 0; t < 500; t++) { ws_frame_rgb(0, D, 120, C[i].g, C[i].r, C[i].b); mDelaymS(1); }
            }
            say("state 6: RAINBOW\n");
            for (int t = 0; t < 1200; t++) {           // hue wheel shifting along the chain
                uint32_t irq; SYS_DisableAllIrq(&irq);
                uint32_t hi = R32_PB_OUT | D, lo = R32_PB_OUT & ~D;
                for (int l = 0; l < 120; l++) {
                    uint8_t h = (uint8_t)(l * 6 + t * 2), g, r, b;
                    if (h < 85) { r = 85 - h; g = h; b = 0; }
                    else if (h < 170) { h -= 85; r = 0; g = 85 - h; b = h; }
                    else { h -= 170; r = h; g = 0; b = 85 - h; }
                    uint8_t col[3] = {g >> 1, r >> 1, b >> 1};
                    for (int c = 0; c < 3; c++) for (int bt = 7; bt >= 0; bt--) {
                        uint32_t t0 = SysTick->CNTL;
                        R32_PB_OUT = hi;
                        uint32_t th = ((col[c] >> bt) & 1) ? T1H : T0H;
                        while (SysTick->CNTL - t0 < th) {}
                        R32_PB_OUT = lo;
                        while (SysTick->CNTL - t0 < TBIT) {}
                    }
                }
                SYS_RecoverIrq(irq);
                mDelaymS(1);
            }
            say("round done - starting again\n");
        }
    }
#endif
#if LEDFINDER_PHASE == 3
    // Phase 3: the LEDs did NOT react to WS2812 data (constant white with PB19 low), so they are
    // plain RGB LEDs whose colour channels are switched by GPIOs. Keep PB19 low (power on), all
    // other candidates low, and raise ONE candidate high for 4 s at a time; then all high.
    // Watch for the white changing (cyan = red off, magenta = green off, yellow = blue off,
    // darker/brighter/off).
    {
        R32_PA_CLR = all_a; R32_PB_CLR = all_b;
        GPIOA_ModeCfg(all_a, GPIO_ModeOut_PP_5mA); GPIOB_ModeCfg(all_b, GPIO_ModeOut_PP_5mA);
        for (;;) {
            unsigned s = 0;
            for (unsigned i = 0; i < NCAND; i++) {
                int p = CAND[i];
                if (p == NPA + 19) continue;
                s++;
                uint32_t ma = p < NPA ? 1u << p : 0, mb = p < NPA ? 0 : 1u << (p - NPA);
                char nm[8], line[48]; int n = 0;
                const char *a = "state "; while (*a) line[n++] = *a++;
                if (s >= 10) line[n++] = '0' + s / 10; line[n++] = '0' + s % 10;
                a = ": HIGH on "; while (*a) line[n++] = *a++;
                for (char *q = pin_name(p, nm); *q;) line[n++] = *q++;
                line[n++] = '\n'; line[n] = 0;
                say(line);
                R32_PA_OUT |= ma; R32_PB_OUT |= mb;
                mDelaymS(4000);
                R32_PA_CLR = ma; R32_PB_CLR = mb;
                mDelaymS(1500);                          // back to plain white in between
            }
            say("state 11: ALL candidates HIGH (except PB19)\n");
            R32_PA_OUT |= all_a; R32_PB_OUT |= all_b & ~(1u << 19);
            mDelaymS(4000);
            R32_PA_CLR = all_a; R32_PB_CLR = all_b;
            mDelaymS(1500);
            say("round done - starting again\n");
        }
    }
#endif
#if LEDFINDER_PHASE == 2
    // Phase 2: LED power found = PB19 active low. Keep it low and send RED on ONE candidate at a
    // time (black on all of them in between): the state where the keys turn red is the data pin.
    {
        const int PWR = NPA + 19;
        uint32_t ca = all_a, cb = all_b & ~(1u << 19);
        R32_PA_CLR = all_a; R32_PB_CLR = all_b;          // PB19 low = LED power on
        GPIOA_ModeCfg(all_a, GPIO_ModeOut_PP_5mA); GPIOB_ModeCfg(all_b, GPIO_ModeOut_PP_5mA);
        (void)PWR;
        for (;;) {
            unsigned s = 0;
            for (unsigned i = 0; i < NCAND; i++) {
                int p = CAND[i];
                if (p == NPA + 19) continue;
                s++;
                for (int t = 0; t < 100; t++) { ws_frame(ca, cb, 120, 0x00); mDelaymS(5); }   // dark ~0.6 s
                char nm[8], line[48]; int n = 0;
                const char *a = "state "; while (*a) line[n++] = *a++;
                if (s >= 10) line[n++] = '0' + s / 10; line[n++] = '0' + s % 10;
                a = ": RED on "; while (*a) line[n++] = *a++;
                for (char *q = pin_name(p, nm); *q;) line[n++] = *q++;
                line[n++] = '\n'; line[n] = 0;
                say(line);
                uint32_t ma = p < NPA ? 1u << p : 0, mb = p < NPA ? 0 : 1u << (p - NPA);
                for (int t = 0; t < 600; t++) { ws_frame_rgb(ma, mb, 120, 0x00, 0x30, 0x00); mDelaymS(1); }  // ~3 s red
            }
            say("round done - starting again\n");
        }
    }
#endif
    for (uint32_t round = 1;; round++) {
        for (unsigned s = 0; s < 2 * NCAND; s++) {
            int p = CAND[s / 2], pol = (s & 1) ? 0 : 1;   // pol 1 = power pin driven HIGH
            uint32_t pa = p < NPA ? 1u << p : 0, pb = p < NPA ? 0 : 1u << (p - NPA);
            // all candidates as push-pull outputs, low; the power candidate at its polarity
            R32_PA_CLR = all_a; R32_PB_CLR = all_b;
            GPIOA_ModeCfg(all_a, GPIO_ModeOut_PP_5mA); GPIOB_ModeCfg(all_b, GPIO_ModeOut_PP_5mA);
            if (pol) { R32_PA_OUT |= pa; R32_PB_OUT |= pb; }
            char nm[8], line[64]; int n = 0;
            const char *a = "state "; while (*a) line[n++] = *a++;
            if (s + 1 >= 10) line[n++] = '0' + (s + 1) / 10; line[n++] = '0' + (s + 1) % 10;
            a = ": power "; while (*a) line[n++] = *a++;
            for (char *q = pin_name(p, nm); *q;) line[n++] = *q++;
            a = pol ? "=HIGH, data on the others\n" : "=LOW, data on the others\n";
            while (*a) line[n++] = *a++;
            line[n] = 0;
            say(line);
            uint32_t t_end = 3000;                    // ~3 s of frames
            for (uint32_t t = 0; t < t_end; t += 5) {
                ws_frame(all_a & ~pa, all_b & ~pb, 120, 0x10);
                mDelaymS(1);                          // >= 280 us latch
            }
        }
        say("round done - starting again\n");
    }
}
