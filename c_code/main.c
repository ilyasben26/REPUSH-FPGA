/* Copyright 2025 Grug Huhler.  License SPDX BSD-2-Clause.

   This is a test program for the simple SoC based on the PicoRV32 core.
   It can be used on either the Tang Nano 9K or the Tang Nano 20K, but
   only the former contains user flash.
*/

#include "std_int_types.h"
#include "leds.h"
#include "uart.h"
#include "countdown_timer.h"
#include "readtime.h"
#include "sd_card_cgpt.h"
#include "monocypher.h"
#include "puf.h"
#if defined(BOARD_9K)
#include "uflash.h"
#include "xorshift32.h"
#elif defined(BOARD_20K)
#include "ws2812b.h"
#else
#error "BOARD NOT PROPERLY DEFINED.  Try make clean; make 9k|10k"
#endif

extern uint32_t timer_instr(uint32_t val);   /* from startup.S */
extern uint32_t maskirq_instr(uint32_t val); /* from startup.S */

#define ARDUINO_UART_DATA_REG ((volatile uint8_t *)0x8000005c)

#define PROTO_SOF1 0xA5
#define PROTO_SOF2 0x5A
#define PROTO_VERSION 1
#define PROTO_MAX_PAYLOAD 96

#define PROTO_MSG_REQ 1
#define PROTO_MSG_RSP 2
#define PROTO_MSG_ERR 3

#define PROTO_CMD_PING 1
#define PROTO_CMD_GET_INFO 2
#define PROTO_CMD_GET_TIME 3

#define PROTO_ERR_BAD_CRC 1
#define PROTO_ERR_BAD_LEN 2
#define PROTO_ERR_BAD_VERSION 3
#define PROTO_ERR_UNKNOWN_CMD 4

enum proto_parse_state
{
  PROTO_WAIT_SOF1 = 0,
  PROTO_WAIT_SOF2,
  PROTO_READ_HEADER,
  PROTO_READ_PAYLOAD,
  PROTO_READ_CRC_LO,
  PROTO_READ_CRC_HI
};

static uint8_t proto_state;
static uint8_t proto_header[6];
static uint8_t proto_payload[PROTO_MAX_PAYLOAD];
static uint16_t proto_payload_len;
static uint16_t proto_payload_index;
static uint8_t proto_header_index;
static uint16_t proto_rx_crc;
static uint8_t proto_rx_crc_lo;

uint16_t proto_crc16_update(uint16_t crc, uint8_t data)
{
  uint16_t i;

  crc ^= data;
  for (i = 0; i < 8; i++)
  {
    if (crc & 1)
      crc = (crc >> 1) ^ 0xA001;
    else
      crc = crc >> 1;
  }

  return crc;
}

void proto_reset_parser(void)
{
  proto_state = PROTO_WAIT_SOF1;
  proto_payload_len = 0;
  proto_payload_index = 0;
  proto_header_index = 0;
  proto_rx_crc = 0xffff;
  proto_rx_crc_lo = 0;
}

void proto_send_frame(uint8_t msg_type, uint8_t seq, uint8_t cmd, uint8_t *payload, uint16_t payload_len)
{
  uint16_t i;
  uint16_t crc;
  uint8_t b;

  crc = 0xffff;

  arduino_uart_putchar(PROTO_SOF1);
  arduino_uart_putchar(PROTO_SOF2);

  b = PROTO_VERSION;
  arduino_uart_putchar(b);
  crc = proto_crc16_update(crc, b);

  b = msg_type;
  arduino_uart_putchar(b);
  crc = proto_crc16_update(crc, b);

  b = seq;
  arduino_uart_putchar(b);
  crc = proto_crc16_update(crc, b);

  b = cmd;
  arduino_uart_putchar(b);
  crc = proto_crc16_update(crc, b);

  b = (uint8_t)(payload_len & 0xff);
  arduino_uart_putchar(b);
  crc = proto_crc16_update(crc, b);

  b = (uint8_t)((payload_len >> 8) & 0xff);
  arduino_uart_putchar(b);
  crc = proto_crc16_update(crc, b);

  for (i = 0; i < payload_len; i++)
  {
    arduino_uart_putchar(payload[i]);
    crc = proto_crc16_update(crc, payload[i]);
  }

  arduino_uart_putchar((uint8_t)(crc & 0xff));
  arduino_uart_putchar((uint8_t)((crc >> 8) & 0xff));
}

