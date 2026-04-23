/* Copyright 2024 Grug Huhler.  License SPDX BSD-2-Clause.
 */

#ifndef _UART_H
#define UART_H

extern void uart_set_div(uint32_t div);
extern void uart_print_hex(uint32_t val);
extern char uart_getchar(void);
extern void uart_putchar(char ch);
extern void uart_puts(char *s);
extern void uart_print_hex_byte(uint8_t v);
extern uint32_t uart_gets(char *buf, uint32_t buf_len);
extern uint32_t uart_get_hex(void);

#endif
