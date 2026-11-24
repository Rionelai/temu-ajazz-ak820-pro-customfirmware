// Key finder for the AK820 Pro main chip U7 (WCH CH584).
// Enumerates as a USB keyboard (WCH's CompoundDev example, built with main renamed) and
// TYPES what it discovers, so no extra tools are needed on the PC:
//   "a3-b12"   a key press connects PA3 and PB12 (matrix row/column pair)
//   "b5 lo"    a pin that is normally high went low with nothing driven (e.g. knob contact)
//   "b5 hi"    ...and went back high
// Method: all pins are inputs with pull-ups; each pin in turn is driven low for a few
// microseconds and every other pin is read. Anything already connected at power-up is the
// baseline and is ignored, so only changes caused by pressing keys or turning the knob show.
#include "CH58x_common.h"

// Output goes to a USB serial port (CDC ACM, usb_cdc.c) instead of keystrokes, so the finder
// can never type into whatever window has focus on the PC.
extern uint8_t DevConfig;
extern volatile uint8_t CdcDTR;
extern uint8_t EP0_Databuf[], EP1_Databuf[], EP2_Databuf[], EP3_Databuf[];

#define NPA 16
#define NPB 24
#define USB_PINS ((1u << 10) | (1u << 11))          // PB10/PB11 = USB D-/D+, never touched
static const uint32_t MASK_A = 0xFFFFu;
static const uint32_t MASK_B = 0xFFFFFFu & ~USB_PINS;

#define NPINS (NPA + NPB)                              // index 0..15 = PA, 16..39 = PB
static inline int usable(int i) { return i < NPA ? 1 : ((MASK_B >> (i - NPA)) & 1); }

static void name_of(int i, char *s) {                  // "a3", "b12"
    int n = i < NPA ? i : i - NPA;
    *s++ = i < NPA ? 'a' : 'b';
    if (n >= 10) *s++ = '0' + n / 10;
    *s++ = '0' + n % 10;
    *s = 0;
}

// ---------------------------------------------------------------- output over USB CDC (EP2 IN)
static void type_str(const char *s) {
    if (!DevConfig || !CdcDTR) return;                 // nobody listening: drop the message
    uint32_t n = 0;
    while (s[n]) n++;
    while (n) {
        uint32_t k = n > 64 ? 64 : n;
        // wait for the previous packet to be taken by the host (max ~50 ms, then drop)
        for (int t = 0; t < 500 && (R8_UEP2_CTRL & MASK_UEP_T_RES) == UEP_T_RES_ACK; t++) DelayUs(100);
        if ((R8_UEP2_CTRL & MASK_UEP_T_RES) == UEP_T_RES_ACK) return;
        memcpy(pEP2_IN_DataBuf, s, k);
        DevEP2_IN_Deal(k);
        s += k; n -= k;
    }
}

// ---------------------------------------------------------------- pin scanning
static void all_inputs_pullup(void) {
    GPIOA_ModeCfg(MASK_A, GPIO_ModeIN_PU);
    GPIOB_ModeCfg(MASK_B, GPIO_ModeIN_PU);
}
static inline void read_low(uint32_t *a, uint32_t *b) {
    *a = ~R32_PA_PIN & MASK_A;
    *b = ~R32_PB_PIN & MASK_B;
}
// For each driver pin: which other pins read low while it is driven low. Plus idle lows.
typedef struct { uint32_t a[NPINS], b[NPINS], idle_a, idle_b; } scan_t;

static void scan(scan_t *s) {
    read_low(&s->idle_a, &s->idle_b);
    for (int d = 0; d < NPINS; d++) {
        s->a[d] = s->b[d] = 0;
        if (!usable(d)) continue;
        if (d < NPA) { uint32_t m = 1u << d; R32_PA_CLR = m; R32_PA_DIR |= m; DelayUs(4); read_low(&s->a[d], &s->b[d]); R32_PA_DIR &= ~m; s->a[d] &= ~m; }
        else { uint32_t m = 1u << (d - NPA); R32_PB_CLR = m; R32_PB_DIR |= m; DelayUs(4); read_low(&s->a[d], &s->b[d]); R32_PB_DIR &= ~m; s->b[d] &= ~m; }
        DelayUs(20);                                   // let the pull-ups recover
    }
}

static scan_t base, cur, prev, prev2;
static uint32_t noisy_a, noisy_b;                      // pins that change on their own: ignored
static uint8_t reported[NPINS][NPINS];                 // pair currently reported as pressed
static uint8_t idle_state[NPINS];                      // 1 = reported as "lo"

static int bit(uint32_t a, uint32_t b, int i) { return i < NPA ? (a >> i) & 1 : (b >> (i - NPA)) & 1; }