void proto_send_error(uint8_t seq, uint8_t cmd, uint8_t err)
{
  uint8_t payload[1];

  payload[0] = err;
  proto_send_frame(PROTO_MSG_ERR, seq, cmd, payload, 1);
}

void proto_dispatch_request(uint8_t msg_type, uint8_t seq, uint8_t cmd, uint8_t *payload, uint16_t payload_len)
{
  uint8_t info[8];
  uint32_t t;
  uint8_t time_payload[4];

  if (msg_type != PROTO_MSG_REQ)
    return;

  if (cmd == PROTO_CMD_PING)
  {
    proto_send_frame(PROTO_MSG_RSP, seq, cmd, payload, payload_len);
    return;
  }

  if (cmd == PROTO_CMD_GET_INFO)
  {
    info[0] = PROTO_VERSION;
    info[1] = (uint8_t)(PROTO_MAX_PAYLOAD & 0xff);
    info[2] = (uint8_t)((PROTO_MAX_PAYLOAD >> 8) & 0xff);
    info[3] = 0x07; /* bit0: ping, bit1: get_info, bit2: get_time */
    info[4] = 1;    /* fw major */
    info[5] = 0;    /* fw minor */
    info[6] = 0;
    info[7] = 0;
    proto_send_frame(PROTO_MSG_RSP, seq, cmd, info, sizeof(info));
    return;
  }

  if (cmd == PROTO_CMD_GET_TIME)
  {
    t = readtime();
    time_payload[0] = (uint8_t)(t & 0xff);
    time_payload[1] = (uint8_t)((t >> 8) & 0xff);
    time_payload[2] = (uint8_t)((t >> 16) & 0xff);
    time_payload[3] = (uint8_t)((t >> 24) & 0xff);
    proto_send_frame(PROTO_MSG_RSP, seq, cmd, time_payload, sizeof(time_payload));
    return;
  }

  proto_send_error(seq, cmd, PROTO_ERR_UNKNOWN_CMD);
}

void proto_feed_byte(uint8_t ch)
{
  uint16_t received_crc;

  switch (proto_state)
  {
  case PROTO_WAIT_SOF1:
    if (ch == PROTO_SOF1)
      proto_state = PROTO_WAIT_SOF2;
    break;

  case PROTO_WAIT_SOF2:
    if (ch == PROTO_SOF2)
    {
      proto_state = PROTO_READ_HEADER;
      proto_header_index = 0;
      proto_payload_index = 0;
      proto_payload_len = 0;
      proto_rx_crc = 0xffff;
    }
    else if (ch != PROTO_SOF1)
    {
      proto_state = PROTO_WAIT_SOF1;
    }
    break;

  case PROTO_READ_HEADER:
    proto_header[proto_header_index++] = ch;
    proto_rx_crc = proto_crc16_update(proto_rx_crc, ch);
    if (proto_header_index == sizeof(proto_header))
    {
      proto_payload_len = (uint16_t)proto_header[4] | ((uint16_t)proto_header[5] << 8);

      if (proto_header[0] != PROTO_VERSION)
      {
        proto_send_error(proto_header[2], proto_header[3], PROTO_ERR_BAD_VERSION);
        proto_reset_parser();
      }
      else if (proto_payload_len > PROTO_MAX_PAYLOAD)
      {
        proto_send_error(proto_header[2], proto_header[3], PROTO_ERR_BAD_LEN);
        proto_reset_parser();
      }
      else if (proto_payload_len == 0)
      {
        proto_state = PROTO_READ_CRC_LO;
      }
      else
      {
        proto_payload_index = 0;
        proto_state = PROTO_READ_PAYLOAD;
      }
    }
    break;

  case PROTO_READ_PAYLOAD:
    proto_payload[proto_payload_index++] = ch;
    proto_rx_crc = proto_crc16_update(proto_rx_crc, ch);
    if (proto_payload_index >= proto_payload_len)
      proto_state = PROTO_READ_CRC_LO;
    break;

  case PROTO_READ_CRC_LO:
    proto_rx_crc_lo = ch;
    proto_state = PROTO_READ_CRC_HI;
    break;

  case PROTO_READ_CRC_HI:
    received_crc = (uint16_t)proto_rx_crc_lo | ((uint16_t)ch << 8);
    if (received_crc == proto_rx_crc)
      proto_dispatch_request(proto_header[1], proto_header[2], proto_header[3], proto_payload, proto_payload_len);
    else
      proto_send_error(proto_header[2], proto_header[3], PROTO_ERR_BAD_CRC);
    proto_reset_parser();
    break;

  default:
    proto_reset_parser();
    break;
  }
}

