#ifndef BCH_PUF_H
#define BCH_PUF_H

#include "std_int_types.h"

/*
 * BCH(127,99,4) shortened to BCH(92,64,4) — covers 64-bit PUF response.
 * Primitive polynomial: x^7+x+1 = 0x83.  Corrects up to 4 bit flips.
 * ECC: 28 bits stored as 4 bytes (MSB of ecc[0] = coefficient of x^27).
 *
 * Fuzzy commitment (used by puf.c):
 *   Enroll: W derived from majority-voted PUF response y.
 *           helper = y XOR W  (8 bytes, stored to SD)
 *           ecc    = bch_puf_encode(W)  (4 bytes, stored to SD)
 *           seed   = BLAKE2b(state_hash || challenge_id || W)
 *
 *   Query:  noisy_y = single-shot PUF response
 *           candidate = noisy_y XOR helper
 *           bch_puf_decode(candidate, ecc)  — corrects up to 4 flips in-place
 *           seed = BLAKE2b(state_hash || challenge_id || candidate)
 */

#define BCH_M         7u
#define BCH_T         4u
#define BCH_N         127u      /* (1<<BCH_M)-1 */
#define BCH_ECC_BITS  28u       /* BCH_M * BCH_T */
#define BCH_ECC_BYTES 4u        /* ceil(28/8) */

#define BCH_ERRS_UNCORRECTABLE (-1)

/* Build GF(2^7) tables and generator polynomial.  Call once at startup. */
void bch_puf_init(void);

/*
 * Compute 28-bit BCH ECC of data[8].  ecc_out must be zeroed before call.
 * Output is 4 bytes big-endian, MSB of ecc_out[0] = x^27 coefficient.
 */
void bch_puf_encode(const uint8_t data[8], uint8_t ecc_out[4]);

/*
 * Decode and correct up to BCH_T=4 bit errors in data[8] using stored ecc[4].
 * Flips corrected bits in data[] in-place.
 * Returns number of errors found (0..4), or BCH_ERRS_UNCORRECTABLE on failure.
 */
int bch_puf_decode(uint8_t data[8], const uint8_t ecc[4]);

#endif /* BCH_PUF_H */
