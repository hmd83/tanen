# ⏱️ NAU7802 Timing — Flush CR Timeout Issue

**Date:** 2026-08-08
**Status:** RESOLVED (PR #1 — `f9dfe4e`)
**Affected file:** `src/drivers/weight.c`
**Board:** Seeed Studio XIAO nRF54LM20A

---

## 🔍 Problem

The NAU7802 (24-bit sigma-delta ADC for the load cell) signals a completed
conversion with the **Conversion Ready (CR) bit** in the PU_CTRL register.

After **CS (chip-select) assert** or **offset calibration (OCAL)**, the
**first CR arrives ~300–350 ms late** — not at the nominal ~100 ms period.
Reason: the sigma-delta **sinc filter must refill** before it produces a valid
result.

### Impact of the old budget

The flush loop (5 samples to clear the startup transient) used a hard-coded
**50 ms + 200 ms** timeout. Because the first CR landed beyond that window:

- `poll_pu_bit(PU_CR, 200)` **timed out on every cold init**
- The loop `break`s on timeout → **the startup transient was never actually flushed**
- Warm path failed one iteration later: `flush[1]` instead of `flush[0]`

## 📐 Measured evidence (LM20A board, logic trace)

| Event | Time |
|---|---|
| CS asserted after OCAL1 log | t = 05.5426 |
| Old deadline (200 ms) passes, **no CR** | t = 05.7941 |
| First three samples complete | t = 06.1473 |
| **First CR after CS** | **~300–350 ms** |

## 🛠️ Fix

```c
/* One conversion per ~100 ms at CRS=10 SPS — the steady-state CR period. */
#define CONV_PERIOD_MS 100

/* 5 periods gives real margin over the measured 350 ms. Ceiling, not delay:
 * settled samples still return in ~100 ms. */
#define CONV_SETTLE_MS (5 * CONV_PERIOD_MS)
```

Both flush paths (cold + warm) now poll with `CONV_SETTLE_MS` (500 ms)
**instead of 200 ms**, applied to **every iteration** — not just the first.

### Why every iteration?

The warm path (after a PUA cycle) had CR **still latched** from before the
cycle: `flush[0]` consumed that stale sample instantly, so `flush[1]` was the
first iteration that actually waited on the settling filter. A first-iteration-only
budget would still have missed it.

## ⚖️ Trade-off

- **Cost:** ~0.5 s extra awake-time per wake (the flush now genuinely runs)
- **Reading quality:** unchanged — `weight_read`'s cluster filter already
  absorbed the transient
- **If that time matters:** cut the flush count, not the timeout

## 🔗 Related

- PR #1: `fix/nau7802-flush-settle`
- Also in PR #1:
  - `6405f95` — build: compile out BLE/LoRa independently (`CONFIG_TANENBASE_BLE_CONFIG`, `CONFIG_LORAWAN` with `-ENOTSUP` stubs)
  - `ffcbfe7` — flash: call `openocd` binary directly (Windows Application Control blocks the chocolatey `bin\` shim)

*Documented by Zena (2026-08-08)*
