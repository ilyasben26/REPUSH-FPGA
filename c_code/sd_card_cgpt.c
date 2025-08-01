/* Copyright 2025 Grug Huhler
 *
 * License: SPDX BSD-2-Clause.
 * Some of this code was written by chatgpt based on my prompt and then
 * fixed by me.
 *
 * This code uses SD cards' SPI mode to allow 1) SD card initialization
 * 2) Single 512 byte block read, and 3) single 512 byte block write.
 *
 * This software uses an "SD card SPI helper" function implemented in
 * hardware on the FPGA.  The helper:
 *   1. Allows SPI signals CS#, CLK, MOSI to be written and
 *   2. MISO to read.  This functionality allows an implementation using
 *      just software bit-banging.
 *   3. But the helper hardware allows an 8-bit SPI transaction to be
 *      done in hardware.  Software just writes the byte to be sent to the
 *      slave to a register, and the transaction happens.  Hardware also
 *      captures whatever the slave sends on MISO.  Software can read a
 *      register to get this response value.
 *
 * The spi_transfer function has two implementations: spi_transfer_b uses
 * bit-banging.  This yields a purely bit-banged implementation. And
 * spi_transfer_h uses the helper hardware.  This yields a hybrid
 * implementation that uses software and just a little bit-banging.  This
 * significantly speeds the performance of block reads and writes, especially
 * when the PicoRV32 core speed to set low.
 *
 * The pins used for SPI (CS, CLK, MISO, MOSI) are determined by the
 * Verilog FPGA image.  They depend on the board's schematic, i.e. the
 * signal connections from the FPGA to the micro SD slot.
 *
 * Do an sd_init before block reads/writes.
 *
 * Extremely old (prior to SDC version 2) micro SD cards may not work. Also,
 * Modern SDUC cards may not work. I don't have one.  I read that SDUC does
 * not support SPI mode.  I have one 128 MByte SD card that fails.  My SD
 * cards with sizes from 4 GBytes to 64 GBytes work.
 *
 * I doubt this code is fully robust.  The SD specification is VERY complex,
 * and some of this code is just from Chat GPT.
 *
 */

#include <stdint.h>
#include "sd_card_cgpt.h"
#include "countdown_timer.h"

uint8_t sd_spi_use_helper = 1;

/* platform-specific access functions to the SPI helper */

#define SD_SPI_TX_REG ((volatile uint8_t *) 0x80000030)
#define SD_SPI_RX_REG ((volatile uint8_t *) 0x80000034)
#define SD_SPI_CMD_REG ((volatile uint8_t *) 0x80000038)

void spi_cs_hi()
{
  *SD_SPI_CMD_REG |= 4;
}

void spi_cs_lo()
{
  *SD_SPI_CMD_REG &= ~4;
}

void spi_clk_hi()
{
  *SD_SPI_CMD_REG |= 1;
}

void spi_clk_lo()
{
  *SD_SPI_CMD_REG &= ~1;
}

void spi_mosi_hi()
{
  *SD_SPI_CMD_REG |= 2;
}

void spi_mosi_lo()
{
  *SD_SPI_CMD_REG &= ~2;
}

uint8_t spi_miso_read()
{
  if ((*SD_SPI_CMD_REG) & 0x8)
    return 1;
  else
    return 0;
}

/* delay functions may delay longer than their argument calls for, especially
 * delay_us */

void delay_us(uint32_t us)
{
  cdt_delay(us*CLK_FREQ/1000000);
}

void delay_ms(uint32_t ms)
{
  cdt_delay(ms*CLK_FREQ/1000);
}

/* send a byte over MOSI, reading MISO */

static uint8_t spi_transfer_b(uint8_t b)
{
  uint8_t reply = 0;
  for (int i = 0; i < 8; i++) {
    if (b & 0x80) spi_mosi_hi(); else spi_mosi_lo();
    b <<= 1;
    spi_clk_hi();
    delay_us(3);
    reply <<= 1;
    if (spi_miso_read()) reply |= 1;
    spi_clk_lo();
    delay_us(3);
  }
  return reply;
}

static uint8_t spi_transfer_h(uint8_t b)
{
  *SD_SPI_TX_REG = b;
  while (*SD_SPI_CMD_REG & 0x10) { /* await not busy */ }

  return *SD_SPI_RX_REG;
}