int main(void) {
    HSECFG_Capacitance(HSECap_18p);
    SetSysClock(SYSCLK_FREQ);
    { void iap_flag_clear(void); iap_flag_clear(); }   // started: clear a pending update request

    pEP0_RAM_Addr = EP0_Databuf;
    pEP1_RAM_Addr = EP1_Databuf;
    pEP2_RAM_Addr = EP2_Databuf;
    pEP3_RAM_Addr = EP3_Databuf;
    USB_DeviceInit();
    PFIC_EnableIRQ(USB_IRQn);

    all_inputs_pullup();
    mDelaymS(50);

    // Baseline from 30 scans with no keys pressed; anything that varies is marked noisy.
    scan(&base);
    for (int k = 0; k < 30; k++) {
        scan(&cur);
        noisy_a |= base.idle_a ^ cur.idle_a; noisy_b |= base.idle_b ^ cur.idle_b;
        for (int d = 0; d < NPINS; d++) { noisy_a |= base.a[d] ^ cur.a[d]; noisy_b |= base.b[d] ^ cur.b[d]; }
        mDelaymS(5);
    }

    // PB20..PB23 toggle together on their own on this board (corner of the old solder
    // bridge): never treat them as key lines.
    noisy_b |= 0xFu << 20;
    // PB0/PB1 are the volume knob encoder, not matrix lines: their changing levels produced
    // bursts of fake "b0-bX"/"b1-bX" pairs. Excluded entirely (no lo/hi reports either).
    noisy_b |= 0x3u;

    prev = base; prev2 = base;
    uint8_t was_open = 0;
    while (1) {
        if (CdcDTR && !was_open) {                     // serial port just opened on the PC
            mDelaymS(50);
            char s[4]; int n = 0;
            for (int i = 0; i < NPINS; i++) if (bit(base.idle_a, base.idle_b, i)) n++;
            s[0] = '0' + (n / 10) % 10; s[1] = '0' + n % 10; s[2] = '\n'; s[3] = 0;
            type_str("keyfinder ready (serial) idle low "); type_str(s);
            type_str("idle low pins:");                // which pins sit low at rest
            for (int i = 0; i < NPINS; i++) if (usable(i) && bit(base.idle_a, base.idle_b, i)) {
                char x[8]; name_of(i, x); type_str(" "); type_str(x);
            }
            type_str("\n");
        }
        was_open = CdcDTR;
        scan(&cur);
        // Key pairs: new connection vs baseline, stable for 3 consecutive scans. Each key has a
        // diode, so a connection shows in one drive direction only: judge the pair (lo,hi) from
        // both directions together, otherwise it looks "released" every scan and repeats.
        for (int lo = 0; lo < NPINS; lo++) {
            for (int hi = lo + 1; hi < NPINS; hi++) {
                if (bit(noisy_a, noisy_b, lo) || bit(noisy_a, noisy_b, hi)) continue;
                // A pin that is already low with nothing driven (knob contact, signal line) is
                // not evidence of a key: only pairs where both pins idle high count.
                if (bit(cur.idle_a, cur.idle_b, lo) || bit(cur.idle_a, cur.idle_b, hi)) continue;
#define CONN(s) ((bit((s).a[lo], (s).b[lo], hi) && !bit(base.a[lo], base.b[lo], hi)) || \
                 (bit((s).a[hi], (s).b[hi], lo) && !bit(base.a[hi], base.b[hi], lo)))
                int now = CONN(cur), was1 = CONN(prev), was2 = CONN(prev2);
#undef CONN
                if (now && was1 && was2 && !reported[lo][hi]) {
                    char x[8], y[8], line[24];
                    reported[lo][hi] = 1;
                    name_of(lo, x); name_of(hi, y);
                    int n = 0; for (char *q = x; *q;) line[n++] = *q++; line[n++] = '-';
                    for (char *q = y; *q;) line[n++] = *q++; line[n++] = '\n'; line[n] = 0;
                    type_str(line);
                } else if (!now && !was1 && !was2 && reported[lo][hi]) {
                    reported[lo][hi] = 0;              // released: may be reported again
                }
            }
        }
        // Idle level changes (knob contacts, switches to GND).
        for (int i = 0; i < NPINS; i++) {
            if (!usable(i) || bit(noisy_a, noisy_b, i)) continue;
            int now = bit(cur.idle_a, cur.idle_b, i), b0 = bit(base.idle_a, base.idle_b, i);
            int s1 = bit(prev.idle_a, prev.idle_b, i), s2 = bit(prev2.idle_a, prev2.idle_b, i);
            if (now == s1 && now == s2) {
                int lowstate = (now != b0);
                if (lowstate != idle_state[i]) {
                    char x[8]; name_of(i, x);
                    type_str(x); type_str(now ? " lo\n" : " hi\n");
                    idle_state[i] = lowstate;
                }
            }
        }
        prev2 = prev; prev = cur;
        mDelaymS(3);
    }
}
