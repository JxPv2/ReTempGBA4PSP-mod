# Plan: Idle loop detection / removal (mGBA-style)

Goal: Speed up games that spend many cycles in **Thumb busy-wait loops** (e.g. polling `DISPSTAT` / VCount) by detecting a tight loop head and **fast-forwarding** to the next timing boundary, without changing behavior when the option is off.

Reference algorithm: mGBA `GBASetActiveRegion` + `_analyzeForIdleLoop` in [`mgba/src/gba/memory.c`](https://github.com/mgba-emu/mgba/blob/master/src/gba/memory.c) (study the logic; reimplement to avoid license mixing).

---

## 1. User-facing option (Emulator Options)

- **Label:** e.g. “Idle loop optimize” or “Skip idle loops” with **Off / On** (reuse `MSG_OFF` / `MSG_ON` like other toggles).
- **Location:** Same submenu as RAM dynarec, boot mode, etc. (`gui.c` emulator options table).
- **Variable:** `option_idle_loop_optimize` (or similar), `u32` **0 = off, 1 = on**.
- **Default:** **Off** (safest until validated on many games).

### Config file (`tempgba.cfg`)

- Bump `GPSP_CONFIG_NUM` in `gui.c` (currently `18 + 16`) by **1**.
- Add a **legacy size branch** in `load_config_file` / `save_config_file` so older configs map cleanly (new field defaults to off).
- Persist the new field in the same order as other global options in `file_options[]` (follow existing pattern for `option_video_renderer`).

### Strings / i18n

- Add `MSG_IDLE_LOOP_OPTIMIZE` (and help line if other emulator options have `MSG_*_HELP`).
- Update `message.c`, `message.h`, and GBK/ANSI message tables to match existing localization workflow.

---

## 2. Core state (C, likely `cpu.c` / `cpu.h`)

Mirror mGBA’s fields in spirit (names can differ):

| Concept | Purpose |
|--------|---------|
| `idle_loop_pc` | Thumb PC of recognized loop head (or `0xFFFFFFFF` = none) |
| `last_dispatch_pc` | Previous block-entry PC for “same address twice” test |
| `idle_detect_step` | 0 = snapshot regs; 1 = compare + analyze |
| `idle_detect_failures` | Count mismatches; disable after threshold (e.g. 10000) |
| `idle_halt_pending` | Two-phase arm before forcing exit (reduces false positives) |
| `cached_gprs[16]` | Snapshot for comparison |
| `idle_opt_disabled_session` | After too many failures, ignore until reset |

When **`option_idle_loop_optimize` is off:** do not update detection state (or reset state); behavior must match current emulator.

**Reset:** Clear idle state on `reset_gba` / ROM load so a new game does not inherit a stale `idle_loop_pc`.

---

## 3. Hook point (dynarec)

gpSP runs ARM/Thumb via translated blocks; the stable hook is **block dispatch**, not per-instruction interpreter.

- **Primary:** `block_lookup_address_thumb` / `block_lookup_address_arm` in `cpu.c` (and any shared path that runs whenever execution **enters** a block at a given PC).
- **Condition:** Same **memory region** as previous dispatch (reuse or mirror `pc_region` / top bits of PC) **and** `pc == last_dispatch_pc` → advance detection state machine.
- **Thumb-only v1:** If CPU is in ARM state at loop head, **abort** analysis (mGBA largely does this today).

### Static loop analysis (`idle_analyze_thumb_loop(u32 head_pc)`)

Reimplement the **idea** of `_analyzeForIdleLoop`:

- Decode Thumb opcodes from mapped memory (read via existing safe read path for IWRAM/EWRAM/ROM).
- Walk linearly until a **branch**:
  - If branch target **equals `head_pc`**, and no disallowed ops were seen (stores; non-constant I/O loads; untracked register updates), mark **`idle_loop_pc = head_pc`** and enter “remove” mode.
- **I/O loads:** Allow only addresses classified as **read-constant** for idle purposes (DISPSTAT-style polling may still change **between** iterations; removal must cooperate with timing—see §4).

**Optional later:** ARM-mode patterns or a small allowlist of known wait templates.

---

## 4. Removal (fast-forward)

When **`pc == idle_loop_pc`** and optimization is on:

- **Phase A (mGBA-style):** First hit: set `idle_halt_pending = 1`. Second hit: clear pending and **trigger fast path**.
- **Fast path (gpSP-specific):** Cause the MIPS JIT to **return to `update_gba`** similarly to `io_alert` in `mips_stub.S`: consume remaining slice cycles appropriately and let PPU/timers/IRQ advance.

**Cycle accounting (important):**

- Either approximate **one loop iteration’s** cycle cost and subtract from `EXECUTE_CYCLES` / local counter, or jump to the next **event** boundary conservatively.
- Start with a **conservative** approach; tune after hardware tests (audio desync and RNG timing are common regressions).

**Safety:** If **IRQ becomes pending** or **I/O state** invalidates the loop assumption, clear `idle_loop_pc` and `idle_detect_step` (hook from existing IRQ / SMC / major I/O paths if needed).

---

## 5. Testing checklist

- Option **Off:** byte-identical behavior to pre-feature baseline on a few titles.
- **On:** measurable FPS gain in titles known to VBlank-spin; no hangs on menus.
- Regression suite ideas: Pokémon (RNG), rhythm games, titles heavy on timers DMA.

---

## 6. Implementation order

1. Option + config + messages + menu wiring (no core change yet; default off).
2. State struct + reset hooks + `option` gating.
3. Hook `block_lookup_address_*` for “same PC twice” + GPR snapshot/compare.
4. Implement Thumb-only `idle_analyze_thumb_loop`.
5. Implement removal path (MIPS stub or C callback from stub) + cycle fudge.
6. Tune threshold and I/O allowlist; add session disable on failures.

---

## 7. Files expected to touch

| Area | Files |
|------|--------|
| Option / UI | `main.h`, `main.c`, `gui.c`, `message.h`, `message.c`, `text/*` |
| Config | `gui.c` (`GPSP_CONFIG_NUM`, load/save branches) |
| Detection / analysis | `cpu.c`, `cpu.h` (possibly small helper `.c`) |
| Removal | `mips_stub.S`, possibly `main.c` (`update_gba`) |

---

## 8. Risks

- **False idle loop:** subtle bugs; mitigate with threshold, two-phase pending, session disable.
- **Timing:** fast-forward too aggressive breaks games; start conservative and profile.
- **Thumb decode:** must match game mapping (open bus / unmapped) — use same memory read helpers as the CPU for fetches where possible.
