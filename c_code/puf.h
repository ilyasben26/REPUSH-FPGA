#ifndef _PUF_H
#define _PUF_H

#include "std_int_types.h"

/*
 * PUF peripheral driver.
 *
 * The CHOICE PUF is memory-mapped at 0x80000040.
 * Commands mirror the PUF UART protocol in PUF-FPGA/python_scripts/puf_uart_client.py.
 *
 * Typical flow:
 *   puf_set_tune(7, 3);         // set ASR tuning
 *   puf_set_choice(3, 0);       // set choice and derive patterns automatically
 *   puf_request();              // single measurement
 *   puf_measure(10);            // 10-sample majority-voted measurement
 */

/* Single PUF measurement — sends REQ, waits for response, prints hi:lo hex. */
extern void puf_request(void);

/* Set ASR tuning.  upper and lower must be 0..7. */
extern void puf_set_tune(uint32_t upper, uint32_t lower);

/* Set ASR choice (top: 1..3, bottom: 0..2, top > bottom) and automatically
 * derive and load the top/bottom shift-register patterns using the same
 * rules as the Python challenge command. */
extern void puf_set_choice(uint32_t top, uint32_t bottom);

/* Collect count PUF measurements, perform per-bit majority voting, and
 * print the result.  Uses the current tune/choice/pattern settings. */
extern void puf_measure(uint32_t count);

/* Sweep all 384 valid challenges (6 choice pairs × 8 top_tune × 8 bottom_tune)
 * and print how many produce a response that is not all-1s. */
extern void puf_scan(void);

/* Scan all 384 challenges and write the valid ones (non-all-1s) to the SD card.
 * Blocks PUF_SD_BASE+0 (header) and +1..+3 (data) are overwritten.
 * Run sd_init() before calling this. */
extern void puf_save(void);

/* Prompt for a string on the UART, hash it with blake2b, map the hash uniformly
 * into the valid challenges stored on SD, query the PUF, and print the result.
 * Run puf_save() at least once before calling this. */
extern void puf_key(void);

/* One-shot challenge: configure tune + choice + patterns, then print a single
 * PUF measurement.  Values follow the same rules as the Python challenge
 * command (top_choice > bottom_choice, each 0..3; tunings 0..7). */
extern void puf_challenge(uint32_t top_choice, uint32_t top_tune,
                          uint32_t bottom_choice, uint32_t bottom_tune);

#endif
