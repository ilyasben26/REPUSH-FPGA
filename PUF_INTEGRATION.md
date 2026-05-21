# PUF Integration into picorv32 SoC

## Overview

This document describes how the Physical Unclonable Function (PUF) is
integrated into the picorv32-based SoC running on the Tang Nano 20K FPGA.
There are two layers:

1. **CHOICE-PUF** — a 64-bit hardware PUF peripheral accessed via memory-mapped
   registers from the PicoRV32 CPU.
2. **LR-PUF (Lookup-and-Reconstruct PUF)** — a software layer built on top
   that expands the 64-bit hardware output to a 256-bit key using a sequence of
   challenges and Blake2b hashing, and manages persistent state on the SD card.

The FPGA acts as a cryptographic co-processor for the Arduino Nano 33 IoT.
The Arduino sends commands over a binary-framed UART protocol; the FPGA runs
the PUF measurement, computes the LR-PUF hash chain, and returns the result.

---

## Part 1: CHOICE-PUF Hardware

### New file: `src/puf_peripheral.v`

A Verilog wrapper bridges the picorv32 memory bus and the existing VHDL PUF
cores. It instantiates two VHDL entities from `PUF-FPGA/src/`:

- **`PUF_controller`** — decodes the 64-bit command payload and drives the PUF
  sequencing state machines (`request_ctrl_unit`, `pattern_ctrl_unit`,
  `tune_ctrl_unit`).
- **`CHOICE_PUF_gen`** — generates 64 `CHOICE_PUF` cells, each producing one
  bit of the PUF response via an ASR-based carry-chain race.

The wrapper exposes three 32-bit registers at its base address:

| Offset | Write | Read |
|--------|-------|------|
| `+0x0` | `payload_hi` — bits [63:32] of the 64-bit command | `response_hi` — bits [63:32] of the last PUF response |
| `+0x4` | `payload_lo` — bits [31:0] of the 64-bit command | `response_lo` — bits [31:0] of the last PUF response |
| `+0x8` | trigger — any write fires the pending command | status — bit 0 set when a REQ response is ready |

The 64-bit command payload format:

| Command nibble | Meaning | Data encoding |
|----------------|---------|---------------|
| `0x1` | `PUF_REQ` — request a measurement | — |
| `0x2` | `PUF_TUNE` — set ASR tuning | bits [7:5] = upper, [4:2] = lower |
| `0x3` | `PUF_CHOICE_SET` — set ASR choice | bits [3:2] = top index, [1:0] = bottom index |
| `0x4` | `TOP_PATTERN_SET` | `payload_hi[0]` = in-bit, `payload_lo` = 32-bit pattern |
| `0x5` | `BOTTOM_PATTERN_SET` | same encoding as `0x4` |

### Modified file: `src/top.v`

PUF peripheral added at base address `0x80000040`:

```
SRAM      0x00000000 – 0x0001ffff
LED       0x80000000
UART      0x80000008 – 0x8000000f
CDT       0x80000010 – 0x80000013
WS2812B   0x80000020 – 0x80000023  (20K only)
SD SPI    0x80000030 – 0x8000003f
PUF       0x80000040 – 0x8000004f
```

### VHDL source files (from `PUF-FPGA/src/`, not modified)

- `CHOICE_PUF.vhd` — single PUF cell
- `CHOICE_PUF_gen.vhd` — 64 instances
- `PUF_controller.vhd` — top-level controller
- `request_ctrl_unit.vhd` — REQ measurement FSM
- `pattern_ctrl_unit.vhd` — shift-register pattern loader
- `tune_ctrl_unit.vhd` — ASR length tuning

---

## Part 2: LR-PUF Software Layer

### Why 64 bits is not enough

Ed25519 requires a 256-bit seed. A single CHOICE-PUF measurement yields only
64 bits. The LR-PUF (Lookup-and-Reconstruct PUF) addresses this by:

1. Running multiple PUF challenges derived from a starting `challenge_id`.
2. Chaining the 64-bit outputs through `Blake2b-256` (already present in
   Monocypher) to accumulate entropy.
3. Producing a final 32-byte value that is device-unique and challenge-dependent.

The same `challenge_id` on the same physical device always produces the same
32-byte output. A different `challenge_id` produces a completely different
output. This makes it possible for the server to select the challenge (and
embed it in the enrollment payload), so the resulting device key is bound to
that specific enrollment session.

### puf_state_t — Per-Enrollment Slot

