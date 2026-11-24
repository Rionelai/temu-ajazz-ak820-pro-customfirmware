// U7 firmware "u8link": USB serial <-> the one-wire link to U8 (U7 PB21-PB23, shorted on the
// board, <-> U8 PD6). Lets the PC update U8 (screen chip) through its bootloader without the
// Pico: tools/u8update.ps1. Installed like the finders: .\tools\update.ps1 -Bin ...\u8link.bin,
// then back to keyboard.bin when done.
// PC -> U7 (binary, over the serial port):
//   '?'                            -> "u8L1"
//   'Y' ...                        -> like 'X' at 460800 push-pull (U8 screen firmware v6+)
//   'X' tl th rn tmo [tl bytes]    -> send the bytes on the wire (115200 8N1, open-drain), then
//                                     receive up to rn bytes (first within tmo ms, then <= 5 ms
//                                     apart) -> 'x' n [n bytes]
//   'Z'                            -> catch the U8 bootloader after a U8 reset: pulse PB20 low
//                                     (maybe U8 reset), then send 7F every 2 ms for 150 ms
//                                     -> 'z' 1 (got 79) / 'z' 0
#include "CH58x_common.h"

extern uint8_t DevConfig;
extern volatile uint8_t CdcDTR;
extern uint8_t EP0_Databuf[], EP1_Databuf[], EP2_Databuf[], EP3_Databuf[];
void iap_flag_clear(void);

// ---------------------------------------------------------------- USB serial
static volatile uint8_t rxb[1024];
static volatile uint16_t rh, rt;               // written by the USB IRQ (rh) / main loop (rt)
void cdc_rx(const uint8_t *d, uint8_t l) {
    for (int i = 0; i < l; i++) { uint16_t n = (rh + 1) & 1023; if (n == rt) break; rxb[rh] = d[i]; rh = n; }
}
static int avail(void) { return (rh - rt) & 1023; }
static int getb(uint32_t ms) {                 // -1 on timeout
    for (uint32_t t = 0; !avail(); t++) { if (t >= ms * 10) return -1; DelayUs(100); }
    uint8_t b = rxb[rt]; rt = (rt + 1) & 1023; return b;
}
static void put(const uint8_t *s, uint32_t n) {
    if (!DevConfig || !CdcDTR) return;
    while (n) {
        uint32_t k = n > 64 ? 64 : n;
        for (int t = 0; t < 2000 && (R8_UEP2_CTRL & MASK_UEP_T_RES) == UEP_T_RES_ACK; t++) DelayUs(100);
        if ((R8_UEP2_CTRL & MASK_UEP_T_RES) == UEP_T_RES_ACK) return;
        memcpy(pEP2_IN_DataBuf, s, k); DevEP2_IN_Deal(k); s += k; n -= k;
    }
}

// ---------------------------------------------------------------- wire
#define WIRE_M  (0xEu << 20)                   // PB21-PB23
#define TXM     (1u << 22)
#define RXM     (1u << 21)
static uint32_t bit, tpms, bit_slow, bit_fast;
static uint8_t pushpull;                         // 'Y': screen firmware at 460800, push-pull
static inline uint32_t now(void) { return SysTick->CNTL; }
static inline void until(uint32_t t) { while ((int32_t)(now() - t) < 0) {} }

static void wtx(const uint8_t *d, int n) {     // open-drain: low = drive, high = pull-up
    uint32_t t = now();
    for (int i = 0; i < n; i++) {
        uint32_t w = 0xE00u | ((uint32_t)d[i] << 1);   // + 2 stop bits: gap for U8's screen firmware
        if (pushpull) R32_PB_DIR |= TXM;
        for (int k = 0; k < 12; k++) {
            if (pushpull) { if (w & (1u << k)) R32_PB_OUT |= TXM; else R32_PB_CLR = TXM; }
            else if (w & (1u << k)) R32_PB_DIR &= ~TXM; else R32_PB_DIR |= TXM;
            t += bit; until(t);
        }
    }
    R32_PB_DIR &= ~TXM; R32_PB_CLR = TXM;
}
static int wrx(uint32_t timeout) {             // one byte or -1
    uint32_t t0 = now();
    while (R32_PB_PIN & RXM) if (now() - t0 > timeout) return -1;
    uint32_t t = now() + bit + bit / 2;
    uint8_t b = 0;
    for (int k = 0; k < 8; k++) { until(t); if (R32_PB_PIN & RXM) b |= 1 << k; t += bit; }
    until(t);
    int ok = (R32_PB_PIN & RXM) != 0;
    while (!(R32_PB_PIN & RXM)) if (now() - t > 2 * bit) break;
    return ok ? b : -1;
}

