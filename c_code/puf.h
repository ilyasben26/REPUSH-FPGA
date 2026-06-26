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

/* Load the valid challenges from the SD card into SRAM to bypass puf_scan(). */
extern void puf_load_challenges(void);

/* Prompt for a string on the UART, hash it with blake2b, map the hash uniformly
 * into the valid challenges stored on SD, query the PUF, and print the result.
 * Run puf_save() at least once before calling this. */
extern void puf_key(void);

/* One-shot challenge: configure tune + choice + patterns, then print a single
 * PUF measurement.  Values follow the same rules as the Python challenge
 * command (top_choice > bottom_choice, each 0..3; tunings 0..7). */
extern void puf_challenge(uint32_t top_choice, uint32_t top_tune,
                          uint32_t bottom_choice, uint32_t bottom_tune);

/* ---- LR-PUF State API ---- */

#define PUF_DOMAIN_STR_LEN 64

/* Save the 11 states into SD Card */
extern void puf_save_states(void);

/* Load the 11 states from SD Card */
extern void puf_load_states(void);

/* Set the domain name for the given state interactively */
extern void puf_set_domain(uint32_t state_index);

/* Set the domain name for the given state from a string (non-interactive). */
extern void puf_set_domain_str(uint32_t state_index, const char *domain);

/* Print the domain name for the given state */
extern void puf_print_domain(uint32_t state_index);

/* Reconfigure the state.  state_index: 0..10, seed: random value from external source. */
extern void puf_reconfigure_state(uint32_t state_index, uint32_t seed);

/* Map an external challenge to a valid PUF challenge via the current state, measure the
 * PUF with per-bit majority voting over `count` samples, hash the result, and return it
 * in out[32].  count=1 gives the single-shot behaviour.
 * Returns 0 on success, -1 on error. */
extern int puf_challenge_lr_ret(uint32_t state_index, uint32_t challenge_id, uint8_t out[32], uint32_t count);

/* Wrapper that calls puf_challenge_lr_ret and discards the return value (CLI use). */
extern void puf_challenge_lr(uint32_t state_index, uint32_t challenge_id);

/* Returns the index of the first uninitialized state via *state_index.
 * Returns 0 on success, -1 if all 11 states are in use. */
extern int puf_get_free_state(uint32_t *state_index);

/* Store the device Ed25519 public key, the raw 16-byte PUF challenge, and the
 * server Ed25519 public key for an enrollment record.
 * Sets acknowledged = 0 (pending server confirmation). */
extern void puf_store_enrollment(uint32_t state_index,
                                 const uint8_t pubkey[32],
                                 const uint8_t challenge_raw[16],
                                 const uint8_t pk_server[32]);

/* Find a state slot by domain name.  On success fills *state_idx, pubkey_out,
 * challenge_out, pk_server_out and returns 0.  Returns -1 if not found. */
extern int puf_find_state_by_domain(const char *domain,
                                    uint32_t *state_idx,
                                    uint8_t pubkey_out[32],
                                    uint8_t challenge_out[16],
                                    uint8_t pk_server_out[32]);

/* Set acknowledged = 1 for the given state slot and save states to SD. */
extern void puf_mark_acknowledged(uint32_t state_index);

/* Fill *is_init, *acknowledged, and domain_out (NUL-terminated, buf ≥ 65 bytes)
 * for the given slot.  Returns 0 on success, -1 if state_index is out of range. */
extern int puf_get_slot_status(uint32_t state_index,
                                uint8_t *is_init,
                                uint8_t *acknowledged,
                                char domain_out[65]);

/* Zero all 11 state slots in SRAM and persist to SD.
 * Does NOT touch the valid-challenge table (blocks 0-3) or the CA key (block 9). */
extern void puf_clear_states(void);

/* Print a one-line summary (domain + ack status) for every state slot. */
extern void puf_list_states(void);

/* ---- Evaluation utilities ---- */

/* Sweep every loaded valid challenge, take one raw measurement, and print to
 * the debug UART.  Format:
 *   PV_START <count_hex8>
 *   <tc_hex2> <tt_hex2> <bc_hex2> <bt_hex2> <HI_hex8>:<LO_hex8>
 *   PV_END
 * Requires 'lc' to have been called first (valid challenges must be in SRAM). */
extern void puf_dump_valid_responses(void);

/* Dump count individual (non-voted) raw PUF samples for the current challenge
 * setting (last configured by 'ph' or 'pv').  count is capped at 1000.
 * NOTE: the firmware argument parser uses hex, so "pm 64" = 100 samples.
 * Format:
 *   PM_START <count_hex8>
 *   <HI_hex8>:<LO_hex8>
 *   PM_END */
extern void puf_dump_raw_samples(uint32_t count);

/* ---- BCH Fuzzy Commitment API ---- */

/* Load BCH helper data (helper[8] + ecc[4] per state) from SD block 16.
 * Call once at startup after sd_card_init(). */
extern void puf_bch_load_helpers(void);

/* Enroll: takes majority-voted PUF measurement (vote_count samples), derives
 * a secret W, computes BCH ECC, stores helper+ecc to SD, writes 32-byte seed.
 * Returns 0 on success, -1 on error. */
extern int puf_bch_enroll(uint32_t state_index, uint32_t challenge_id,
                          uint32_t vote_count, uint8_t out_seed[32]);

/* Query: takes single-shot PUF measurement, XORs with helper, BCH-corrects
 * up to 4 bit flips, and derives the same 32-byte seed as enroll.
 * Returns 0 on success, -1 on error. */
extern int puf_bch_query(uint32_t state_index, uint32_t challenge_id,
                         uint8_t out_seed[32]);

#endif
