#ifndef SD_CARD_CGPT_H
#define SD_CARD_CGPT_H

/* Copyright 2024 Grug Huhler
 *
 * License: SPDX BSD-2-Clause.
 *
 * All functions return 0 on success and a negative value on failure.
 * buf is 512 bytes, the size of one block
 */


int sd_init(void);
int sd_read_block(uint32_t block_addr, uint8_t *buf);
int sd_write_block(uint32_t block_addr, const uint8_t *buf);

extern uint8_t sd_spi_use_helper;

#endif
