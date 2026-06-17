/*
 * BCH(127,99,4) error correction for PUF fuzzy commitment.
 * Ported from Linux lib/bch.c (GPL-2.0, Parrot S.A.).
 * All dynamic allocation replaced with static globals.
 * Fixed parameters: m=7, t=4, prim_poly=0x83 (x^7+x+1).
 */

#include "bch_puf.h"

/* ---- Fixed parameters ---- */
#define PRIM_POLY   0x83u       /* x^7 + x + 1 */
#define N           127u        /* BCH_N */
#define M           7u          /* BCH_M */
#define T           4u          /* BCH_T */
#define ECC_BITS    28u         /* M * T */
#define ECC_BYTES   4u          /* ceil(28/8) */

/* Linux BCH allocates (T+1)*sizeof(gf_poly_deg1) = 5*12 = 60 bytes for elp,
 * allowing degree up to 13 during intermediate BM steps.  Match that. */
#define POLY_COEF_MAX 14u

typedef struct {
    uint32_t deg;
    uint32_t c[POLY_COEF_MAX];
} bch_poly_t;

/* ---- Static tables ---- */
static uint32_t bch_a_pow[N + 1];          /* alpha^i, i=0..127 */
static uint32_t bch_a_log[N + 1];          /* log_alpha(x), x=0..127 */
static uint32_t bch_xi[M];                 /* precomputed xi values for deg-2 solver */
static uint32_t bch_feedback_poly;          /* lower 28 bits of g(x) for LFSR encode */

/* Working buffers for decode (reused each call; not reentrant). */
static uint32_t bch_ecc_buf;               /* computed ECC as left-justified u32 */
static uint32_t bch_ecc_buf2;              /* stored ECC as left-justified u32 */
static uint32_t bch_syn[2 * T];            /* syndromes S1..S8 */
static int32_t  bch_cache[2 * T];          /* log-rep cache for gf_poly_logrep */
static bch_poly_t bch_elp;                 /* error locator polynomial */
static bch_poly_t bch_pelp;                /* BM helper: previous elp */
static bch_poly_t bch_elp_copy;            /* BM helper: elp copy */

/* ---- GF(2^7) arithmetic ---- */

static inline int bch_fls(uint32_t x)
{
    int r = 0;
    if (!x) return 0;
    if (x & 0xffff0000u) { r += 16; x >>= 16; }
    if (x & 0xff00u)     { r += 8;  x >>= 8;  }
    if (x & 0xf0u)       { r += 4;  x >>= 4;  }
    if (x & 0xcu)        { r += 2;  x >>= 2;  }
    if (x & 0x2u)        { r += 1; }
    return r + 1;
}

static inline int bch_deg(uint32_t x)
{
    return bch_fls(x) - 1;
}

static inline int bch_parity(uint32_t x)
{
    x ^= x >> 1; x ^= x >> 2;
    x = (x & 0x11111111u) * 0x11111111u;
    return (x >> 28) & 1;
}

static inline uint32_t bch_mod_s(uint32_t v)
{
    return (v < N) ? v : v - N;
}

static uint32_t bch_modulo(int v)
{
    while (v < 0)    v += N;
    while (v >= (int)N) {
        v -= N;
        v = (v & (int)N) + (v >> (int)M);
    }
    return (uint32_t)v;
}

static inline uint32_t bch_gf_mul(uint32_t a, uint32_t b)
{
    return (a && b) ? bch_a_pow[bch_mod_s(bch_a_log[a] + bch_a_log[b])] : 0u;
}

static inline uint32_t bch_gf_sqr(uint32_t a)
{
    return a ? bch_a_pow[bch_mod_s(2u * bch_a_log[a])] : 0u;
}

static inline uint32_t bch_gf_div(uint32_t a, uint32_t b)
{
    return a ? bch_a_pow[bch_mod_s(bch_a_log[a] + N - bch_a_log[b])] : 0u;
}

static inline uint32_t bch_gf_inv(uint32_t a)
{
    return bch_a_pow[N - bch_a_log[a]];
}

static inline uint32_t bch_a_pow_fn(int i)
{
    return bch_a_pow[bch_modulo(i)];
}

static inline int bch_a_log_fn(uint32_t x)
{
    return (int)bch_a_log[x];
}

static inline int bch_a_ilog(uint32_t x)
{
    return (int)bch_mod_s(N - bch_a_log[x]);
}

