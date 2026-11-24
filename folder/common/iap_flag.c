// DataFlash flag shared with the USB IAP updater (firmware/iap). Values must match iap/iap.h.
#include "CH58x_common.h"

#define IAP_FLAG_DATAFLASH_ADD 0
#define FLAG_USER_CALL_IAP     0x55
#define FLAG_USER_CALL_APP     0xaa

static void set_flag(uint8_t v) {
    __attribute__((aligned(4))) uint8_t buf[4];
    EEPROM_READ(IAP_FLAG_DATAFLASH_ADD, (PUINT32)buf, 4);
    if (buf[0] == v) return;
    EEPROM_ERASE(IAP_FLAG_DATAFLASH_ADD, EEPROM_BLOCK_SIZE);   // whole block: see keyboard/rgb.c
    buf[0] = v; buf[1] = buf[2] = buf[3] = 0xFF;
    EEPROM_WRITE(IAP_FLAG_DATAFLASH_ADD, (PUINT32)buf, 4);
}

// Call early in main(): we are running, so a pending update request is done.
void iap_flag_clear(void) { set_flag(FLAG_USER_CALL_APP); }

// Reboot into the USB updater (it stays in update mode until a new firmware is written).
void iap_request_update(void) {
    uint32_t irq;
    set_flag(FLAG_USER_CALL_IAP);
    SYS_DisableAllIrq(&irq);
    SYS_ResetExecute();
}