void proto_poll_arduino_uart(void)
{
  uint8_t ch;

  while ((ch = *ARDUINO_UART_DATA_REG) != 0xff)
  {
    proto_feed_byte(ch);
  }
}

uint32_t timer_irq_count;
uint32_t illegal_irq_count;
uint32_t buserr_irq_count;
uint32_t irq3_count;

/* Keep at least 128 words so SD block read/write helpers (512 bytes)
  can use mem[] as a sector buffer. */
#ifndef MEMSIZE_WORDS
#define MEMSIZE_WORDS 128
#endif
uint32_t mem[MEMSIZE_WORDS];
uint32_t test_vals[] = {0, 0xffffffff, 0xaaaaaaaa, 0x55555555, 0xdeadbeef};

/* A simple memory test.  Delete this and also array mem
   above to free much of the SRAM for other things
*/

int mem_test(void)
{
  int i, test, errors;
  uint32_t val, val_read;

  errors = 0;
  for (test = 0; test < sizeof(test_vals) / sizeof(test_vals[0]); test++)
  {

    for (i = 0; i < MEMSIZE_WORDS; i++)
      mem[i] = test_vals[test];

    for (i = 0; i < MEMSIZE_WORDS; i++)
    {
      val_read = mem[i];
      if (val_read != test_vals[test])
        errors += 1;
    }
  }

  for (i = 0; i < MEMSIZE_WORDS; i++)
    mem[i] = i + (i << 17);

  for (i = 0; i < MEMSIZE_WORDS; i++)
  {
    val_read = mem[i];
    if (val_read != i + (i << 17))
      errors += 1;
  }

  return (errors);
}

void endian_test(void)
{
  volatile uint32_t test_loc = 0;
  volatile uint32_t *addr = &test_loc;
  volatile uint8_t *cp0, *cp3;
  char byte0, byte3;
  uint32_t i, ok;

  cp0 = (volatile uint8_t *)addr;
  cp3 = cp0 + 3;
  *addr = 0x44332211;
  byte0 = *cp0;
  byte3 = *cp3;
  *cp3 = 0xab;
  i = *addr;

  ok = (byte0 == 0x11) && (byte3 == 0x44) && (i == 0xab332211);
  uart_puts("\r\nEndian test: at ");
  uart_print_hex((uint32_t)addr);
  uart_puts(", byte0: ");
  uart_print_hex((uint32_t)byte0);
  uart_puts(", byte3: ");
  uart_print_hex((uint32_t)byte3);
  uart_puts(",\r\n     word: ");
  uart_print_hex(i);
  if (ok)
    uart_puts(" [PASSED]\r\n");
  else
    uart_puts(" [FAILED]\r\n");
}

/* la_functions are useful for looking at the bus using a logic
   analyzer.
*/

void la_wtest(void)
{
  uint32_t v;
  volatile uint32_t *ip = (volatile uint32_t *)&v;
  volatile uint16_t *sp = (volatile uint16_t *)&v;
  volatile uint8_t *cp = (volatile uint8_t *)&v;

  *ip = 0x03020100; // addr 0x00

  *sp = 0x0302;       // addr 0x00
  *(sp + 1) = 0x0100; // addr 0x02

  *cp = 0x03;       // addr 0x00
  *(cp + 1) = 0x02; // addr 0x01
  *(cp + 2) = 0x01; // addr 0x02
  *(cp + 3) = 0x00; // addr 0x03
}

