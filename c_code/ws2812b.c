/* Copyright 2024 Grug Huhler.  License SPDX BSD-2-Clause.
*/

#include "std_int_types.h"
#include "ws2812b.h"

#define RGB_LED ((volatile uint32_t *) 0x80000020)

void set_ws2812b(uint32_t val)
{
  *RGB_LED = val;
}