static uint8_t txb[300], ans[260];

int main(void) {
    HSECFG_Capacitance(HSECap_18p);
    SetSysClock(SYSCLK_FREQ);
    iap_flag_clear();
    SysTick->CTLR = SysTick_CTLR_STCLK | SysTick_CTLR_STE;
    tpms = GetSysClock() / 1000; bit = bit_slow = GetSysClock() / 115200;
    bit_fast = (GetSysClock() + 230400) / 460800;
    GPIOB_SetBits(WIRE_M | (1u << 20)); GPIOB_ModeCfg(WIRE_M | (1u << 20), GPIO_ModeIN_PU);
    R32_PB_CLR = TXM | (1u << 20);             // output latches low; the pins stay inputs until driven

    // Recovery: U8 restarts together with U7 (reboot / power-up) and its bootloader listens for 7F
    // for 500 ms (bootloader v2). Send it right away (before USB starts), so u8link always lands in U8's
    // bootloader - even when the screen firmware is broken and can't be asked to restart itself.
    static uint8_t caught;
    {
        static const uint8_t sync = 0x7F;
        for (int i = 0; i < 600 && !caught; i++) { wtx(&sync, 1); if (wrx(1 * tpms) == 0x79) caught = 1; }
    }

    pEP0_RAM_Addr = EP0_Databuf; pEP1_RAM_Addr = EP1_Databuf;
    pEP2_RAM_Addr = EP2_Databuf; pEP3_RAM_Addr = EP3_Databuf;
    USB_DeviceInit();
    PFIC_EnableIRQ(USB_IRQn);

    for (;;) {
        int c = getb(1000);
        pushpull = 0; bit = bit_slow;              // every command starts at the bootloader speed
        if (c == '?') { put((const uint8_t *)"u8L1", 4); continue; }
        if (c == 'V') {                            // speed of 'Y': V lo hi (baud / 1200)
            int lo = getb(50), hi = getb(50);
            if (lo >= 0 && hi >= 0) { uint32_t baud = (lo | hi << 8) * 1200u; if (baud >= 9600) bit_fast = (GetSysClock() + baud / 2) / baud; }
            uint8_t r = 'v'; put(&r, 1); continue;
        }
        if (c == 'X' || c == 'Y') {
            pushpull = c == 'Y'; bit = pushpull ? bit_fast : bit_slow;
            int tl = getb(50), th = getb(50), rn = getb(50), tmo = getb(50);
            if (tl < 0 || th < 0 || rn < 0 || tmo < 0) continue;
            int n = tl | th << 8, ok = n <= (int)sizeof(txb);
            for (int i = 0; i < n; i++) { int b = getb(100); if (b < 0) { ok = 0; break; } if (i < (int)sizeof(txb)) txb[i] = b; }
            if (!ok) continue;
            uint32_t irq; SYS_DisableAllIrq(&irq);
            wtx(txb, n);
            int got = 0;
            for (; got < rn; got++) {
                int b = wrx(got ? 5 * tpms : (uint32_t)tmo * tpms);
                if (b < 0) break;
                ans[1 + got] = b;
            }
            SYS_RecoverIrq(irq);
            ans[0] = got;
            pushpull = 0; bit = bit_slow;
            uint8_t hdr = 'x'; put(&hdr, 1); put(ans, 1 + got);
            continue;
        }
        if (c == 'Z') {
            R32_PB_DIR |= 1u << 20; mDelaymS(20); R32_PB_DIR &= ~(1u << 20);   // PB20 low 20 ms
            int hit = 0;
            uint32_t irq; SYS_DisableAllIrq(&irq);
            static const uint8_t sync = 0x7F;
            for (int i = 0; i < 75 && !hit; i++) { wtx(&sync, 1); if (wrx(1 * tpms) == 0x79) hit = 1; }
            SYS_RecoverIrq(irq);
            uint8_t r[2] = {'z', (uint8_t)hit}; put(r, 2);
        }
    }
}