void la_rtest(void)
{
  uint32_t v;
  volatile uint32_t *ip = (volatile uint32_t *)&v;
  volatile uint16_t *sp = (volatile uint16_t *)&v;
  volatile uint8_t *cp = (volatile uint8_t *)&v;

  *ip = 0x03020100; // addr 0x00

  *ip; // addr 0x00

  *sp;       // addr 0x00
  *(sp + 1); // addr 0x02

  *cp;       // addr 0x00
  *(cp + 1); // addr 0x01
  *(cp + 2); // addr 0x02
  *(cp + 3); // addr 0x03
}

void countdown_timer_test(void)
{
  uint32_t val;
  uint32_t test_errors = 0;

  // If register is little-endian, write to 0x80000013 should set
  // the MSB,  Does it?

  cdt_wbyte3(0xff);
  val = cdt_read();
  if ((val == 0xff000000) || (val < 0xfe000000))
    test_errors = 1;

  // Write zero to most significant half-word.
  cdt_whalf2(0);
  val = cdt_read();
  if (val > 0xffff)
    test_errors |= 2;

  uart_puts("Countdown timer test ");
  if (test_errors)
  {
    uart_puts("FAILED, mask = ");
    uart_print_hex(test_errors);
    uart_puts("\r\n");
  }
  else
  {
    uart_puts("PASSED\r\n");
  }
}

void cycle_delay(uint32_t cycles)
{
  uart_puts("delay ");
  uart_print_hex(cycles);
  uart_puts(" cycles\r\n");
  cdt_delay(cycles);
  uart_puts("done\r\n");
}

void read_led(void)
{
  uint8_t v;

  v = get_leds();
  uart_puts("LED = ");
  uart_print_hex(v);
  uart_puts("\r\n");
}

void incr_led(void)
{
  uint8_t v;

  v = get_leds();
  set_leds(v + 1);
}

void set_led(uint32_t value)
{
  set_leds(value);
}

void memory_test(void)
{
  if (mem_test())
    uart_puts("memory test FAILED.\r\n");
  else
    uart_puts("memory test PASSED.\r\n");
}

void read_clock(void)
{
  uart_puts("RC:");
  uart_print_hex(readtime());
  uart_puts("\r\n");
}

void read_clock_ll(void)
{
  time_ll_t t;

  t = readtime_ll();
  uart_puts("time is ");
  uart_print_hex(t.s.time_high);
  uart_putchar(':');
  uart_print_hex(t.s.time_low);
  uart_puts("\r\n");
}

#if defined(BOARD_9K)
void erase_all_flash(void)
{
  volatile uint8_t *p = (volatile uint8_t *)0x20000;
  time_ll_t start, end;
  uint32_t diff;
  int i;

  start = readtime_ll();
  for (i = 0; i < 38; i++)
  {
    *p = 0;
    p += 2048;
    uart_print_hex(i);
    uart_puts("\r\n");
  }
  end = readtime_ll();

  diff = end.time_val - start.time_val;
  uart_puts("done in ");
  uart_print_hex(diff);
  uart_puts(" cycles\r\n");
}

void write_all_flash(void)
{
  volatile uint32_t *p = (volatile uint32_t *)0x20000;
  uint32_t state = 1;
  time_ll_t start, end;
  uint32_t diff;
  int i;

  start = readtime_ll();
  for (i = 0; i < 19456; i++)
  {
    *p++ = xorshift32(&state);
  }
  end = readtime_ll();

  diff = end.time_val - start.time_val;
  uart_puts("done in ");
  uart_print_hex(diff);
  uart_puts(" cycles\r\n");
}

void check_all_flash(void)
{
  volatile uint32_t *p = (volatile uint32_t *)0x20000;
  uint32_t state = 1;
  int i, j, errors = 0;

  for (i = 0; i < 304; i++)
  {
    uart_print_hex(0x20000 + (i << 8)); /* 0x20000 + 4*64*i */
    uart_putchar(' ');
    for (j = 0; j < 64; j++)
    {
      if (*p++ != xorshift32(&state))
      {
        errors += 1;
        uart_putchar('x');
      }
      else
      {
        uart_putchar('.');
      }
    }
    uart_puts("\r\n");
  }

  uart_puts("errors = ");
  uart_print_hex(errors);
  uart_puts("\r\n");
}
#endif