/* ---- Initialisation helpers ---- */

static void bch_build_gf_tables(void)
{
    uint32_t i, x = 1;
    const uint32_t k = 1u << (uint32_t)bch_deg(PRIM_POLY); /* = 128 */

    for (i = 0; i < N; i++) {
        bch_a_pow[i] = x;
        bch_a_log[x] = i;
        x <<= 1;
        if (x & k) x ^= PRIM_POLY;
    }
    bch_a_pow[N] = 1;
    bch_a_log[0] = 0;
}

/* Compute generator polynomial g(x) using the GF tables.
 * Returns the lower 28 bits (LFSR feedback polynomial). */
static uint32_t bch_compute_genpoly(void)
{
    /* g->c[i] = coefficient of x^i in g(x); max degree = M*T = 28 */
    static uint32_t gc[M * T + 2]; /* 30 elements */
    static uint8_t  roots[N + 1];  /* 128 bytes */
    uint32_t i, j, r, g_deg;
    uint32_t genpoly = 0;           /* stores the full 29-bit g(x) left-justified */

    for (i = 0; i <= N; i++) roots[i] = 0;
    for (i = 0; i < M * T + 2u; i++) gc[i] = 0;

    /* Mark roots: alpha^(2i+1) and all their GF(2^m) conjugates for i=0..T-1 */
    for (i = 0; i < T; i++) {
        for (j = 0, r = 2u * i + 1u; j < M; j++) {
            roots[r] = 1;
            r = bch_mod_s(2u * r);
        }
    }

    /* Build g(x) = product of (X + alpha^i) for each root i */
    gc[0] = 1;
    g_deg = 0;
    for (i = 0; i < N; i++) {
        if (roots[i]) {
            r = bch_a_pow[i];
            gc[g_deg + 1] = 1;
            for (j = g_deg; j > 0; j--)
                gc[j] = bch_gf_mul(gc[j], r) ^ gc[j - 1];
            gc[0] = bch_gf_mul(gc[0], r);
            g_deg++;
        }
    }
    /* g_deg should be 28; pack as 29-bit value left-justified in 32-bit word.
     * Bit 31 = x^28 coefficient (=1), bit 3 = x^0 coefficient. */
    for (j = 0; j < (g_deg + 1u); j++) {
        if (gc[g_deg - j])
            genpoly |= 1u << (31u - j);
    }

    /* Return lower 28 bits: coefficients of x^27..x^0 as a 28-bit value.
     * Shift genpoly right by 3 to get bit 28 (the x^0 coeff) into bit 0. */
    return (genpoly >> 3) & 0x0FFFFFFFu;
}

static void bch_build_deg2_base(void)
{
    uint32_t i, j, x, y, ak = 0;
    uint32_t sum, remaining, xi[M];

    /* Find k such that Tr(alpha^k) = 1 */
    for (i = 0; i < M; i++) {
        for (j = 0, sum = 0; j < M; j++)
            sum ^= bch_a_pow_fn((int)(i * (1u << j)));
        if (sum) { ak = bch_a_pow[i]; break; }
    }

    /* Find xi[i]: xi^2 + xi = alpha^i + Tr(alpha^i)*alpha^k */
    remaining = M;
    for (i = 0; i < M; i++) xi[i] = 0;
    for (x = 0; (x <= N) && remaining; x++) {
        y = bch_gf_sqr(x) ^ x;
        for (i = 0; i < 2u; i++) {
            int r = bch_a_log_fn(y);
            if (y && ((uint32_t)r < M) && !xi[(uint32_t)r]) {
                xi[(uint32_t)r] = x;
                remaining--;
                if (!remaining) break;
            }
            y ^= ak;
        }
    }
    for (i = 0; i < M; i++)
        bch_xi[i] = xi[i];
}

void bch_puf_init(void)
{
    bch_build_gf_tables();
    bch_feedback_poly = bch_compute_genpoly();
    bch_build_deg2_base();
}

/* ---- Encode ---- */

