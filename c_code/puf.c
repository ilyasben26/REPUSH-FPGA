#include "std_int_types.h"
#include "uart.h"
#include "puf.h"

/*
 * Register addresses for the PUF peripheral (base 0x80000040).
 *
 * Write path:
 *   PUF_PAYLOAD_HI  (0x80000040) — bits [63:32] of the 64-bit command payload
 *   PUF_PAYLOAD_LO  (0x80000044) — bits [31:0]  of the 64-bit command payload
 *   PUF_TRIGGER     (0x80000048) — any write fires the pending command
 *
 * Read path (same word addresses as write, different behaviour):
 *   PUF_RESP_HI     (0x80000040) — bits [63:32] of the last PUF response
 *   PUF_RESP_LO     (0x80000044) — bits [31:0]  of the last PUF response
 *   PUF_STATUS      (0x80000048) — bit[0] = 1 when a REQ response is ready
 *
 * Command payload encoding (top nibble of payload_hi):
 *   0x1 — PUF_REQ        request measurement (produces an 8-byte response)
 *   0x2 — PUF_TUNE       payload_lo[7:5]=upper tuning, [4:2]=lower tuning
 *   0x3 — PUF_CHOICE_SET payload_lo[3:2]=top ASR index, [1:0]=bottom index
 *   0x4 — TOP_PATTERN    payload_hi[0]=in_bit, payload_lo=32-bit pattern
 *   0x5 — BOTTOM_PATTERN payload_hi[0]=in_bit, payload_lo=32-bit pattern
 */

#define PUF_PAYLOAD_HI  ((volatile uint32_t *) 0x80000040u)
#define PUF_PAYLOAD_LO  ((volatile uint32_t *) 0x80000044u)
#define PUF_TRIGGER     ((volatile uint32_t *) 0x80000048u)
#define PUF_STATUS      ((volatile uint32_t *) 0x80000048u)
#define PUF_RESP_HI     ((volatile uint32_t *) 0x80000040u)
#define PUF_RESP_LO     ((volatile uint32_t *) 0x80000044u)

static void puf_send_cmd(uint32_t phi, uint32_t plo)
{
    *PUF_PAYLOAD_HI = phi;
    *PUF_PAYLOAD_LO = plo;
    *PUF_TRIGGER    = 1u;
}

static void puf_do_req(uint32_t *hi, uint32_t *lo)
{
    puf_send_cmd(0x10000000u, 0x00000000u);
    while (!(*PUF_STATUS & 1u)) {}
    *hi = *PUF_RESP_HI;
    *lo = *PUF_RESP_LO;
}

void puf_request(void)
{
    uint32_t hi, lo;

    puf_do_req(&hi, &lo);
    uart_puts("PUF: ");
    uart_print_hex(hi);
    uart_putchar(':');
    uart_print_hex(lo);
    uart_puts("\r\n");
}

void puf_set_tune(uint32_t upper, uint32_t lower)
{
    uint32_t data = ((upper & 7u) << 5) | ((lower & 7u) << 2);

    puf_send_cmd(0x20000000u, data);
    uart_puts("PUF tune set\r\n");
}

void puf_set_choice(uint32_t top, uint32_t bottom)
{
    uint32_t choice_data;
    uint32_t top_pattern, top_phi;
    uint32_t bot_pattern, bot_phi;

    /* Send CHOICE_SET command */
    choice_data = ((top & 3u) << 2) | (bottom & 3u);
    puf_send_cmd(0x30000000u, choice_data);

    /*
     * Derive shift-register patterns from the choice indices.
     * This mirrors the pattern selection logic in the Python challenge command:
     *
     *   top_choice % 2 != 0  → top uses 0xAAAAAAAA, in_bit=0
     *   top_choice % 2 == 0  → top uses 0x55555555, in_bit=1
     *
     *   bottom_choice % 2 == 0 → bottom uses 0xAAAAAAAA, in_bit=0
     *   bottom_choice % 2 != 0 → bottom uses 0x55555555, in_bit=1
     */
    if (top % 2u != 0u) {
        top_pattern = 0xAAAAAAAAu;
        top_phi     = 0x40000000u;  /* in_bit = 0 */
    } else {
        top_pattern = 0x55555555u;
        top_phi     = 0x40000001u;  /* in_bit = 1 */
    }

    if (bottom % 2u == 0u) {
        bot_pattern = 0xAAAAAAAAu;
        bot_phi     = 0x50000000u;  /* in_bit = 0 */
    } else {
        bot_pattern = 0x55555555u;
        bot_phi     = 0x50000001u;  /* in_bit = 1 */
    }

    puf_send_cmd(top_phi, top_pattern);
    puf_send_cmd(bot_phi, bot_pattern);

    uart_puts("PUF choice+patterns set\r\n");
}