void toggle_spi(void)
{
  sd_spi_use_helper = 1 - sd_spi_use_helper;
  if (sd_spi_use_helper)
    uart_puts("\r\nusing helper\r\n");
  else
    uart_puts("\r\nusing bit-bang\r\n");
}

void sd_card_init(void)
{
  if (sd_init() >= 0)
    uart_puts("\r\nSD init OK\r\n");
  else
    uart_puts("\r\nSD init failed\r\n");
}

void sd_display_block(uint32_t blk)
{
  uint8_t *buf = (uint8_t *)mem;
  int i;

  if (sd_read_block(blk, buf) < 0)
  {
    uart_puts("\r\nread failed\r\n");
    return;
  }

  for (i = 0; i < 512 / 4; i++)
  {
    uart_print_hex(mem[i]);
    if (i % 8 == 7)
      uart_puts("\r\n");
    else
      uart_putchar(' ');
  }
}

void sd_set_block(uint32_t blk, uint32_t start_val)
{
  uint8_t *buf = (uint8_t *)mem;
  int i;

  for (i = 0; i < 512 / 4; i++)
  {
    mem[i] = start_val++;
  }

  if (sd_write_block(blk, buf) < 0)
  {
    uart_puts("\r\nwrite failed\r\n");
    return;
  }
}

void hello_world(void)
{
  uart_puts("Hello World!");
  uart_puts("\r\n");
}

void hash_uart_line(void)
{
  char msg[64];
  uint8_t hash[32];
  uint32_t len;
  int i;

  uart_puts("text> ");
  len = uart_gets(msg, sizeof(msg));
  if (len == 0)
  {
    uart_puts("cancelled\r\n");
    return;
  }

  crypto_blake2b(hash, sizeof(hash), (const uint8_t *)msg, len);

  uart_puts("blake2b-256: ");
  for (i = 0; i < sizeof(hash); i++)
  {
    uart_print_hex_byte(hash[i]);
  }
  uart_puts("\r\n");
}

void help(void)
{
  uart_puts("ct            : test countdown timer\r\n");
  uart_puts("dc cycles     : delay for cycles\r\n");
#if defined(BOARD_9K)
  uart_puts("cf            : check that uflash values match wf\r\n");
  uart_puts("ef addr       : erase page of uflash\r\n");
  uart_puts("af            : erase all uflash\r\n");
  uart_puts("wf            : write all uflash\r\n");
#endif
  uart_puts("et            : endian test\r\n");
  uart_puts("rl            : read LEDs\r\n");
  uart_puts("il            : increment LEDs\r\n");
  uart_puts("sl value      : set LEDs to value\r\n");
  uart_puts("mt            : memory test\r\n");
  uart_puts("rc            : read clock low\r\n");
  uart_puts("rd            : read clock hi:low\r\n");
  uart_puts("he            : print help\r\n");
  uart_puts("si            : soft reset SD card-- do first\r\n");
  uart_puts("sr            : read specified block from SD\r\n");
  uart_puts("sw            : write specified block of SD\r\n");
  uart_puts("sx            : Toggle using SD SPI helper\r\n");
  uart_puts("bi            : cause bus error\r\n");
  uart_puts("ii            : cause illegal instruction\r\n");
  uart_puts("mi            : set IRQ mask\r\n");
  uart_puts("pi            : print IRQ count\r\n");
  uart_puts("ti            : set timer to val\r\n");
#if defined(BOARD_20K)
  uart_puts("sn            : set ws2812b LED\r\n");
#endif
  uart_puts("rb addr       : read byte\r\n");
  uart_puts("rh addr       : read half word\r\n");
  uart_puts("rw addr       : read word\r\n");
  uart_puts("wb addr value : write byte\r\n");
  uart_puts("wh addr value : write half word\r\n");
  uart_puts("ww addr value : write word\r\n");
  uart_puts("hb            : hash input text with blake2b-256\r\n");
  uart_puts("hl            : print Hello World\r\n");
  uart_puts("pr            : PUF single measurement (hi:lo hex)\r\n");
  uart_puts("pt upper lower: PUF set ASR tuning (each 0..7)\r\n");
  uart_puts("pc top bottom : PUF set choice + patterns (top>bottom, 0..3)\r\n");
  uart_puts("pq count      : PUF majority vote over count samples\r\n");
  uart_puts("ph tc,tt,bc,bt: PUF challenge (top_choice,top_tune,bottom_choice,bottom_tune)\r\n");
  uart_puts("pa            : scan all 384 PUF challenges, count non-all-1s\r\n");
  uart_puts("ps            : save valid challenges to SD card (run si first)\r\n");
  uart_puts("lc            : load valid challenges from SD card into memory\r\n");
  uart_puts("pk            : query PUF for a typed string (maps string -> challenge)\r\n");
  uart_puts("rs state seed : reconfigure LR-PUF state (0-10) with external seed\r\n");
  uart_puts("cl state chal : perform LR-PUF challenge using state\r\n");
  uart_puts("ss            : save LR-PUF states to SD card\r\n");
  uart_puts("ls            : load LR-PUF states from SD card\r\n");
  uart_puts("sd state      : set domain name for LR-PUF state (interactively asks for string)\r\n");
  uart_puts("pd state      : print domain name for LR-PUF state\r\n");
  uart_puts("   all numbers are hex\r\n");
}