void bch_puf_encode(const uint8_t data[8], uint8_t ecc_out[4])
{
    uint32_t lfsr = 0;
    const uint32_t fp = bch_feedback_poly;
    int byte, bit;
    uint32_t ecc_word;

    for (byte = 0; byte < 8; byte++) {
        for (bit = 7; bit >= 0; bit--) {
            uint32_t in_bit = (uint32_t)((data[byte] >> bit) & 1);
            uint32_t fb = ((lfsr >> 27) ^ in_bit) & 1u;
            lfsr = (lfsr << 1) & 0x0FFFFFFFu;
            if (fb) lfsr ^= fp;
        }
    }
    /* Store as 4 bytes big-endian, left-justified: bit31=x^27, bit4=x^0 */
    ecc_word = lfsr << 4;
    ecc_out[0] = (uint8_t)(ecc_word >> 24);
    ecc_out[1] = (uint8_t)(ecc_word >> 16);
    ecc_out[2] = (uint8_t)(ecc_word >> 8);
    ecc_out[3] = (uint8_t)(ecc_word);
}

/* ---- Syndrome computation ---- */

static void bch_compute_syndromes(uint32_t ecc_word)
{
    uint32_t poly;
    int i, j, s;

    /* Clear low 4 bits (padding zeros for 28-bit ECC in 32-bit word) */
    ecc_word &= 0xFFFFFFF0u;

    for (i = 0; i < 2u * T; i++) bch_syn[i] = 0;

    s = (int)ECC_BITS;   /* 28 */
    poly = ecc_word;
    s -= 32;             /* s = -4 */

    while (poly) {
        i = bch_deg(poly);
        for (j = 0; j < (int)(2u * T); j += 2)
            bch_syn[j] ^= bch_a_pow_fn((j + 1) * (i + s));
        poly ^= (1u << i);
    }
    /* Odd syndromes via Frobenius: v(alpha^(2j)) = v(alpha^j)^2 */
    for (j = 0; j < (int)T; j++)
        bch_syn[2 * j + 1] = bch_gf_sqr(bch_syn[j]);
}

/* ---- Error locator polynomial (Berlekamp-Massey) ---- */

static int bch_compute_elp(void)
{
    uint32_t i, j, tmp, l, pd = 1, d;
    int k, pp = -1;
    bch_poly_t *elp = &bch_elp;
    bch_poly_t *pelp = &bch_pelp;
    bch_poly_t *elp_copy = &bch_elp_copy;

    pelp->deg = 0;
    for (j = 0; j < POLY_COEF_MAX; j++) pelp->c[j] = 0;
    pelp->c[0] = 1;
    elp->deg  = 0;
    for (j = 0; j < POLY_COEF_MAX; j++) elp->c[j] = 0;
    elp->c[0] = 1;
    d = bch_syn[0];

    for (i = 0; (i < T) && (elp->deg <= T); i++) {
        if (d) {
            k = (int)(2u * i) - pp;
            *elp_copy = *elp;
            tmp = (uint32_t)bch_a_log_fn(d) + N - (uint32_t)bch_a_log_fn(pd);
            for (j = 0; j <= pelp->deg; j++) {
                if (pelp->c[j])
                    elp->c[j + (uint32_t)k] ^= bch_a_pow_fn((int)(tmp + bch_a_log[pelp->c[j]]));
            }
            tmp = pelp->deg + (uint32_t)k;
            if (tmp > elp->deg) {
                elp->deg = tmp;
                *pelp = *elp_copy;
                pd = d;
                pp = (int)(2u * i);
            }
        }
        if (i < T - 1u) {
            d = bch_syn[2u * i + 2u];
            for (j = 1; j <= elp->deg; j++)
                d ^= bch_gf_mul(elp->c[j], bch_syn[2u * i + 2u - j]);
        }
    }
    return (elp->deg > T) ? -1 : (int)elp->deg;
}

/* ---- Root finding (BTZ for degree <= 4) ---- */

static int find_poly_deg1_roots(bch_poly_t *poly, uint32_t *roots)
{
    int n = 0;
    if (poly->c[0])
        roots[n++] = bch_mod_s(N - bch_a_log[poly->c[0]] + bch_a_log[poly->c[1]]);
    return n;
}

