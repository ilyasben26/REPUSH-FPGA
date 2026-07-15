/* Copyright 2024 Grug Huhler.  License SPDX BSD-2-Clause.
 */

#include "std_int_types.h"
#include "uart.h"

#define UART_DIV ((volatile uint32_t *)0x80000008)
#define UART_DATA ((volatile uint8_t *)0x8000000c)
#define ARDUINO_UART_DIV ((volatile uint32_t *)0x80000058)
#define ARDUINO_UART_DATA ((volatile uint8_t *)0x8000005c)
#define ARDUINO_UART_DATA32 ((volatile uint32_t *)0x8000005c)

static uint32_t arduino_mirror_enabled = 1;

void uart_set_arduino_mirror(uint32_t enable)
{
  arduino_mirror_enabled = (enable != 0);
}

void uart_set_div(uint32_t div)
{
  volatile int delay;

  *UART_DIV = div;

  /* Need to delay a little */
  for (delay = 0; delay < 200; delay++)
  {
  }
}

void uart_print_hex(uint32_t val)
{
  char ch;
  int i;

  for (i = 0; i < 8; i++)
  {
    ch = (val & 0xf0000000) >> 28;
    *UART_DATA = "0123456789abcdef"[ch];
    if (arduino_mirror_enabled)
      *ARDUINO_UART_DATA = "0123456789abcdef"[ch];
    val = val << 4;
  }
}

char uart_getchar(void)
{
  uint8_t ch;

  /* UART gives 0xff when empty */
  while ((ch = *UART_DATA) == 0xff)
  {
  }

  return (ch);
}

void uart_putchar(char ch)
{
  *UART_DATA = ch;
  if (arduino_mirror_enabled)
    *ARDUINO_UART_DATA = ch;
}

void uart_puts(char *s)
{
  while (*s != 0)
  {
    *UART_DATA = *s;
    if (arduino_mirror_enabled)
      *ARDUINO_UART_DATA = *s;
    s++;
  }
}

void uart_print_hex_byte(uint8_t v)
{
  char ch;
  ch = (v >> 4) & 0xf;
  uart_putchar("0123456789abcdef"[(uint8_t)ch]);
  ch = v & 0xf;
  uart_putchar("0123456789abcdef"[(uint8_t)ch]);
}

uint32_t uart_gets(char *buf, uint32_t buf_len)
{
  uint32_t i;
  char ch;

  for (i = 0; i < buf_len; i++)
    buf[i] = 0;

  i = 0;
  while ((i < buf_len - 1) && ((ch = uart_getchar()) != '\r'))
  {
    if ((ch == 3) || (ch == 0x7f) || (ch == 8))
    {
      uart_puts("\r\ncancelled\r\n");
      return 0;
    }
    uart_putchar(ch);
    *buf++ = ch;
    i += 1;
  }
  uart_puts("\r\n");

  return i;
}

uint32_t uart_get_hex(void)
{
  uint32_t v;
  int keep_going;
  char ch;

  keep_going = 1;

  v = 0;
  while (keep_going)
  {

    ch = uart_getchar();

    if ((ch >= '0') && (ch <= '9'))
    {
      v = 16 * v + (ch - '0');
      uart_putchar(ch);
    }
    else if ((ch >= 'a') && (ch <= 'f'))
    {
      v = 16 * v + (ch - 'a' + 10);
      uart_putchar(ch);
    }
    else if ((ch >= 'A') && (ch <= 'F'))
    {
      v = 16 * v + (ch - 'A' + 10);
      uart_putchar(ch);
    }
    else if (ch == '\r')
    {
      uart_putchar('\n');
      keep_going = 0;
    }
  }

  return v;
}

void arduino_uart_set_div(uint32_t div)
{
  volatile int delay;
  *ARDUINO_UART_DIV = div;
  for (delay = 0; delay < 200; delay++)
  {
  }
}

void arduino_uart_print_hex(uint32_t val)
{
  char ch;
  int i;
  for (i = 0; i < 8; i++)
  {
    ch = (val & 0xf0000000) >> 28;
    *ARDUINO_UART_DATA = "0123456789abcdef"[ch];
    val = val << 4;
  }
}

char arduino_uart_getchar(void)
{
  uint32_t word;
  /* See proto_poll_arduino_uart() in main.c: only the full 32-bit word
   * distinguishes "no data" (0xffffffff) from a genuine 0xff data byte. */
  while ((word = *ARDUINO_UART_DATA32) == 0xffffffffu)
  {
  }
  return (char)word;
}

void arduino_uart_putchar(char ch)
{
  *ARDUINO_UART_DATA = ch;
}

void arduino_uart_puts(char *s)
{
  while (*s != 0)
    *ARDUINO_UART_DATA = *s++;
}

void arduino_uart_print_hex_byte(uint8_t v)
{
  char ch;
  ch = (v >> 4) & 0xf;
  arduino_uart_putchar("0123456789abcdef"[(uint8_t)ch]);
  ch = v & 0xf;
  arduino_uart_putchar("0123456789abcdef"[(uint8_t)ch]);
}

uint32_t arduino_uart_gets(char *buf, uint32_t buf_len)
{
  uint32_t i;
  char ch;

  for (i = 0; i < buf_len; i++)
    buf[i] = 0;

  i = 0;
  while ((i < buf_len - 1) && ((ch = arduino_uart_getchar()) != '\r') && ch != '\n')
  {
    if ((ch == 3) || (ch == 0x7f) || (ch == 8))
    {
      arduino_uart_puts("\r\ncancelled\r\n");
      return 0;
    }
    *buf++ = ch;
    i += 1;
  }

  return i;
}

uint32_t arduino_uart_get_hex(void)
{
  uint32_t v;
  int keep_going;
  char ch;

  keep_going = 1;
  v = 0;
  while (keep_going)
  {
    ch = arduino_uart_getchar();
    if ((ch >= '0') && (ch <= '9'))
    {
      v = 16 * v + (ch - '0');
      arduino_uart_putchar(ch);
    }
    else if ((ch >= 'a') && (ch <= 'f'))
    {
      v = 16 * v + (ch - 'a' + 10);
      arduino_uart_putchar(ch);
    }
    else if ((ch >= 'A') && (ch <= 'F'))
    {
      v = 16 * v + (ch - 'A' + 10);
      arduino_uart_putchar(ch);
    }
    else if (ch == '\r' || ch == '\n')
    {
      arduino_uart_putchar('\n');
      keep_going = 0;
    }
  }

  return v;
}