void read_byte(uint32_t addr)
{
  volatile uint8_t *p = (volatile uint8_t *)addr;

  uart_print_hex(*p);
  uart_puts("\r\n");
}

void read_half(uint32_t addr)
{
  volatile uint16_t *p = (volatile uint16_t *)addr;

  uart_print_hex(*p);
  uart_puts("\r\n");
}

void read_word(uint32_t addr)
{
  volatile uint32_t *p = (volatile uint32_t *)addr;

  uart_print_hex(*p);
  uart_puts("\r\n");
}

void write_byte(uint32_t addr, uint32_t value)
{
  volatile uint8_t *p = (volatile uint8_t *)addr;

  *p = value;
}

void write_half(uint32_t addr, uint32_t value)
{
  volatile uint16_t *p = (volatile uint16_t *)addr;

  *p = value;
}

void write_word(uint32_t addr, uint32_t value)
{
  volatile uint32_t *p = (volatile uint32_t *)addr;

  *p = value;
}

#if defined(BOARD_20K)
void change_ws2812b(uint32_t value)
{
  uart_puts("setting neopixel to ");
  uart_print_hex(value);
  uart_puts("\r\n");
  set_ws2812b(value);
}
#endif

uint32_t *irq(uint32_t *regs, uint32_t irqs)
{
  if ((irqs & 1) != 0)
  {
    timer_irq_count++;
  }

  if ((irqs & 2) != 0)
  {
    illegal_irq_count++;
  }

  if ((irqs & 4) != 0)
  {
    buserr_irq_count++;
  }

  if ((irqs & 8) != 0)
  {
    irq3_count++;
  }

  return regs;
}

void print_irq_counts(void)
{
  uart_puts("timer: ");
  uart_print_hex(timer_irq_count);
  uart_puts("\r\nillegal: ");
  uart_print_hex(illegal_irq_count);
  uart_puts("\r\nbus error: ");
  uart_print_hex(buserr_irq_count);
  uart_puts("\r\nIRQ3: ");
  uart_print_hex(irq3_count);
  uart_puts("\r\n");
}

void do_timer_instr(uint32_t val)
{
  uint32_t old_val;

  old_val = timer_instr(val);
  uart_puts("old value was ");
  uart_print_hex(old_val);
  uart_puts("\r\n");
}

void do_maskirq_instr(uint32_t val)
{
  uint32_t old_val;

  old_val = maskirq_instr(val);
  uart_puts("old value was ");
  uart_print_hex(old_val);
  uart_puts("\r\n");
}

