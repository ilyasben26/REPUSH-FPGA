#include "std_int_types.h"
#include "uart.h"
#include "sd_card_cgpt.h"
#include "monocypher.h"
#include "puf.h"
#include "readtime.h"

/* ---- LR-PUF State integration declarations ---- */

#define NUM_STATES 11
#define HASH_SIZE 32
#define DOMAIN_STR_LEN PUF_DOMAIN_STR_LEN
#define MAX_POSSIBLE_CHALLENGES 384
/* Serialised bytes per state: 32+4+(4*4)+64+32+16+32+4 = 200 */
#define STATE_RECORD_SIZE 200u

typedef struct
{
    uint8_t hash_value[HASH_SIZE];
    uint32_t is_initialized;
    uint32_t tc_last, tt_last, bc_last, bt_last;
    char domain_name[DOMAIN_STR_LEN];
    uint8_t pubkey[32];        /* device Ed25519 public key set during enroll */
    uint8_t challenge_raw[16]; /* raw 16-byte PUF challenge from enroll payload */
    uint8_t pk_server[32];     /* server Ed25519 public key (from cert) */
    uint32_t acknowledged;     /* 0 = pending server ack, 1 = acknowledged */
} puf_state_t;

static puf_state_t lr_states[NUM_STATES];

static uint16_t mem_valid_challenges[MAX_POSSIBLE_CHALLENGES];
static uint32_t num_mem_valid_challenges = 0;

static uint16_t pack_challenge(uint32_t tc, uint32_t tt, uint32_t bc, uint32_t bt)
{
    return (uint16_t)(((tc & 0x3) << 8) | ((tt & 0x7) << 5) | ((bc & 0x3) << 3) | (bt & 0x7));
}

static void unpack_challenge(uint16_t packed, uint32_t *tc, uint32_t *tt, uint32_t *bc, uint32_t *bt)
{
    *tc = (packed >> 8) & 0x3;
    *tt = (packed >> 5) & 0x7;
    *bc = (packed >> 3) & 0x3;
    *bt = packed & 0x7;
}

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

// MMIO adresses for the PUF peripheral
#define PUF_PAYLOAD_HI ((volatile uint32_t *)0x80000040u)
#define PUF_PAYLOAD_LO ((volatile uint32_t *)0x80000044u)
#define PUF_TRIGGER ((volatile uint32_t *)0x80000048u)
#define PUF_STATUS ((volatile uint32_t *)0x80000048u)
#define PUF_RESP_HI ((volatile uint32_t *)0x80000040u)
#define PUF_RESP_LO ((volatile uint32_t *)0x80000044u)

// PUF Commands
#define PUF_REQ 0x10000000u
#define PUF_TUNE 0x20000000u
#define PUF_CHOICE_SET 0x30000000u
#define PUF_TOP_PATTERN 0x40000000u
#define PUF_BOTTOM_PATTERN 0x50000000u

static void puf_send_cmd(uint32_t phi, uint32_t plo)
{
    *PUF_PAYLOAD_HI = phi;
    *PUF_PAYLOAD_LO = plo;
    *PUF_TRIGGER = 1u;
}

static void puf_do_req(uint32_t *hi, uint32_t *lo)
{
    puf_send_cmd(PUF_REQ, 0x00000000u);
    while (!(*PUF_STATUS & 1u))
    {
    }
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

    puf_send_cmd(PUF_TUNE, data);
    uart_puts("PUF tune set\r\n");
}