static int find_poly_deg2_roots(bch_poly_t *poly, uint32_t *roots)
{
    int n = 0;
    uint32_t u, v, r;
    int l0, l1, l2, i;

    if (poly->c[0] && poly->c[1]) {
        l0 = (int)bch_a_log[poly->c[0]];
        l1 = (int)bch_a_log[poly->c[1]];
        l2 = (int)bch_a_log[poly->c[2]];
        u = bch_a_pow_fn(l0 + l2 + 2 * ((int)N - l1));
        r = 0; v = u;
        while (v) {
            i = bch_deg(v);
            r ^= bch_xi[i];
            v ^= (1u << i);
        }
        if ((bch_gf_sqr(r) ^ r) == u) {
            roots[n++] = (uint32_t)bch_modulo(2 * (int)N - l1 - (int)bch_a_log[r] + l2);
            roots[n++] = (uint32_t)bch_modulo(2 * (int)N - l1 - (int)bch_a_log[r ^ 1u] + l2);
        }
    }
    return n;
}

/* Forward declaration */
static int solve_linear_system(uint32_t *rows, uint32_t *sol, int nsol);
static int find_affine4_roots(uint32_t a, uint32_t b, uint32_t c, uint32_t *roots);

static int find_poly_deg3_roots(bch_poly_t *poly, uint32_t *roots)
{
    int i, n = 0;
    uint32_t a, b, c, a2, b2, c2, e3, tmp[4];

    if (poly->c[0]) {
        e3 = poly->c[3];
        c2 = bch_gf_div(poly->c[0], e3);
        b2 = bch_gf_div(poly->c[1], e3);
        a2 = bch_gf_div(poly->c[2], e3);
        c  = bch_gf_mul(a2, c2);
        b  = bch_gf_mul(a2, b2) ^ c2;
        a  = bch_gf_sqr(a2) ^ b2;
        if (find_affine4_roots(a, b, c, tmp) == 4) {
            for (i = 0; i < 4; i++) {
                if (tmp[i] != a2)
                    roots[n++] = (uint32_t)bch_a_ilog(tmp[i]);
            }
        }
    }
    return n;
}

static int find_poly_deg4_roots(bch_poly_t *poly, uint32_t *roots)
{
    int i, l, n = 0;
    uint32_t a, b, c, d, e = 0, f, a2, b2, c2, e4;

    if (poly->c[0] == 0) return 0;

    e4 = poly->c[4];
    d  = bch_gf_div(poly->c[0], e4);
    c  = bch_gf_div(poly->c[1], e4);
    b  = bch_gf_div(poly->c[2], e4);
    a  = bch_gf_div(poly->c[3], e4);

    if (a) {
        if (c) {
            f = bch_gf_div(c, a);
            l = bch_a_log_fn(f);
            l += (l & 1) ? (int)N : 0;
            e = bch_a_pow_fn(l / 2);
            d = bch_a_pow_fn(2 * (l / 2)) ^ bch_gf_mul(b, f) ^ d;
            b = bch_gf_mul(a, e) ^ b;
        }
        if (d == 0) return 0;
        c2 = bch_gf_inv(d);
        b2 = bch_gf_div(a, d);
        a2 = bch_gf_div(b, d);
    } else {
        c2 = d; b2 = c; a2 = b;
    }

    if (find_affine4_roots(a2, b2, c2, roots) == 4) {
        for (i = 0; i < 4; i++) {
            f = a ? bch_gf_inv(roots[i]) : roots[i];
            roots[i] = (uint32_t)bch_a_ilog(f ^ e);
        }
        n = 4;
    }
    return n;
}

static int solve_linear_system(uint32_t *rows, uint32_t *sol, int nsol)
{
    const int m = (int)M;
    uint32_t tmp, mask;
    int rem, c, r, p, k;
    int param[M];  /* at most M=7 free parameters */

    k = 0;
    mask = 1u << m;

    for (c = 0; c < m; c++) {
        rem = 0; p = c - k;
        for (r = p; r < m; r++) {
            if (rows[r] & mask) {
                if (r != p) { uint32_t t = rows[r]; rows[r] = rows[p]; rows[p] = t; }
                rem = r + 1;
                break;
            }
        }
        if (rem) {
            tmp = rows[p];
            for (r = rem; r < m; r++)
                if (rows[r] & mask) rows[r] ^= tmp;
        } else {
            param[k++] = c;
        }
        mask >>= 1;
    }
    if (k > 0) {
        p = k;
        for (r = m - 1; r >= 0; r--) {
            if ((r > m - 1 - k) && rows[r]) return 0;
            rows[r] = (p && (r == param[p - 1])) ? (p--, 1u << (m - r)) : rows[r - p];
        }
    }
    if (nsol != (1 << k)) return 0;

    for (p = 0; p < nsol; p++) {
        for (c = 0; c < k; c++)
            rows[param[c]] = (rows[param[c]] & ~1u) | (uint32_t)((p >> c) & 1);
        tmp = 0;
        for (r = m - 1; r >= 0; r--) {
            mask = rows[r] & (tmp | 1u);
            tmp |= (uint32_t)bch_parity(mask) << (m - r);
        }
        sol[p] = tmp >> 1;
    }
    return nsol;
}