void illegal(void)
{
  asm volatile("unimp"); /* Illegal instruction */
}

void buserr(void)
{
  int x, v;
  int *p = &x;

  /* If the compiler detects a misaligned access, it breaks it into a
     sequence of smaller aligned accesses so I use an asm to force a
     misaligned short read */
  asm volatile("lh %[rd], 1(%[rs])" : [rd] "=r"(v) : [rs] "r"(p));
}

/* struct command lists available commands.  See function help.
 * Each function takes one or two arguments.  The table below
 * contains a pointer to the function to call for each command.
 */

struct command
{
  char *cmd_string;
  int num_args;
  union
  {
    void (*func0)(void);
    void (*func1)(uint32_t val);
    void (*func2)(uint32_t val1, uint32_t val2);
    void (*func4)(uint32_t val1, uint32_t val2, uint32_t val3, uint32_t val4);
  } u;
} commands[] = {
    {"ct", 0, .u.func0 = countdown_timer_test},
    {"dc", 1, .u.func1 = cycle_delay}, // cycles
#if defined(BOARD_9K)
    {"ef", 1, .u.func1 = erase_page_uflash}, // addr
    {"cf", 0, .u.func0 = check_all_flash},
    {"af", 0, .u.func0 = erase_all_flash},
    {"wf", 0, .u.func0 = write_all_flash},
#endif
    {"et", 0, .u.func0 = endian_test},
    {"rl", 0, .u.func0 = read_led},
    {"il", 0, .u.func0 = incr_led},
    {"sl", 1, .u.func1 = set_led}, // val
    {"mt", 0, .u.func0 = memory_test},
    {"rc", 0, .u.func0 = read_clock},
    {"rd", 0, .u.func0 = read_clock_ll},
    {"he", 0, .u.func0 = help},
    {"sx", 0, .u.func0 = toggle_spi},
    {"si", 0, .u.func0 = sd_card_init},
    {"sr", 1, .u.func1 = sd_display_block},
    {"sw", 2, .u.func2 = sd_set_block},
    {"bi", 0, .u.func0 = buserr},
    {"ii", 0, .u.func0 = illegal},
    {"mi", 1, .u.func1 = do_maskirq_instr},
    {"pi", 0, .u.func0 = print_irq_counts},
    {"ti", 1, .u.func1 = do_timer_instr},
#if defined(BOARD_20K)
    {"sn", 1, .u.func1 = change_ws2812b},
#endif
    {"rb", 1, .u.func1 = read_byte},  // addr
    {"rh", 1, .u.func1 = read_half},  // addr
    {"rw", 1, .u.func1 = read_word},  // addr
    {"wb", 2, .u.func2 = write_byte}, // addr, val
    {"wh", 2, .u.func2 = write_half}, // addr, val
    {"ww", 2, .u.func2 = write_word}, // addr, val
    {"hb", 0, .u.func0 = hash_uart_line},
    {"hl", 0, .u.func0 = hello_world},
    {"pr", 0, .u.func0 = puf_request},
    {"pt", 2, .u.func2 = puf_set_tune},
    {"pc", 2, .u.func2 = puf_set_choice},
    {"pq", 1, .u.func1 = puf_measure},
    {"ph", 4, .u.func4 = puf_challenge},
    {"pa", 0, .u.func0 = puf_scan},
    {"ps", 0, .u.func0 = puf_save},
    {"lc", 0, .u.func0 = puf_load_challenges},
    {"pk", 0, .u.func0 = puf_key},
    {"ss", 0, .u.func0 = puf_save_states},
    {"ls", 0, .u.func0 = puf_load_states},
    {"sd", 1, .u.func1 = puf_set_domain},
    {"pd", 1, .u.func1 = puf_print_domain},
    {"rs", 2, .u.func2 = puf_reconfigure_state},
    {"cl", 2, .u.func2 = puf_challenge_lr}};

void eat_spaces(char **buf, uint32_t *len)
{
  while (len > 0)
  {
    if (**buf == ' ')
    {
      *buf += 1;
      *len -= 1;
    }
    else
      break;
  }
}