Each enrollment occupies one state slot. The firmware supports up to 11 slots
(indices 0–10). Each slot is a `puf_state_t` structure (168 bytes):

```c
typedef struct {
    uint8_t  hash_value[32];    // Blake2b running state (LR-PUF accumulator)
    uint32_t is_initialized;    // non-zero when slot is in use
    uint32_t tc_last;           // last challenge: top_choice
    uint32_t tt_last;           // last challenge: top_tune
    uint32_t bc_last;           // last challenge: bottom_choice
    uint32_t bt_last;           // last challenge: bottom_tune
    char     domain_name[64];   // enrolled domain (UTF-8, NUL-terminated)
    uint8_t  pubkey[32];        // device Ed25519 public key
    uint8_t  challenge_raw[16]; // the 16-byte PUF_challenge from the enrollment payload
    uint32_t acknowledged;      // 0 = pending server confirmation, 1 = confirmed
} puf_state_t;
```

Total structure size: 168 bytes. Padding: none (manual layout).

### SD Card Layout

```
Block  0      PUF challenge table header (magic + count)
Blocks 1–3    Valid CHOICE-PUF challenge records (4 bytes each, 128 per block)
Blocks 4–7    LR-PUF state slots (168 bytes × 11 = 1848 bytes, 4 × 512-byte blocks)
Block  8      CA public key record (magic + version + 32-byte key)
```

State slots occupy blocks 4–7 (4 × 512 = 2048 bytes ≥ 11 × 168 = 1848 bytes).
The CA key is at block 8.

The CA key record format (512-byte block, rest zeroed):
```
Bytes 0–3:   Magic = 0x314B4143 ("CAK1" LE)
Bytes 4–7:   Version = 1 (LE uint32)
Bytes 8–39:  CA Ed25519 public key (32 bytes)
```

---

## Part 3: FPGA Firmware Changes (`c_code/`)

### New file: `c_code/puf.c` / `c_code/puf.h`

The PUF driver provides three layers of functions:

#### CHOICE-PUF low-level (memory-mapped register access)

| Function | Description |
|----------|-------------|
| `puf_request()` | Single PUF measurement, prints 64-bit result |
| `puf_set_tune(upper, lower)` | Set ASR tuning (0–7 each) |
| `puf_set_choice(top, bottom)` | Set ASR choice + derive patterns |
| `puf_challenge(tc, tt, bc, bt)` | One-shot challenge |
| `puf_measure(count)` | Per-bit majority vote over `count` samples |
| `puf_scan()` | Scan all 384 valid challenge configurations |
| `puf_save()` | Save valid challenges to SD card (blocks 0–3) |
| `puf_key()` | Map a typed string → challenge via Blake2b + modular index |

#### LR-PUF state management

| Function | Description |
|----------|-------------|
| `puf_reconfigure_state(state_idx, seed)` | Initialize/reset a state slot with a random seed |
| `puf_challenge_lr(state_idx, challenge_id)` | Run LR-PUF, print result (interactive) |
| `puf_challenge_lr_ret(state_idx, challenge_id, out[32])` | Run LR-PUF, return 32-byte result |
| `puf_get_free_state(state_index*)` | Find the first uninitialized state slot |
| `puf_set_domain(state_idx)` | Interactively ask for domain via UART |
| `puf_set_domain_str(state_idx, domain)` | Set domain programmatically (used by Arduino protocol) |
| `puf_store_enrollment(state_idx, pubkey[32], challenge_raw[16])` | Store pubkey and challenge; sets acknowledged=0 |
| `puf_print_domain(state_idx)` | Print domain for a slot |
| `puf_save_states()` | Write all state slots to SD blocks 4–7 |
| `puf_load_states()` | Load all state slots from SD at boot |
| `puf_load_challenges()` | Load CHOICE-PUF challenge table from SD at boot |

#### LR-PUF algorithm (`puf_challenge_lr_ret`)

```
input: state_idx, challenge_id (uint32)
output: 32-byte key seed

1. Load valid challenge table from SRAM (loaded from SD at boot)
2. start_idx = challenge_id % num_valid_challenges
3. h = Blake2b-256("")     (empty initial state)
4. for i in 0..N_ROUNDS:
       c = valid_challenges[(start_idx + i) % num_valid_challenges]
       puf_response = puf_measure(1)   (64-bit majority-voted measurement)
       h = Blake2b-256(h || puf_response)
5. store h in puf_states[state_idx].hash_value
6. return h
```

