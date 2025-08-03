/* Copyright 2024 Grug Huhler.  License SPDX BSD-2-Clause.
*/

#include <stdint.h>
#include "leds.h"

#define LEDS ((volatile uint8_t *) 0x80000000)

void set_leds(uint8_t val)
{
  *LEDS = val;
}

uint8_t get_leds(void)
{
  return *LEDS;
}
