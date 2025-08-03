/* Copyright 2024 Grug Huhler
 *
 * License: SPDX BSD-2-Clause.
 */

#include "std_int_types.h"
#include "countdown_timer.h"

#define CDT_COUNTER ((volatile uint32_t *) 0x80000010)
#define CDT_COUNTER_H0 ((volatile uint16_t *) 0x80000010)
#define CDT_COUNTER_H2 ((volatile uint16_t *) 0x80000012)
#define CDT_COUNTER_B0 ((volatile uint8_t *) 0x80000010)
#define CDT_COUNTER_B1 ((volatile uint8_t *) 0x80000011)
#define CDT_COUNTER_B2 ((volatile uint8_t *) 0x80000012)
#define CDT_COUNTER_B3 ((volatile uint8_t *) 0x80000013)

void cdt_wbyte0(const uint8_t value)
{
  *CDT_COUNTER_B0 = value;
}

void cdt_wbyte1(const uint8_t value)
{
  *CDT_COUNTER_B1 = value;
}

void cdt_wbyte2(const uint8_t value)
{
  *CDT_COUNTER_B2 = value;
}

void cdt_wbyte3(const uint8_t value)
{
  *CDT_COUNTER_B3 = value;
}

void cdt_whalf0(const uint16_t value)
{
  *CDT_COUNTER_H0 = value;
}

void cdt_whalf2(const uint16_t value)
{
  *CDT_COUNTER_H2 = value;
}

void cdt_write(const uint32_t value)
{
  *CDT_COUNTER = value;
}

uint32_t cdt_read(void)
{
  return *CDT_COUNTER;
}

void cdt_delay(const uint32_t value)
{
  cdt_write(value);
  while (*CDT_COUNTER) {}
}
