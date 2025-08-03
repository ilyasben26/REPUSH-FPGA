#ifndef COUNTDOWN_TIMER_H
#define COUNTDOWN_TIMER_H

/* Copyright 2024 Grug Huhler
 *
 * License: SPDX BSD-2-Clause.
 */

extern void cdt_wbyte0(const uint8_t value);
extern void cdt_wbyte1(const uint8_t value);
extern void cdt_wbyte2(const uint8_t value);
extern void cdt_wbyte3(const uint8_t value);

extern void cdt_whalf0(const uint16_t value);
extern void cdt_whalf2(const uint16_t value);

extern void cdt_write(const uint32_t value);
extern uint32_t cdt_read(void);
extern void cdt_delay(const uint32_t value);
#endif