static uint8_t spi_transfer(uint8_t b)
{
  if (sd_spi_use_helper) return spi_transfer_h(b);
  else return spi_transfer_b(b);
}

/* send command; read R1 response */
static uint8_t sd_cmd(uint8_t cmd, uint32_t arg, uint8_t crc)
{
  spi_cs_lo();
  spi_transfer(0xFF);
  spi_transfer(0x40 | cmd);
  spi_transfer((arg >> 24) & 0xFF);
  spi_transfer((arg >> 16) & 0xFF);
  spi_transfer((arg >> 8) & 0xFF);
  spi_transfer(arg & 0xFF);
  spi_transfer(crc);
  /* await for a response */
  for (int i = 0; i < 8; i++) {
    uint8_t r = spi_transfer(0xFF);
    if ((r & 0x80) == 0) return r;
  }
  return 0xFF;
}

/* initialize SD card into SPI mode, return 0 = success */
int sd_init(void)
{
  uint8_t r;

  /* Initial state */
  spi_cs_hi();
  spi_clk_lo();
  spi_mosi_lo();
  delay_ms(1);

  // ensure CS high and at least 74 clocks
  spi_cs_hi();
  for (int i = 0; i < 10; i++) spi_transfer(0xFF);
  delay_ms(3);

  /* CMD0 – reset to idle */
  r = sd_cmd(0, 0, 0x95);
  spi_transfer(0xFF);
  if (r != 0x01) return -1;

  /* CMD8 – send interface condition.  I think the real
   * point of this is to await intialization completion.
   * The Tang Nano 9l/20k boards do not support SD operation at
   * anything other than 3.3V
   */
  r = sd_cmd(8, 0x1AA, 0x87);
  if (r == 0x01) {
    // read back echo bytes
    uint8_t buf[4];
    for (int i = 0; i < 4; i++) buf[i] = spi_transfer(0xFF);
    if ((buf[2] != 0x01) || (buf[3] != 0xAA)) return -2;
    // now ACMD41 with HCS
    do {
      sd_cmd(55, 0, 0x01);
      spi_transfer(0xFF);
      r = sd_cmd(41, 0x40000000, 0x01);
      spi_transfer(0xFF);
      delay_ms(1);
    } while (r != 0x00);
  } else {
    // Possibly an older card for which r == 0x5.  In
    // any case, we don't support it.
    return -3;
  }

  // CMD16 – set block length to 512 bytes
  r = sd_cmd(16, 512, 0x01);
  spi_transfer(0xFF);
  if (r != 0x00) return -4;

  spi_cs_hi();
  return 0;
}

/* read one 512‑byte block into buf */
int sd_read_block(uint32_t block_addr, uint8_t *buf)
{
  uint8_t r = sd_cmd(17, block_addr, 0x01);
  spi_transfer(0xFF);
  if (r != 0x00) {
    spi_cs_hi(); return -1;
  }
  // wait for data token (0xFE)
  for (uint32_t i = 0; i < 100000; i++) {
    uint8_t token = spi_transfer(0xFF);
    if (token == 0xFE) {
      // read data
      for (int j = 0; j < 512; j++) buf[j] = spi_transfer(0xFF);
      // read and discard CRC
      spi_transfer(0xFF);
      spi_transfer(0xFF);
      spi_cs_hi();
      spi_transfer(0xFF);
      return 0;
    }
  }
  spi_cs_hi();
  return -2;
}

/* write one 512‑byte block from buf */
int sd_write_block(uint32_t block_addr, const uint8_t *buf)
{
  uint8_t r = sd_cmd(24, block_addr, 0x01);
  spi_transfer(0xFF);
  if (r != 0x00) {
    spi_cs_hi(); return -1;
  }
  spi_transfer(0xFF);
  spi_transfer(0xFE);  // data token
  // send data
  for (int j = 0; j < 512; j++) spi_transfer(buf[j]);
  // dummy CRC
  spi_transfer(0xFF);
  spi_transfer(0xFF);
  // read data response token
  uint8_t resp = spi_transfer(0xFF);
  if ((resp & 0x1F) != 0x05) {
    spi_cs_hi(); return -2;
  }
  // wait until card finishes writing (busy = 0x00)
  while (spi_transfer(0xFF) == 0x00);
  spi_cs_hi();
  spi_transfer(0xFF);
  return 0;
}