void puf_set_choice(uint32_t top, uint32_t bottom)
{
    uint32_t choice_data;
    uint32_t top_pattern, top_phi;
    uint32_t bot_pattern, bot_phi;

    /* Send CHOICE_SET command */
    choice_data = ((top & 3u) << 2) | (bottom & 3u);
    puf_send_cmd(PUF_CHOICE_SET, choice_data);

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
    if (top % 2u != 0u)
    {
        top_pattern = 0xAAAAAAAAu;
        top_phi = PUF_TOP_PATTERN; /* in_bit = 0 */
    }
    else
    {
        top_pattern = 0x55555555u;
        top_phi = PUF_TOP_PATTERN | 1u; /* in_bit = 1 */
    }

    if (bottom % 2u == 0u)
    {
        bot_pattern = 0xAAAAAAAAu;
        bot_phi = PUF_BOTTOM_PATTERN; /* in_bit = 0 */
    }
    else
    {
        bot_pattern = 0x55555555u;
        bot_phi = PUF_BOTTOM_PATTERN | 1u; /* in_bit = 1 */
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
    puf_send_cmd(PUF_TUNE, tune_data);

    choice_data = ((top_choice & 3u) << 2) | (bottom_choice & 3u);
    puf_send_cmd(PUF_CHOICE_SET, choice_data);

    if (top_choice % 2u != 0u)
    {
        top_pattern = 0xAAAAAAAAu;
        top_phi = PUF_TOP_PATTERN;
    }
    else
    {
        top_pattern = 0x55555555u;
        top_phi = PUF_TOP_PATTERN | 1u;
    }

    if (bottom_choice % 2u == 0u)
    {
        bot_pattern = 0xAAAAAAAAu;
        bot_phi = PUF_BOTTOM_PATTERN;
    }
    else
    {
        bot_pattern = 0x55555555u;
        bot_phi = PUF_BOTTOM_PATTERN | 1u;
    }

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
static const uint8_t scan_top[] = {1, 2, 2, 3, 3, 3};
static const uint8_t scan_bottom[] = {0, 0, 1, 0, 1, 2};
#define SCAN_TOTAL 384u

void puf_scan(void)
{
    uint32_t valid = 0;
    uint32_t hi, lo;
    uint32_t tt, bt;
    int ci;

    num_mem_valid_challenges = 0;

    for (ci = 0; ci < 6; ci++)
    {
        uint32_t tc = scan_top[ci];
        uint32_t bc = scan_bottom[ci];
        uint32_t top_phi, top_pattern, bot_phi, bot_pattern;

        /* patterns are determined by choice index parity — set once per pair */
        if (tc % 2u != 0u)
        {
            top_pattern = 0xAAAAAAAAu;
            top_phi = PUF_TOP_PATTERN;
        }
        else
        {
            top_pattern = 0x55555555u;
            top_phi = PUF_TOP_PATTERN | 1u;
        }
        if (bc % 2u == 0u)
        {
            bot_pattern = 0xAAAAAAAAu;
            bot_phi = PUF_BOTTOM_PATTERN;
        }
        else
        {
            bot_pattern = 0x55555555u;
            bot_phi = PUF_BOTTOM_PATTERN | 1u;
        }

        puf_send_cmd(PUF_CHOICE_SET, ((tc & 3u) << 2) | (bc & 3u));
        puf_send_cmd(top_phi, top_pattern);
        puf_send_cmd(bot_phi, bot_pattern);

        for (tt = 0; tt < 8u; tt++)
        {
            for (bt = 0; bt < 8u; bt++)
            {
                puf_send_cmd(PUF_TUNE, ((tt & 7u) << 5) | ((bt & 7u) << 2));
                puf_do_req(&hi, &lo);
                if (hi != 0xFFFFFFFFu || lo != 0xFFFFFFFFu)
                {
                    valid++;
                    if (num_mem_valid_challenges < MAX_POSSIBLE_CHALLENGES)
                    {
                        mem_valid_challenges[num_mem_valid_challenges++] = pack_challenge(tc, tt, bc, bt);
                    }
                }
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

    for (i = 0u; i < count; i++)
    {
        puf_do_req(&hi, &lo);
        for (bit = 0u; bit < 32u; bit++)
        {
            if ((lo >> bit) & 1u)
                bit_ones[bit]++;
            if ((hi >> bit) & 1u)
                bit_ones[bit + 32u]++;
        }
    }

    majority_hi = majority_lo = 0u;
    for (bit = 0u; bit < 32u; bit++)
    {
        if (bit_ones[bit] * 2u >= count)
            majority_lo |= (1u << bit);
        if (bit_ones[bit + 32u] * 2u >= count)
            majority_hi |= (1u << bit);
    }

    uart_puts("PUF majority (");
    uart_print_hex(count);
    uart_puts(" samples): ");
    uart_print_hex(majority_hi);
    uart_putchar(':');
    uart_print_hex(majority_lo);
    uart_puts("\r\n");
}

/* ---- SD-backed challenge storage ---- */

/*
 * On-card layout (raw SD blocks):
 *
 *   Block PUF_SD_BASE + 0  — header
 *       [0:3]  magic = PUF_SD_MAGIC
 *       [4:7]  count of stored challenges (uint32_t LE)
 *       [8:]   zeroed
 *
 *   Blocks PUF_SD_BASE + 1, +2, +3  — packed challenge records
 *       128 records per block, 4 bytes each:
 *           byte 0: top_choice
 *           byte 1: top_tune
 *           byte 2: bottom_choice
 *           byte 3: bottom_tune
 *
 * The header is written last so an interrupted scan leaves no valid magic,
 * which puf_key detects and reports.
 */
#define PUF_SD_BASE 0u
#define PUF_SD_MAGIC 0x50554630u /* "PUF0" */

static uint8_t sd_buf[512];

static void write_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void puf_save(void)
{
    uint32_t count = 0;
    uint32_t hi, lo, tt, bt;
    uint32_t data_block = PUF_SD_BASE + 1u;
    int ci, i;

    uart_puts("PUF: scanning all challenges...\r\n");

    for (i = 0; i < 512; i++)
        sd_buf[i] = 0;

    for (ci = 0; ci < 6; ci++)
    {
        uint32_t tc = scan_top[ci];
        uint32_t bc = scan_bottom[ci];
        uint32_t top_phi, top_pattern, bot_phi, bot_pattern;

        if (tc % 2u != 0u)
        {
            top_pattern = 0xAAAAAAAAu;
            top_phi = PUF_TOP_PATTERN;
        }
        else
        {
            top_pattern = 0x55555555u;
            top_phi = PUF_TOP_PATTERN | 1u;
        }
        if (bc % 2u == 0u)
        {
            bot_pattern = 0xAAAAAAAAu;
            bot_phi = PUF_BOTTOM_PATTERN;
        }
        else
        {
            bot_pattern = 0x55555555u;
            bot_phi = PUF_BOTTOM_PATTERN | 1u;
        }

        puf_send_cmd(PUF_CHOICE_SET, ((tc & 3u) << 2) | (bc & 3u));
        puf_send_cmd(top_phi, top_pattern);
        puf_send_cmd(bot_phi, bot_pattern);

        for (tt = 0; tt < 8u; tt++)
        {
            for (bt = 0; bt < 8u; bt++)
            {
                puf_send_cmd(PUF_TUNE, ((tt & 7u) << 5) | ((bt & 7u) << 2));
                puf_do_req(&hi, &lo);
                if (hi != 0xFFFFFFFFu || lo != 0xFFFFFFFFu)
                {
                    uint32_t slot = count % 128u;
                    sd_buf[slot * 4u + 0u] = (uint8_t)tc;
                    sd_buf[slot * 4u + 1u] = (uint8_t)tt;
                    sd_buf[slot * 4u + 2u] = (uint8_t)bc;
                    sd_buf[slot * 4u + 3u] = (uint8_t)bt;
                    count++;
                    if (slot == 127u)
                    {
                        if (sd_write_block(data_block++, sd_buf) < 0)
                        {
                            uart_puts("SD write error\r\n");
                            return;
                        }
                        for (i = 0; i < 512; i++)
                            sd_buf[i] = 0;
                    }
                }
            }
        }
    }

    /* flush any remaining partial block */
    if (count % 128u != 0u)
    {
        if (sd_write_block(data_block, sd_buf) < 0)
        {
            uart_puts("SD write error\r\n");
            return;
        }
    }

    /* write header last — only a fully written table gets a valid magic */
    for (i = 0; i < 512; i++)
        sd_buf[i] = 0;
    write_u32_le(sd_buf + 0, PUF_SD_MAGIC);
    write_u32_le(sd_buf + 4, count);
    if (sd_write_block(PUF_SD_BASE, sd_buf) < 0)
    {
        uart_puts("SD write error\r\n");
        return;
    }

    uart_puts("PUF: saved ");
    uart_print_hex(count);
    uart_puts(" valid challenges to SD\r\n");
}

void puf_load_challenges(void)
{
    uint32_t count = 0;
    uint32_t idx, block, offset;
    uint32_t tc, tt, bc, bt;

    /* read header block */
    if (sd_read_block(PUF_SD_BASE, sd_buf) < 0)
    {
        uart_puts("SD read error\r\n");
        return;
    }
    if (read_u32_le(sd_buf) != PUF_SD_MAGIC)
    {
        uart_puts("no PUF table on SD — run ps first\r\n");
        return;
    }
    count = read_u32_le(sd_buf + 4);
    if (count == 0u)
    {
        uart_puts("0 valid challenges stored\r\n");
        return;
    }
    if (count > MAX_POSSIBLE_CHALLENGES)
    {
        count = MAX_POSSIBLE_CHALLENGES;
    }

    num_mem_valid_challenges = 0;
    block = PUF_SD_BASE + 1u;

    if (sd_read_block(block, sd_buf) < 0)
    {
        uart_puts("SD read error\r\n");
        return;
    }

    for (idx = 0; idx < count; idx++)
    {
        uint32_t expected_block = PUF_SD_BASE + 1u + (idx / 128u);
        if (expected_block != block)
        {
            block = expected_block;
            if (sd_read_block(block, sd_buf) < 0)
            {
                uart_puts("SD read error\r\n");
                return;
            }
        }

        offset = (idx % 128u) * 4u;
        tc = sd_buf[offset + 0u];
        tt = sd_buf[offset + 1u];
        bc = sd_buf[offset + 2u];
        bt = sd_buf[offset + 3u];

        mem_valid_challenges[num_mem_valid_challenges++] = pack_challenge(tc, tt, bc, bt);
    }

    uart_puts("PUF: loaded ");
    uart_print_hex(num_mem_valid_challenges);
    uart_puts(" valid challenges from SD to memory\r\n");
}

void puf_key(void)
{
    char input[64];
    uint8_t hash[32];
    uint32_t len, count, idx, block, offset;
    uint32_t tc, tt, bc, bt;
    uint32_t top_phi, top_pattern, bot_phi, bot_pattern;
    uint32_t hi, lo;

    uart_puts("challenge> ");
    len = uart_gets(input, sizeof(input));
    if (len == 0)
    {
        uart_puts("cancelled\r\n");
        return;
    }

    /* read header block */
    if (sd_read_block(PUF_SD_BASE, sd_buf) < 0)
    {
        uart_puts("SD read error\r\n");
        return;
    }
    if (read_u32_le(sd_buf) != PUF_SD_MAGIC)
    {
        uart_puts("no PUF table on SD — run ps first\r\n");
        return;
    }
    count = read_u32_le(sd_buf + 4);
    if (count == 0u)
    {
        uart_puts("0 valid challenges stored\r\n");
        return;
    }

    /* hash the input string and map uniformly into [0, count) */
    crypto_blake2b(hash, sizeof(hash), (const uint8_t *)input, (size_t)len);
    idx = read_u32_le(hash) % count;

    /* fetch the challenge record from the appropriate data block */
    block = PUF_SD_BASE + 1u + idx / 128u;
    offset = (idx % 128u) * 4u;
    if (sd_read_block(block, sd_buf) < 0)
    {
        uart_puts("SD read error\r\n");
        return;
    }
    tc = sd_buf[offset + 0u];
    tt = sd_buf[offset + 1u];
    bc = sd_buf[offset + 2u];
    bt = sd_buf[offset + 3u];

    /* configure PUF with the selected challenge */
    puf_send_cmd(PUF_TUNE, ((tt & 7u) << 5) | ((bt & 7u) << 2));
    puf_send_cmd(PUF_CHOICE_SET, ((tc & 3u) << 2) | (bc & 3u));
    if (tc % 2u != 0u)
    {
        top_pattern = 0xAAAAAAAAu;
        top_phi = PUF_TOP_PATTERN;
    }
    else
    {
        top_pattern = 0x55555555u;
        top_phi = PUF_TOP_PATTERN | 1u;
    }
    if (bc % 2u == 0u)
    {
        bot_pattern = 0xAAAAAAAAu;
        bot_phi = PUF_BOTTOM_PATTERN;
    }
    else
    {
        bot_pattern = 0x55555555u;
        bot_phi = PUF_BOTTOM_PATTERN | 1u;
    }
    puf_send_cmd(top_phi, top_pattern);
    puf_send_cmd(bot_phi, bot_pattern);

    puf_do_req(&hi, &lo);

    uart_puts("idx=");
    uart_print_hex(idx);
    uart_puts(" tc=");
    uart_print_hex(tc);
    uart_puts(" tt=");
    uart_print_hex(tt);
    uart_puts(" bc=");
    uart_print_hex(bc);
    uart_puts(" bt=");
    uart_print_hex(bt);
    uart_puts("\r\nPUF: ");
    uart_print_hex(hi);
    uart_putchar(':');
    uart_print_hex(lo);
    uart_puts("\r\n");
}

void puf_reconfigure_state(uint32_t state_index, uint32_t seed)
{
    if (state_index >= NUM_STATES)
        return;
    puf_state_t *st = &lr_states[state_index];

    time_ll_t now = readtime_ll();

    if (!st->is_initialized)
    {
        if (num_mem_valid_challenges == 0)
        {
            uart_puts("LR-PUF Error: Run pa (puf_scan) first!\r\n");
            return;
        }

        uint32_t idx = seed % num_mem_valid_challenges;
        uint32_t tc, tt, bc, bt;
        unpack_challenge(mem_valid_challenges[idx], &tc, &tt, &bc, &bt);

        uint32_t top_pattern, top_phi, bot_pattern, bot_phi;
        puf_send_cmd(PUF_TUNE, ((tt & 7u) << 5) | ((bt & 7u) << 2));
        puf_send_cmd(PUF_CHOICE_SET, ((tc & 3u) << 2) | (bc & 3u));
        if (tc % 2u != 0u)
        {
            top_pattern = 0xAAAAAAAAu;
            top_phi = PUF_TOP_PATTERN;
        }
        else
        {
            top_pattern = 0x55555555u;
            top_phi = PUF_TOP_PATTERN | 1u;
        }
        if (bc % 2u == 0u)
        {
            bot_pattern = 0xAAAAAAAAu;
            bot_phi = PUF_BOTTOM_PATTERN;
        }
        else
        {
            bot_pattern = 0x55555555u;
            bot_phi = PUF_BOTTOM_PATTERN | 1u;
        }
        puf_send_cmd(top_phi, top_pattern);
        puf_send_cmd(bot_phi, bot_pattern);

        uint32_t hi, lo;
        puf_do_req(&hi, &lo);

        uint8_t puf_resp[8];
        puf_resp[0] = hi >> 24;
        puf_resp[1] = hi >> 16;
        puf_resp[2] = hi >> 8;
        puf_resp[3] = hi;
        puf_resp[4] = lo >> 24;
        puf_resp[5] = lo >> 16;
        puf_resp[6] = lo >> 8;
        puf_resp[7] = lo;

        crypto_blake2b_ctx ctx;
        crypto_blake2b_init(&ctx, HASH_SIZE);
        crypto_blake2b_update(&ctx, (const uint8_t *)&tc, 4);
        crypto_blake2b_update(&ctx, (const uint8_t *)&tt, 4);
        crypto_blake2b_update(&ctx, (const uint8_t *)&bc, 4);
        crypto_blake2b_update(&ctx, (const uint8_t *)&bt, 4);
        crypto_blake2b_update(&ctx, puf_resp, 8);
        crypto_blake2b_update(&ctx, (const uint8_t *)&seed, 4);
        crypto_blake2b_update(&ctx, (const uint8_t *)&now.s.time_low, 4);
        crypto_blake2b_final(&ctx, st->hash_value);

        st->tc_last = tc;
        st->tt_last = tt;
        st->bc_last = bc;
        st->bt_last = bt;
        st->is_initialized = 1;

        uart_puts("LR-PUF: State ");
        uart_print_hex(state_index);
        uart_puts(" initialized.\r\n");
    }
    else
    {
        crypto_blake2b_ctx ctx;
        crypto_blake2b_init(&ctx, HASH_SIZE);
        crypto_blake2b_update(&ctx, st->hash_value, HASH_SIZE);
        crypto_blake2b_update(&ctx, (const uint8_t *)&seed, 4);
        crypto_blake2b_update(&ctx, (const uint8_t *)&now.s.time_low, 4);
        crypto_blake2b_final(&ctx, st->hash_value);

        uart_puts("LR-PUF: State ");
        uart_print_hex(state_index);
        uart_puts(" reconfigured.\r\n");
    }
}

int puf_challenge_lr_ret(uint32_t state_index, uint32_t challenge_id, uint8_t out[32], uint32_t count)
{
    if (state_index >= NUM_STATES)
    {
        uart_puts("Invalid state index\r\n");
        return -1;
    }
    puf_state_t *st = &lr_states[state_index];

    if (!st->is_initialized || num_mem_valid_challenges == 0)
    {
        uart_puts("LR-PUF Error: Not initialized\r\n");
        return -1;
    }

    uint8_t new_hash[HASH_SIZE];
    crypto_blake2b_ctx ctx;

    crypto_blake2b_init(&ctx, HASH_SIZE);
    crypto_blake2b_update(&ctx, st->hash_value, HASH_SIZE);
    crypto_blake2b_update(&ctx, (const uint8_t *)&challenge_id, 4);
    crypto_blake2b_final(&ctx, new_hash);

    uint32_t combined = ((uint32_t)new_hash[0] << 24) |
                        ((uint32_t)new_hash[1] << 16) |
                        ((uint32_t)new_hash[2] << 8) |
                        (uint32_t)new_hash[3];

    uint32_t idx = combined % num_mem_valid_challenges;
    uint32_t tc, tt, bc, bt;
    unpack_challenge(mem_valid_challenges[idx], &tc, &tt, &bc, &bt);

    uart_puts("LR-PUF: mapped input ");
    uart_print_hex(challenge_id);
    uart_puts(" -> tc=");
    uart_print_hex(tc);
    uart_puts(" tt=");
    uart_print_hex(tt);
    uart_puts(" bc=");
    uart_print_hex(bc);
    uart_puts(" bt=");
    uart_print_hex(bt);
    uart_puts("\r\n");

    uint32_t top_pattern, top_phi, bot_pattern, bot_phi;
    puf_send_cmd(PUF_TUNE, ((tt & 7u) << 5) | ((bt & 7u) << 2));
    puf_send_cmd(PUF_CHOICE_SET, ((tc & 3u) << 2) | (bc & 3u));
    if (tc % 2u != 0u)
    {
        top_pattern = 0xAAAAAAAAu;
        top_phi = PUF_TOP_PATTERN;
    }
    else
    {
        top_pattern = 0x55555555u;
        top_phi = PUF_TOP_PATTERN | 1u;
    }
    if (bc % 2u == 0u)
    {
        bot_pattern = 0xAAAAAAAAu;
        bot_phi = PUF_BOTTOM_PATTERN;
    }
    else
    {
        bot_pattern = 0x55555555u;
        bot_phi = PUF_BOTTOM_PATTERN | 1u;
    }
    puf_send_cmd(top_phi, top_pattern);
    puf_send_cmd(bot_phi, bot_pattern);

    /* Per-bit majority vote over `count` raw PUF samples before hashing.
     * A single flipped bit in the raw response would avalanche through Blake2b,
     * so voting must happen here on the 64 raw bits, not on the hash output. */
    uint32_t bit_ones[64];
    uint32_t hi, lo;
    uint32_t mv_bit;

    if (count < 1u)
        count = 1u;
    for (mv_bit = 0u; mv_bit < 64u; mv_bit++)
        bit_ones[mv_bit] = 0u;
    for (mv_bit = 0u; mv_bit < count; mv_bit++)
    {
        uint32_t s_hi, s_lo;
        puf_do_req(&s_hi, &s_lo);
        for (uint32_t b = 0u; b < 32u; b++)
        {
            if ((s_lo >> b) & 1u)
                bit_ones[b]++;
            if ((s_hi >> b) & 1u)
                bit_ones[b + 32u]++;
        }
    }
    hi = 0u;
    lo = 0u;
    for (mv_bit = 0u; mv_bit < 32u; mv_bit++)
    {
        if (bit_ones[mv_bit] * 2u >= count)
            lo |= (1u << mv_bit);
        if (bit_ones[mv_bit + 32u] * 2u >= count)
            hi |= (1u << mv_bit);
    }

    uint8_t puf_resp[8];
    puf_resp[0] = (uint8_t)(hi >> 24);
    puf_resp[1] = (uint8_t)(hi >> 16);
    puf_resp[2] = (uint8_t)(hi >> 8);
    puf_resp[3] = (uint8_t)hi;
    puf_resp[4] = (uint8_t)(lo >> 24);
    puf_resp[5] = (uint8_t)(lo >> 16);
    puf_resp[6] = (uint8_t)(lo >> 8);
    puf_resp[7] = (uint8_t)lo;

    crypto_blake2b_init(&ctx, HASH_SIZE);
    crypto_blake2b_update(&ctx, st->hash_value, HASH_SIZE);
    crypto_blake2b_update(&ctx, (const uint8_t *)&challenge_id, 4);
    crypto_blake2b_update(&ctx, puf_resp, 8);
    crypto_blake2b_final(&ctx, out);

    uart_puts("LR-PUF output hash: ");
    for (int i = 0; i < HASH_SIZE; i++)
        uart_print_hex_byte(out[i]);
    uart_puts("\r\n");

    return 0;
}

void puf_challenge_lr(uint32_t state_index, uint32_t challenge_id)
{
    uint8_t out[HASH_SIZE];
    puf_challenge_lr_ret(state_index, challenge_id, out, 1u);
}

int puf_get_free_state(uint32_t *state_index)
{
    uint32_t i;
    for (i = 0; i < NUM_STATES; i++)
    {
        if (!lr_states[i].is_initialized)
        {
            *state_index = i;
            return 0;
        }
    }
    return -1;
}

void puf_set_domain_str(uint32_t state_index, const char *domain)
{
    int i;
    if (state_index >= NUM_STATES)
        return;
    puf_state_t *st = &lr_states[state_index];
    for (i = 0; domain[i] && i < DOMAIN_STR_LEN - 1; i++)
        st->domain_name[i] = domain[i];
    st->domain_name[i] = '\0';
}

void puf_store_enrollment(uint32_t state_index,
                          const uint8_t pubkey[32],
                          const uint8_t challenge_raw[16],
                          const uint8_t pk_server[32])
{
    int i;
    if (state_index >= NUM_STATES)
        return;
    puf_state_t *st = &lr_states[state_index];
    for (i = 0; i < 32; i++)
        st->pubkey[i] = pubkey[i];
    for (i = 0; i < 16; i++)
        st->challenge_raw[i] = challenge_raw[i];
    for (i = 0; i < 32; i++)
        st->pk_server[i] = pk_server[i];
    st->acknowledged = 0;
}

void puf_save_states(void)
{
    uint32_t state_idx;
    uint32_t offset = 0;
    uint32_t block = PUF_SD_BASE + 4u;
    int i;

    for (i = 0; i < 512; i++)
        sd_buf[i] = 0;

    for (state_idx = 0; state_idx < NUM_STATES; state_idx++)
    {
        puf_state_t *st = &lr_states[state_idx];
        if (offset + STATE_RECORD_SIZE > 512u)
        {
            if (sd_write_block(block++, sd_buf) < 0)
            {
                uart_puts("SD write error\r\n");
                return;
            }
            for (i = 0; i < 512; i++)
                sd_buf[i] = 0;
            offset = 0;
        }
        for (i = 0; i < HASH_SIZE; i++)
            sd_buf[offset++] = st->hash_value[i];
        write_u32_le(sd_buf + offset, st->is_initialized);
        offset += 4u;
        write_u32_le(sd_buf + offset, st->tc_last);
        offset += 4u;
        write_u32_le(sd_buf + offset, st->tt_last);
        offset += 4u;
        write_u32_le(sd_buf + offset, st->bc_last);
        offset += 4u;
        write_u32_le(sd_buf + offset, st->bt_last);
        offset += 4u;
        for (i = 0; i < DOMAIN_STR_LEN; i++)
            sd_buf[offset++] = (uint8_t)st->domain_name[i];
        for (i = 0; i < 32; i++)
            sd_buf[offset++] = st->pubkey[i];
        for (i = 0; i < 16; i++)
            sd_buf[offset++] = st->challenge_raw[i];
        for (i = 0; i < 32; i++)
            sd_buf[offset++] = st->pk_server[i];
        write_u32_le(sd_buf + offset, st->acknowledged);
        offset += 4u;
    }
    if (sd_write_block(block, sd_buf) < 0)
    {
        uart_puts("SD write error\r\n");
        return;
    }
    uart_puts("LR-PUF: States saved to SD card.\r\n");
}

void puf_load_states(void)
{
    uint32_t state_idx;
    uint32_t offset = 0;
    uint32_t block = PUF_SD_BASE + 4u;
    int i;

    if (sd_read_block(block++, sd_buf) < 0)
    {
        uart_puts("SD read error\r\n");
        return;
    }

    for (state_idx = 0; state_idx < NUM_STATES; state_idx++)
    {
        puf_state_t *st = &lr_states[state_idx];
        if (offset + STATE_RECORD_SIZE > 512u)
        {
            if (sd_read_block(block++, sd_buf) < 0)
            {
                uart_puts("SD read error\r\n");
                return;
            }
            offset = 0;
        }
        for (i = 0; i < HASH_SIZE; i++)
            st->hash_value[i] = sd_buf[offset++];
        st->is_initialized = read_u32_le(sd_buf + offset);
        offset += 4u;
        st->tc_last = read_u32_le(sd_buf + offset);
        offset += 4u;
        st->tt_last = read_u32_le(sd_buf + offset);
        offset += 4u;
        st->bc_last = read_u32_le(sd_buf + offset);
        offset += 4u;
        st->bt_last = read_u32_le(sd_buf + offset);
        offset += 4u;
        for (i = 0; i < DOMAIN_STR_LEN; i++)
            st->domain_name[i] = (char)sd_buf[offset++];
        for (i = 0; i < 32; i++)
            st->pubkey[i] = sd_buf[offset++];
        for (i = 0; i < 16; i++)
            st->challenge_raw[i] = sd_buf[offset++];
        for (i = 0; i < 32; i++)
            st->pk_server[i] = sd_buf[offset++];
        st->acknowledged = read_u32_le(sd_buf + offset);
        offset += 4u;
    }
    uart_puts("LR-PUF: States loaded from SD card.\r\n");
}
void puf_set_domain(uint32_t state_index)
{
    if (state_index >= NUM_STATES)
    {
        uart_puts("Invalid state index\r\n");
        return;
    }
    puf_state_t *st = &lr_states[state_index];

    char input[DOMAIN_STR_LEN];
    uint32_t len;

    uart_puts("domain_name> ");
    len = uart_gets(input, sizeof(input));
    if (len == 0)
    {
        uart_puts("cancelled\r\n");
        return;
    }

    int i;
    for (i = 0; i < len && i < DOMAIN_STR_LEN - 1; i++)
    {
        st->domain_name[i] = input[i];
    }
    st->domain_name[i] = '\0';

    uart_puts("LR-PUF: State ");
    uart_print_hex(state_index);
    uart_puts(" domain set to '");
    uart_puts(st->domain_name);
    uart_puts("'\r\n");
}

void puf_print_domain(uint32_t state_index)
{
    if (state_index >= NUM_STATES)
    {
        uart_puts("Invalid state index\r\n");
        return;
    }
    puf_state_t *st = &lr_states[state_index];

    uart_puts("LR-PUF: State ");
    uart_print_hex(state_index);
    uart_puts(" domain is '");
    uart_puts(st->domain_name);
    uart_puts("'\r\n");
}

int puf_find_state_by_domain(const char *domain,
                             uint32_t *state_idx,
                             uint8_t pubkey_out[32],
                             uint8_t challenge_out[16],
                             uint8_t pk_server_out[32])
{
    uint32_t i, j;
    for (i = 0; i < NUM_STATES; i++)
    {
        if (!lr_states[i].is_initialized)
            continue;
        /* compare NUL-terminated strings without libc */
        int match = 1;
        for (j = 0;; j++)
        {
            if (lr_states[i].domain_name[j] != domain[j])
            {
                match = 0;
                break;
            }
            if (domain[j] == '\0')
                break;
        }
        if (!match)
            continue;
        *state_idx = i;
        for (j = 0; j < 32; j++)
            pubkey_out[j] = lr_states[i].pubkey[j];
        for (j = 0; j < 16; j++)
            challenge_out[j] = lr_states[i].challenge_raw[j];
        for (j = 0; j < 32; j++)
            pk_server_out[j] = lr_states[i].pk_server[j];
        return 0;
    }
    return -1;
}

void puf_mark_acknowledged(uint32_t state_index)
{
    if (state_index >= NUM_STATES)
    {
        uart_puts("Invalid state index\r\n");
        return;
    }
    lr_states[state_index].acknowledged = 1;
    puf_save_states();
    uart_puts("LR-PUF: State ");
    uart_print_hex(state_index);
    uart_puts(" acknowledged.\r\n");
}

int puf_get_slot_status(uint32_t state_index,
                        uint8_t *is_init,
                        uint8_t *acknowledged,
                        char domain_out[65])
{
    uint32_t i;
    if (state_index >= NUM_STATES)
        return -1;
    puf_state_t *st = &lr_states[state_index];
    *is_init = st->is_initialized ? 1 : 0;
    *acknowledged = (st->is_initialized && st->acknowledged) ? 1 : 0;
    for (i = 0; i < 64 && st->domain_name[i]; i++)
        domain_out[i] = st->domain_name[i];
    domain_out[i] = '\0';
    return 0;
}

void puf_clear_states(void)
{
    uint32_t i, j;
    for (i = 0; i < NUM_STATES; i++)
    {
        puf_state_t *st = &lr_states[i];
        for (j = 0; j < HASH_SIZE; j++)
            st->hash_value[j] = 0;
        st->is_initialized = 0;
        st->tc_last = st->tt_last = st->bc_last = st->bt_last = 0;
        for (j = 0; j < DOMAIN_STR_LEN; j++)
            st->domain_name[j] = '\0';
        for (j = 0; j < 32; j++)
            st->pubkey[j] = 0;
        for (j = 0; j < 16; j++)
            st->challenge_raw[j] = 0;
        for (j = 0; j < 32; j++)
            st->pk_server[j] = 0;
        st->acknowledged = 0;
    }
    puf_save_states();
    uart_puts("LR-PUF: All enrollment slots cleared.\r\n");
}

void puf_list_states(void)
{
    uint32_t i;
    uint32_t found = 0;
    uart_puts("LR-PUF enrollment slots:\r\n");
    for (i = 0; i < NUM_STATES; i++)
    {
        puf_state_t *st = &lr_states[i];
        uart_puts("  [");
        uart_print_hex(i);
        uart_puts("] ");
        if (!st->is_initialized)
        {
            uart_puts("(empty)\r\n");
        }
        else
        {
            found++;
            uart_puts(st->domain_name);
            uart_puts("  ");
            uart_puts(st->acknowledged ? "ACKNOWLEDGED" : "PENDING");
            uart_puts("\r\n");
        }
    }
    uart_puts("Total: ");
    uart_print_hex(found);
    uart_puts(" enrolled\r\n");
}