The number of rounds `N_ROUNDS` is chosen so that the output has passed through
enough independent PUF measurements to provide 256 bits of effective entropy.

### Modified file: `c_code/main.c`

#### Binary protocol dispatcher

The main loop polls two sources: the debug UART (text commands) and the Arduino
UART (binary frames via `proto_poll_arduino_uart()`).

The Arduino-side binary protocol (see `Dongle-PUFMAN-Interface.md`) is handled
by `proto_dispatch_request()`. Enrollment-related commands:

| CMD | Handler |
|-----|---------|
| `4` (GET_CA_KEY) | Returns the 32-byte CA public key from the SD card |
| `5` (PUF_GET_FREE_STATE) | Calls `puf_get_free_state()`, returns 1-byte index |
| `6` (PUF_RECONFIGURE_STATE) | Calls `puf_reconfigure_state(idx, seed)` |
| `7` (PUF_CHALLENGE_LR) | Calls `puf_challenge_lr_ret(idx, challenge_id, out)`, returns 32 bytes |
| `8` (PUF_SET_DOMAIN) | Calls `puf_set_domain_str(idx, domain)` |
| `9` (PUF_STORE_ENROLLMENT) | Calls `puf_store_enrollment(idx, pubkey, challenge_raw)` |
| `10` (PUF_SAVE_STATES) | Calls `puf_save_states()` |

**Note on Ed25519 → X25519 conversion**: An earlier version included a CMD 11
that called `crypto_eddsa_to_x25519` from Monocypher. This caused a stack
overflow on the PicoRV32 (the protocol handler call chain exhausted the
available stack when Monocypher's inversion routine allocated its local arrays).
The command was removed; the conversion is now performed entirely on the Arduino
using TweetNaCl-style GF(2^255-19) field arithmetic.

#### Boot sequence additions

```c
puf_load_challenges();   // restore valid challenge table from SD blocks 0–3
puf_load_states();       // restore LR-PUF state slots from SD blocks 4–7
```

These run at startup so that any previously enrolled state is available
immediately when the Arduino sends its first RPC command.

#### Interactive UART commands (debug)

| Command | Description |
|---------|-------------|
| `rs state seed` | Reconfigure LR-PUF state |
| `cl state chal` | Perform LR-PUF challenge |
| `ss` | Save LR-PUF states to SD |
| `ls` | Load LR-PUF states from SD |
| `sd state` | Set domain (interactively) |
| `pd state` | Print domain |
| `ck` | Check CA public key in SD vs. compiled constant |
| `lc` | Load valid challenges from SD |
| `pk` | Map a string to a PUF challenge and print the response |

---

## Part 4: Cryptographic Libraries

### Monocypher 4.0.2 (`monocypher.c` / `monocypher.h`)

Used on the FPGA for:
- **Blake2b-256** — LR-PUF hash chaining and `puf_key` string-to-challenge mapping.
- **Ed25519** (sign/verify) — available but currently used only on the Arduino side.

Monocypher was chosen because it is a single-file C library with no
dependencies, making it suitable for the restricted build environment (no
standard library, limited SRAM, RISC-V cross-compiler).

### Why `crypto_eddsa_to_x25519` is NOT called on the FPGA

`crypto_eddsa_to_x25519` is present in `monocypher.o` and links correctly, but
calling it from inside the Arduino protocol handler chain
(`main_loop → proto_poll → proto_feed_byte → proto_dispatch_request → crypto_eddsa_to_x25519`)
caused the PicoRV32 to crash. The function allocates several `gf` (64-byte)
field element arrays on the stack, plus `crypto_f25519_inv` which is
equally stack-intensive. The combined depth exceeded the available stack.

The fix was to move the Ed25519 → X25519 public key conversion to the Arduino
(SAMD21, 32 KB SRAM), where it is implemented using TweetNaCl-style field
arithmetic (`int64_t gf25519[16]`, 16-limb representation).

---

## Part 5: Build and Flash

```bash
cd c_code
make clean && make 20k       # builds prog.bin and associated Verilog init files
# Then open Gowin IDE, synthesize, and program the Tang Nano 20K
```

Required toolchain: `riscv64-unknown-elf-gcc`.
See `c_code/README` for full build environment notes.

Useful debug commands after flashing:
```
si          — initialise SD card
ck          — verify CA key in SD matches compiled constant
lc          — load valid PUF challenges from SD
ls          — load LR-PUF states from SD
```