/* Skip spaces and commas — used between 4-argument commands. */
void eat_delimiters(char **buf, uint32_t *len)
{
  while (*len > 0)
  {
    if (**buf == ' ' || **buf == ',')
    {
      *buf += 1;
      *len -= 1;
    }
    else
      break;
  }
}

/* Returns 1 if a number found, else 0.  Number is in *v */

uint32_t get_hex(char **buf, uint32_t *len, uint32_t *v)
{
  int valid = 0;
  int keep_going;
  char ch;

  keep_going = 1;

  *v = 0;
  while (keep_going && (*len > 0))
  {

    ch = **buf;
    *buf += 1;
    *len -= 1;

    if ((ch >= '0') && (ch <= '9'))
    {
      *v = 16 * (*v) + (ch - '0');
      valid = 1;
    }
    else if ((ch >= 'a') && (ch <= 'f'))
    {
      *v = 16 * (*v) + (ch - 'a' + 10);
      valid = 1;
    }
    else if ((ch >= 'A') && (ch <= 'F'))
    {
      *v = 16 * (*v) + (ch - 'A' + 10);
      valid = 1;
    }
    else
    {
      keep_going = 0;
    }
  }

  return valid;
}

void parse(char *buf, uint32_t len)
{
  int i, cmd_not_ok;
  uint32_t val1, val2, val3, val4;

  cmd_not_ok = 1;
  eat_spaces(&buf, &len);
  if (len < 2)
    goto err;

  for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
    if ((buf[0] == commands[i].cmd_string[0]) && (buf[1] == commands[i].cmd_string[1]))
    {
      buf += 2;
      len -= 2;
      switch (commands[i].num_args)
      {
      case 0:
        commands[i].u.func0();
        cmd_not_ok = 0;
        break;
      case 1:
        eat_spaces(&buf, &len);
        if (get_hex(&buf, &len, &val1))
        {
          commands[i].u.func1(val1);
          cmd_not_ok = 0;
        }
        break;
      case 2:
        eat_spaces(&buf, &len);
        if (get_hex(&buf, &len, &val1))
        {
          eat_spaces(&buf, &len);
          if (get_hex(&buf, &len, &val2))
          {
            commands[i].u.func2(val1, val2);
            cmd_not_ok = 0;
          }
        }
        break;
      case 4:
        eat_delimiters(&buf, &len);
        if (get_hex(&buf, &len, &val1))
        {
          eat_delimiters(&buf, &len);
          if (get_hex(&buf, &len, &val2))
          {
            eat_delimiters(&buf, &len);
            if (get_hex(&buf, &len, &val3))
            {
              eat_delimiters(&buf, &len);
              if (get_hex(&buf, &len, &val4))
              {
                commands[i].u.func4(val1, val2, val3, val4);
                cmd_not_ok = 0;
              }
            }
          }
        }
        break;
      default:
        break;
      }
      break;
    }

err:
  if (cmd_not_ok)
    uart_puts("illegal command, he for help\r\n");
}

#define BUFLEN 64

int main()
{
  char buf[BUFLEN];
  uint32_t len;

  set_leds(6);

  timer_irq_count = 0;
  illegal_irq_count = 0;
  buserr_irq_count = 0;
  irq3_count = 0;

  la_wtest(); /* Could delete these.  They produce transactions that are */
  la_rtest(); /* nice to view on a logic analyzer. */

  uart_set_div(CLK_FREQ / 115200.0 + 0.5);
  arduino_uart_set_div(CLK_FREQ / 115200.0 + 0.5);
  uart_set_arduino_mirror(0);
  proto_reset_parser();

  uart_puts("\r\nStarting, CLK_FREQ: 0x");
  uart_print_hex(CLK_FREQ);
  uart_puts("\r\nenter he for a list of commands\r\n");

  uart_puts("Initializing SD card...\r\n");
  sd_card_init();

  while (1)
  {
    /* Poll main UART */
    if (*((volatile uint8_t *)0x8000000c) != 0xff)
    {
      len = uart_gets(buf, BUFLEN);
      if (len != 0)
        parse(buf, len);
    }

    /* Poll Arduino UART */
    proto_poll_arduino_uart();
  }

  return 0;
}