static int find_affine4_roots(uint32_t a, uint32_t b, uint32_t c, uint32_t *roots)
{
    int i, j, k;
    const int m = (int)M;
    uint32_t mask = 0xff, t;
    uint32_t rows[16];

    for (i = 0; i < 16; i++) rows[i] = 0;
    j = bch_a_log_fn(b);
    k = bch_a_log_fn(a);
    rows[0] = c;

    for (i = 0; i < m; i++) {
        rows[i + 1] = bch_a_pow[4 * i % N] ^
                      (a ? bch_a_pow[bch_mod_s((uint32_t)k)] : 0u) ^
                      (b ? bch_a_pow[bch_mod_s((uint32_t)j)] : 0u);
        j++;
        k += 2;
    }
    /* Transpose 16x16 binary matrix */
    for (j = 8; j != 0; j >>= 1, mask ^= (mask << j)) {
        for (k = 0; k < 16; k = (k + j + 1) & ~j) {
            t = ((rows[k] >> j) ^ rows[k + j]) & mask;
            rows[k]     ^= (t << j);
            rows[k + j] ^= t;
        }
    }
    return solve_linear_system(rows, roots, 4);
}

static int bch_find_roots(bch_poly_t *poly, uint32_t *roots)
{
    switch (poly->deg) {
    case 1: return find_poly_deg1_roots(poly, roots);
    case 2: return find_poly_deg2_roots(poly, roots);
    case 3: return find_poly_deg3_roots(poly, roots);
    case 4: return find_poly_deg4_roots(poly, roots);
    default: return 0;  /* BCH_T=4, elp->deg is always 1..4 */
    }
}

/* ---- Public decode ---- */

int bch_puf_decode(uint8_t data[8], const uint8_t ecc[4])
{
    uint32_t errloc[T];
    uint32_t sum;
    uint32_t nbits;
    int err, nroots, i;

    /* Compute ECC of (possibly noisy) data */
    bch_ecc_buf = 0;
    {
        uint32_t lfsr = 0;
        const uint32_t fp = bch_feedback_poly;
        int byte, bit;
        for (byte = 0; byte < 8; byte++) {
            for (bit = 7; bit >= 0; bit--) {
                uint32_t in_bit = (uint32_t)((data[byte] >> bit) & 1);
                uint32_t fb = ((lfsr >> 27) ^ in_bit) & 1u;
                lfsr = (lfsr << 1) & 0x0FFFFFFFu;
                if (fb) lfsr ^= fp;
            }
        }
        bch_ecc_buf = lfsr << 4;
    }

    /* Load stored ECC big-endian into u32 */
    bch_ecc_buf2 = ((uint32_t)ecc[0] << 24) | ((uint32_t)ecc[1] << 16) |
                   ((uint32_t)ecc[2] << 8)  |  (uint32_t)ecc[3];

    /* XOR to get syndrome word */
    sum = bch_ecc_buf ^ bch_ecc_buf2;
    if (!sum) return 0;  /* no errors */

    bch_compute_syndromes(sum);

    err = bch_compute_elp();
    if (err <= 0) return BCH_ERRS_UNCORRECTABLE;

    nroots = bch_find_roots(&bch_elp, errloc);
    if (nroots != err) return BCH_ERRS_UNCORRECTABLE;

    /* Convert error locations to bit positions and correct data */
    nbits = 8u * 8u + ECC_BITS;  /* 64 + 28 = 92 */
    for (i = 0; i < err; i++) {
        if (errloc[i] >= nbits) return BCH_ERRS_UNCORRECTABLE;
        errloc[i] = nbits - 1u - errloc[i];
        errloc[i] = (errloc[i] & ~7u) | (7u - (errloc[i] & 7u));
        if (errloc[i] < 64u)
            data[errloc[i] / 8u] ^= (uint8_t)(1u << (errloc[i] % 8u));
        /* errors in ECC portion (errloc[i] >= 64) are ignored */
    }
    return err;
}