void puf_challenge(uint32_t top_choice, uint32_t top_tune,
                   uint32_t bottom_choice, uint32_t bottom_tune)
{
    uint32_t tune_data, choice_data;
    uint32_t top_pattern, top_phi, bot_pattern, bot_phi;
    uint32_t hi, lo;

    tune_data = ((top_tune & 7u) << 5) | ((bottom_tune & 7u) << 2);
    puf_send_cmd(0x20000000u, tune_data);

    choice_data = ((top_choice & 3u) << 2) | (bottom_choice & 3u);
    puf_send_cmd(0x30000000u, choice_data);

    if (top_choice % 2u != 0u) { top_pattern = 0xAAAAAAAAu; top_phi = 0x40000000u; }
    else                        { top_pattern = 0x55555555u; top_phi = 0x40000001u; }

    if (bottom_choice % 2u == 0u) { bot_pattern = 0xAAAAAAAAu; bot_phi = 0x50000000u; }
    else                          { bot_pattern = 0x55555555u; bot_phi = 0x50000001u; }

    puf_send_cmd(top_phi, top_pattern);
    puf_send_cmd(bot_phi, bot_pattern);

    puf_do_req(&hi, &lo);
    uart_puts("PUF: ");
    uart_print_hex(hi);
    uart_putchar(':');
    uart_print_hex(lo);
    uart_puts("\r\n");
}

/*
 * All valid (top_choice, bottom_choice) pairs where top_choice > bottom_choice.
 * Mirrors the choice_combinations list in the Python gather command.
 * 6 pairs × 8 top_tune × 8 bottom_tune = 384 total configurations.
 */
static const uint8_t scan_top[]    = {1, 2, 2, 3, 3, 3};
static const uint8_t scan_bottom[] = {0, 0, 1, 0, 1, 2};
#define SCAN_TOTAL 384u

void puf_scan(void)
{
    uint32_t valid = 0;
    uint32_t hi, lo;
    uint32_t tt, bt;
    int ci;

    for (ci = 0; ci < 6; ci++) {
        uint32_t tc = scan_top[ci];
        uint32_t bc = scan_bottom[ci];
        uint32_t top_phi, top_pattern, bot_phi, bot_pattern;

        /* patterns are determined by choice index parity — set once per pair */
        if (tc % 2u != 0u) { top_pattern = 0xAAAAAAAAu; top_phi = 0x40000000u; }
        else                { top_pattern = 0x55555555u; top_phi = 0x40000001u; }
        if (bc % 2u == 0u) { bot_pattern = 0xAAAAAAAAu; bot_phi = 0x50000000u; }
        else                { bot_pattern = 0x55555555u; bot_phi = 0x50000001u; }

        puf_send_cmd(0x30000000u, ((tc & 3u) << 2) | (bc & 3u));
        puf_send_cmd(top_phi, top_pattern);
        puf_send_cmd(bot_phi, bot_pattern);

        for (tt = 0; tt < 8u; tt++) {
            for (bt = 0; bt < 8u; bt++) {
                puf_send_cmd(0x20000000u, ((tt & 7u) << 5) | ((bt & 7u) << 2));
                puf_do_req(&hi, &lo);
                if (hi != 0xFFFFFFFFu || lo != 0xFFFFFFFFu)
                    valid++;
            }
        }
    }

    uart_puts("PUF scan: ");
    uart_print_hex(valid);
    uart_puts("/");
    uart_print_hex(SCAN_TOTAL);
    uart_puts(" challenges produce non-all-1s\r\n");
}

void puf_measure(uint32_t count)
{
    uint32_t bit_ones[64];
    uint32_t hi, lo;
    uint32_t majority_hi, majority_lo;
    uint32_t i, bit;

    if (count == 0u)
        count = 1u;

    for (bit = 0u; bit < 64u; bit++)
        bit_ones[bit] = 0u;

    for (i = 0u; i < count; i++) {
        puf_do_req(&hi, &lo);
        for (bit = 0u; bit < 32u; bit++) {
            if ((lo >> bit) & 1u) bit_ones[bit]++;
            if ((hi >> bit) & 1u) bit_ones[bit + 32u]++;
        }
    }

    majority_hi = majority_lo = 0u;
    for (bit = 0u; bit < 32u; bit++) {
        if (bit_ones[bit]        * 2u >= count) majority_lo |= (1u << bit);
        if (bit_ones[bit + 32u]  * 2u >= count) majority_hi |= (1u << bit);
    }

    uart_puts("PUF majority (");
    uart_print_hex(count);
    uart_puts(" samples): ");
    uart_print_hex(majority_hi);
    uart_putchar(':');
    uart_print_hex(majority_lo);
    uart_puts("\r\n");
}
