// U8 RAM program: remove read-out protection from U8 (CH32V006). THIS ERASES U8's FLASH (the
// stock U8 firmware, which never initialises anything on this board). Same sequence as
// minichlink's unbrick (ch32fun): unlock keys, erase option bytes, program a fresh option image
// (RDPR = 0xA5 = unprotected, USER = 0xF7 = the value this board had), then mass erase.
// The new option bytes are loaded at the next power-up (unplug the keyboard once).
// Mailbox: [0] C0DE0008 running / C0DE00D8 done / C0DEEEEE error, [1] step, [2] STATR, [3] OBR.
#include <stdint.h>
#define R32(a) (*(volatile uint32_t *)(a))
#define R16(a) (*(volatile uint16_t *)(a))
#define MBOX ((volatile uint32_t *)0x20000300u)

#define KEYR    0x40022004u
#define OBKEYR  0x40022008u
#define STATR   0x4002200Cu
#define CTLR    0x40022010u
#define OBR     0x4002201Cu
#define MODEKEYR 0x40022024u
#define BOOTKEYR 0x40022028u
#define K1 0x45670123u
#define K2 0xCDEF89ABu
#define C_MER   0x004u
#define C_OPTPG 0x010u
#define C_OPTER 0x020u
#define C_STRT  0x040u
#define C_OPTWRE 0x200u

static const uint16_t OPT[8] = {0x5AA5, 0x08F7, 0x00FF, 0x00FF, 0x00FF, 0x00FF, 0x00FF, 0x00FF};

static int wait(void) {
    for (uint32_t i = 0; i < 20000000u; i++) if (!(R32(STATR) & 3)) return (R32(STATR) & 0x10) ? -1 : 0;
    return -2;
}
static void fail(uint32_t step) { MBOX[1] = step; MBOX[2] = R32(STATR); MBOX[3] = R32(OBR); MBOX[0] = 0xC0DEEEEEu; for (;;) {} }

void main_unlock(void) {
    MBOX[0] = 0xC0DE0008u;
    MBOX[1] = 1; MBOX[3] = R32(OBR);
    R32(KEYR) = K1; R32(KEYR) = K2;
    R32(OBKEYR) = K1; R32(OBKEYR) = K2;
    R32(MODEKEYR) = K1; R32(MODEKEYR) = K2;
    R32(BOOTKEYR) = K1; R32(BOOTKEYR) = K2;
    if (R32(CTLR) & 0x80) fail(2);                       // still LOCKed
    if (!(R32(CTLR) & C_OPTWRE)) fail(3);                // option bytes not writable
    MBOX[1] = 4;
    R32(CTLR) = C_OPTER | C_OPTWRE;                      // erase option bytes
    R32(CTLR) = C_OPTER | C_OPTWRE | C_STRT;
    if (wait()) fail(5);
    for (int i = 0; i < 8; i++) {                        // program the 8 half-words
        MBOX[1] = 0x10 + i;
        R32(CTLR) = C_OPTPG | C_OPTWRE;
        R32(CTLR) = C_OPTPG | C_OPTWRE | C_STRT;
        R16(0x1FFFF800u + 2 * i) = OPT[i];
        if (wait()) fail(0x20 + i);
    }
    R32(CTLR) = 0;
    MBOX[1] = 0x30;
    R32(CTLR) = C_MER;                                   // mass erase of the main flash
    R32(CTLR) = C_MER | C_STRT;
    if (wait()) fail(0x31);
    R32(CTLR) = 0;
    MBOX[1] = 0x40; MBOX[2] = R32(STATR); MBOX[3] = R32(OBR);
    MBOX[0] = 0xC0DE00D8u;
    for (;;) {}
}

__attribute__((naked, section(".text.start"))) void _start(void) {
    __asm__ volatile("csrci mstatus, 8\n li sp, 0x20000F00\n j main_unlock\n");
}
