// U8 RAM program: write a payload into U8's flash (one-time bootloader install via the Pico).
// Image layout (tools/u8flash.ps1 builds it): this program, padded to 0x400 bytes, then at RAM
// 0x20000800 a header {magic 'U8FW', flash offset, length, sum32} and the payload at 0x20000810.
// 256-byte fast pages (CH32V006): erase (FTER), program (BUF_RST, word + BUF_LOAD, FTPG|STRT),
// verify. Sequence as minichlink (ch32fun). Mailbox: [0] C0DE0009 running / C0DE00D9 ok /
// C0DEEEEE error, [1] step | page<<8, [2] STATR, [3] bad word count.
#include <stdint.h>
#define R32(a) (*(volatile uint32_t *)(a))
#define MBOX ((volatile uint32_t *)0x20000300u)
#define HDR  ((volatile uint32_t *)0x20000800u)
#define DATA ((const uint32_t *)0x20000810u)

#define KEYR     0x40022004u
#define STATR    0x4002200Cu
#define CTLR     0x40022010u
#define ADDR     0x40022014u
#define MODEKEYR 0x40022024u
#define K1 0x45670123u
#define K2 0xCDEF89ABu
#define C_STRT   0x00040u
#define C_FTPG   0x10000u
#define C_FTER   0x20000u
#define C_BUFLD  0x40000u
#define C_BUFRST 0x80000u

static int wait(void) {
    for (uint32_t i = 0; i < 5000000u; i++) if (!(R32(STATR) & 3)) return (R32(STATR) & 0x10) ? -1 : 0;
    return -2;
}
static void fail(uint32_t step) { MBOX[1] = step; MBOX[2] = R32(STATR); MBOX[0] = 0xC0DEEEEEu; for (;;) {} }

void main_fw(void) {
    MBOX[0] = 0xC0DE0009u; MBOX[1] = 0; MBOX[2] = 0; MBOX[3] = 0;
    uint32_t off = HDR[1], len = HDR[2], sum = 0;
    if (HDR[0] != 0x57463855u || (off & 0xFF) || !len || len > 0xF800 - off) fail(1);   // 'U8FW'
    for (uint32_t i = 0; i < (len + 3) / 4; i++) sum += DATA[i];
    if (sum != HDR[3]) fail(2);                          // payload damaged on the way
    R32(KEYR) = K1; R32(KEYR) = K2;
    R32(MODEKEYR) = K1; R32(MODEKEYR) = K2;              // fast page mode (FLOCK)
    if (R32(CTLR) & 0x8080) fail(3);
    uint32_t bad = 0;
    for (uint32_t p = 0; p < len; p += 256) {
        uint32_t a = 0x08000000u + off + p;
        MBOX[1] = 4 | (p >> 8) << 8;
        R32(CTLR) = C_FTER; R32(ADDR) = a; R32(CTLR) = C_FTER | C_STRT;   // erase page
        if (wait()) fail(5 | (p >> 8) << 8);
        R32(CTLR) = C_FTPG; R32(CTLR) = C_FTPG | C_BUFRST;                // program page
        if (wait()) fail(6 | (p >> 8) << 8);
        for (uint32_t w = 0; w < 64; w++) {
            uint32_t i = (p >> 2) + w;
            R32(a + w * 4) = (p + w * 4 < len) ? DATA[i] : 0xFFFFFFFFu;
            R32(CTLR) = C_FTPG | C_BUFLD;
            if (wait()) fail(7 | (p >> 8) << 8);
        }
        R32(ADDR) = a; R32(CTLR) = C_FTPG | C_STRT;
        if (wait()) fail(8 | (p >> 8) << 8);
        R32(CTLR) = 0;
        for (uint32_t w = 0; w < 64 && p + w * 4 < len; w++) if (R32(a + w * 4) != DATA[(p >> 2) + w]) bad++;
        MBOX[3] = bad;
    }
    R32(CTLR) = 0x80;                                    // lock again
    MBOX[2] = R32(STATR);
    MBOX[0] = bad ? 0xC0DEEEEEu : 0xC0DE00D9u;
    for (;;) {}
}

__attribute__((naked, section(".text.start"))) void _start(void) {
    __asm__ volatile("csrci mstatus, 8\n li sp, 0x20001F00\n j main_fw\n");
}
