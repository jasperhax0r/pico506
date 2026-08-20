# Changes from upstream pico506

This fork adapts [kuba2k2/pico506](https://github.com/kuba2k2/pico506) — originally an
RLL emulator for the Toshiba T1200 — to **MFM** operation on a **Kaypro 10** with a
**WD1002-HDO** (WD1010) controller, on a custom PCB.

**The entire PIO layer is unmodified.** All nine `.pio` / `pio_util.h` files are
byte-identical to upstream, as are `clicker.c`, `diskio.c`, `utils.c` and `utils.h`.
Everything below is confined to five files.

Changes are grouped into two kinds:

* **[UPSTREAM]** — bugs or limitations in upstream that would affect anyone, on any
  hardware or geometry. These are worth merging back.
* **[BOARD]** — adaptations to this specific PCB, controller and geometry. Not
  upstream material, but useful to anyone porting to another machine.

---

## config.h — pin map and geometry

### [BOARD] Complete pin remap

Every pin moved to match the PCB layout. Upstream vs here:

| Signal | Upstream | Here | Note |
|---|---|---|---|
| RDDATA | 5 | 5 | direct to SN75176 (U4) D input |
| WRDATA | 3 | 6 | from SN75176 (U5) R output |
| DRIVE SELECT | 11 | 13 | |
| WRITE GATE | 2 | 9 | |
| HEAD SEL 1 | 4 | 10 | |
| HEAD SEL 2 | — | 11 | **new** — see `st506.c` head decode |
| DIRECTION IN | 13 | 8 | |
| STEP | 12 | 7 | |
| SEEK COMPLETE | 10 | 1 | |
| INDEX | 6 | 0 | |
| TRACK 0 | 9 | 2 | |
| READY | 8 | 4 | |
| SERVO GATE | 7 | 1 | aliased onto SEEK_COMPLETE — see below |
| BUZZER | 15 | 18 | |
| SD SCK/MOSI/MISO/CS | 18/19/20/21 | 14/15/28/17 | **SPI1**, not SPI0 |

Control **inputs** arrive through a non-inverting SN74AHCT245 and are active-low at
the bus, so they read inverted at the Pico. Control **outputs** go through an
inverting ULN2803, so they are active-high at the Pico. This asymmetry drives most of
the `gpio_set_inover` work in `st506.c`.

The SD card is on **SPI1** because of the pin choice. Upstream's `sd_spi_init` picks
the instance from the pins, but anything that touches `spi0_hw` directly needs
changing to `spi1_hw`.

### [BOARD] SERVO_GATE aliased onto SEEK_COMPLETE

`PIN_SERVO_GATE` is defined as GP1 — the same pin as `PIN_SEEK_COMPLETE`.

SERVO_GATE is a JVC-drive signal with no meaning on an ST-506 bus, but the RDGT PIO
program requires it to be *declared* adjacent to `PIN_INDEX` because it drives the two
as a consecutive pin group. Since GP1 is genuinely SEEK_COMPLETE on this board, the
pin is declared for the PIO's benefit but **must never be handed to the PIO** — see the
deleted `pio_gpio_init` in `st506.c`. Without that deletion, the RDGT servo pattern
(17 pulses/revolution ≈ 1021 Hz) bleeds onto SEEK_COMPLETE and the controller sees the
drive constantly re-seeking.

### [BOARD] Geometry: RLL 7.5 Mb/s → MFM 5 Mb/s, 306 × 4 × 17 × 512

| | Upstream | Here |
|---|---|---|
| `DATA_RATE` | 7500000 | 5000000 |
| `MARK_LBYTES` | 50 | 42 |
| `HEADER_LBYTES` | 100 | 58 |
| `SECTORS_PER_PULSE` | 2 | 1 |
| `HEADS` | 2 | 4 |
| `CYLINDERS` | 615 | 306 |

The gap/header sizes were chosen so every derived macro stays integral and the track
comes out at exactly one revolution:

```
TRACK_BITS = 16 × 17 × (42 + 58 + 512) = 166464 bits
           → 16.646 ms/rev → 60.07 Hz INDEX
TRACK_BYTES 20808, TRACK_WORDS 5202, CYLINDER_BYTES 83232
DRIVE_BYTES 25,468,992
```

The Kaypro BIOS only addresses 5 MB, so the 10 MB image is used as two 5 MB
partitions (B: and C:).

### [BOARD] `STEP_SETTLE_US` (new)

Not present upstream. Debounce window for coalescing a step burst before committing to
a seek — see the `st506_loop` rework below. Currently 10000.

> **Note:** the image file is still opened as `HDD.RLL` even though this is an MFM
> drive, purely to match upstream's `f_open` string.

---

## pico506.h — seek state

### [UPSTREAM-adjacent] Four new fields in the `st506` struct

```c
uint            seek_target;
bool            seek_pending;
bool            needs_load;
absolute_time_t seek_time;
```

Required by the two-phase seek in `st506_loop`. Upstream seeks synchronously on every
step and needs no state.

---

## storage.c — SD reliability

### [UPSTREAM] SPI clock reduced to 8 MHz after init

```c
spi_set_baudrate(pico->storage.sd.spi.inst, 8 * 1000 * 1000);
```

Card init runs at 25 MHz, then the bus drops to 8 MHz for normal operation. At the
higher rate this setup saw multi-block transfer termination failures. A cylinder read
takes ~104 ms at 8 MHz, which still fits inside the controller's seek timeout.

### [UPSTREAM] `ulong` → `DWORD` for the volume serial

`f_getlabel` takes a `DWORD*`. `ulong` is not portable across newlib configurations
and fails to compile in this toolchain. (Same substitution appears twice in
`st506.c` for `absolute_time_diff_us` results, as `unsigned long`.)

---

## pico506.c — build banner

### [BOARD] Build timestamp at boot

```c
LT_I("BUILD %s %s", __DATE__, __TIME__);
```

Added purely to catch flash-vs-build mismatches, which caused a long false trail early
on (a stale binary looked exactly like a wiring fault).

---

## st506.c — the substantive changes

### [UPSTREAM] Head select decoded from **two** bits

```c
// upstream
uint hd = gpio_get(PIN_HEAD_1);
// here
uint hd = gpio_get(PIN_HEAD_1) | (gpio_get(PIN_HEAD_2) << 1);
```

plus `gpio_set_irq_enabled(PIN_HEAD_2, …)` alongside the existing HEAD_1 registration
(and mirrored in `st506_stop`).

Upstream reads a single head-select line, which is correct for its 2-head drive but
silently collapses a 4-head drive onto two slots: heads 0/1 and 2/3 overwrite each
other. Symptom was two heads formatting correctly and two reading back blank.

Anyone running `HEADS > 2` needs this.

### [UPSTREAM] `f_sync` after writing

```c
f_sync(&pico->storage.fp);
```

at the end of `st506_do_write`. Upstream uses `f_write` alone, which leaves data in
the FatFS cache. Every write reported success and nothing durably reached the card —
pulling the SD showed a blank image. Anyone who can power-cycle or remove the card
needs this.

### [UPSTREAM] Dirty-block range computed from byte offsets

```c
// upstream — derived from the DMA end address and transfer count
uint end_byte    = end_addr - (uint)pico->st506.cyl_data;
uint start_byte  = (end_byte - 1) - ((end_byte - 1) % TRACK_BYTES)
                   + TRACK_BYTES - trans_count * sizeof(uint);
uint start_block = start_byte / 512;
uint end_block   = end_byte / 512;

// here — derived from the selected head
uint start_byte  = pico->st506.hd * TRACK_BYTES;
uint end_byte    = start_byte + TRACK_BYTES;
uint start_block = start_byte / 512;
uint end_block   = (end_byte + 511) / 512;
```

Two problems with the upstream form on this geometry.

First, `TRACK_BLOCKS` is `CYLINDER_BLOCKS / HEADS` = `163 / 4` = **40**, but a track is
20808 bytes = **40.64** blocks. Marking `hd * TRACK_BLOCKS` through
`+ TRACK_BLOCKS - 1` therefore covers only 20480 of each track's 20808 bytes. Heads
0–2 get away with it because the *next* head's range overlaps their tail; head 3 has no
neighbour, so blocks 160–162 (bytes 81920–83232) were never marked dirty and never
written. `f_sync` reported `out_bytes = 81920` against a `CYLINDER_BYTES` of 83232.
Symptom: head 3 failed verify on every single cylinder.

Second, deriving the range from `end_addr` makes the accounting depend on where the
capture DMA happened to stop, which is fragile. Since a write always targets the
currently selected head, computing the range from `hd` directly is both correct and
simpler. Rounding up on `end_block` means blocks straddling a head boundary get marked
by both neighbours, which is what you want.

This bug affects any geometry where `CYLINDER_BLOCKS` is not divisible by `HEADS`.

### [UPSTREAM] Two-phase seek — positioning separated from loading

Upstream calls `st506_on_seek()` directly on every step:

```c
uint cyl_next = pico->st506.cyl_next;
if (cyl_next != CYL_INVALID) {
    st506_on_seek(pico, cyl_next);
}
```

The WD1002 performs a **per-step handshake** during a restore: pulse STEP, wait for
SEEK_COMPLETE, pulse again. There is no step burst to coalesce, so debouncing alone
cannot help. Doing a full cylinder load per step meant a 306-cylinder restore took
**9.61 s** — measured — far past the controller's restore timeout, and `k10hdfmt`
aborted with "drive unusable, unable to write track 0".

The replacement splits the two concerns:

1. **On step latch** — drop SEEK_COMPLETE immediately, update TRACK_0, record the
   target and timestamp. Dropping SEEK_COMPLETE here rather than inside `on_seek`
   also fixes a separate bug: the controller otherwise still saw SEEK_COMPLETE
   asserted from the *previous* cylinder and began writing the next cylinder's head 0
   into the old buffer. That showed up on the platter as head 0 of cylinder 100
   carrying an ID stamped cylinder 101.
2. **After `STEP_SETTLE_US`** — set `needs_load`, re-assert SEEK_COMPLETE. The head
   has "arrived"; no SD access yet.
3. **After a further quiet window** — actually call `st506_on_seek()`, which flushes
   any dirty buffer and loads the target cylinder.

During a restore the quiet window never elapses until stepping stops, so 306 loads
collapse into one.

> **Critical:** do **not** set `cyl = CYL_INVALID` before that deferred `on_seek` call.
> `on_seek` opens with `if (cyl == CYL_INVALID) write_any = false;`, which discards
> every pending write. An earlier revision did exactly this and silently threw away all
> file writes while appearing to work. `cyl` must still hold the *old* cylinder so the
> flush targets the right place.

### [UPSTREAM] WRRM drain moved before seek handling

The write-completion FIFO is now drained at the top of `st506_loop`, ahead of the
step/seek block. Previously a seek could flush the cylinder before the last head's
write completion had been accounted for, dropping that head's data.

### [BOARD] Input polarity overrides

```c
gpio_set_inover(PIN_DIR_IN,     GPIO_OVERRIDE_INVERT);
gpio_set_inover(PIN_HEAD_1,     GPIO_OVERRIDE_INVERT);
gpio_set_inover(PIN_HEAD_2,     GPIO_OVERRIDE_INVERT);
gpio_set_inover(PIN_WRITE_GATE, GPIO_OVERRIDE_INVERT);
```

Control inputs arrive active-low through the non-inverting '245. Determined
empirically, and the exceptions matter:

* **STEP does not need it** — it is edge-counted by the PIO, so it survives inversion.
* **WRDATA must not have it** — MFM write data idles high natively, which is what the
  WRDT PIO's `jmp pin` detector expects. Adding `inover` there only flips a constant
  `0xFF` capture into a constant `0x00`.
* **WRITE_GATE was the single hardest bug in the project.** Without the override, the
  WRGT control chain armed the write-capture DMA during the *idle* window and aborted
  it when the real write began. The emulator faithfully captured an idle line, wrote
  that blank template to the card, and then played it back perfectly. Every
  "reads don't work" symptom — for weeks — traced to this one missing line.

### [BOARD] `pio_gpio_init(PIO_RDGT, PIN_SERVO_GATE)` deleted

See the SERVO_GATE note under `config.h`. The pin stays under SIO control so the RDGT
pattern cannot reach SEEK_COMPLETE.

### [BOARD] `/WRITE_FAULT` (GP3) driven low

```c
gpio_init(3);
gpio_set_dir(3, GPIO_OUT);
gpio_put(3, 0);
```

GP3 feeds a ULN2803 input, which has **no internal pull-down**. Leaving it floating
turns the Darlington on and pulls the bus line low — i.e. asserts WRITE FAULT, and the
WD1010 refuses to write. Driven low: transistor off, output high-Z, bus pull-up holds
the line deasserted.

### [BOARD] TRACK_0 asserted from the latched position

```c
// upstream
if (cyl != 0)
    gpio_put(PIN_TRACK_0, false);
// here
gpio_put(PIN_TRACK_0, cyl == 0);
```

plus an assertion in `st506_loop` the moment `cyl_next == 0` is latched — before the
settle wait and before the SD read. Upstream only raises TRACK_0 after the cylinder
load completes, which is ~104 ms too late for the Kaypro's restore.

### [BOARD] Head pointer re-established after a cylinder load

```c
st506_on_head(pico, pico->st506.hd < HEADS ? pico->st506.hd : 0);
```

at the end of `on_seek`. Because `on_head` no longer early-returns on an unchanged
head (see below), this re-points the RDDT DMA at the freshly loaded buffer even when
the head number itself has not changed.

### [BOARD] `on_head` retargets unconditionally, guarded on DMA readiness

Upstream returns early if `hd == pico->st506.hd`. Here the early return is replaced by
a `changed` flag that gates only the clicker and activity timestamp — the DMA retarget
always runs, because the buffer contents may be new even when the head number is not.

The retarget is wrapped in `if (st506_rdgt_ctrl_chan >= 0)` because `st506_start`
calls `on_seek` (and therefore `on_head`) *before* the RDGT DMA channels are claimed.
Without the guard, boot hard-faults retargeting channel −1.

### [BOARD] `cyl_data` allocation

```c
// upstream
malloc(CYLINDER_BYTES)
// here
calloc(1, CYLINDER_BYTES + 256)
```

`calloc` so an unwritten region reads as zeros rather than heap garbage — this matters
when inspecting a pulled SD image, since uninitialised heap is indistinguishable from
real captured flux. The `+ 256` is a defensive pad left over from investigating a
suspected end-of-buffer overrun; it turned out not to be the cause and can be dropped.

---

## Known-good signal reference

For anyone verifying a port, a correctly formatted MFM track played back by this
emulator decodes as:

* 13–14 bytes of `0x00` PLL sync preamble before each address mark
* `A1` sync with the missing-clock violation — raw cell pattern `0x4489`
* `FE` ID address mark, then cylinder, SDH, sector
* CRC-16-CCITT, init `0xFFFF`, computed over `[A1, FE, cyl, SDH, sector]`
* Data field: `A1` then `F8` (deleted — formatted but never written) or `FB` (normal
  data — a real WRITE SECTOR landed)

SDH `0x20` means 512-byte sectors; `0x40` means 1024. A Kaypro expects 17 × 512.

> **Formatter warning:** the Kayplus `HDCNFG` utility writes a non-standard
> high-performance layout (9 × 1024-byte sectors) that a stock Kaypro BIOS cannot read.
> It formats and verifies cleanly against itself, so it looks like success. Use the
> stock `k10hdfmt` instead.

---

## Leftover debug code

Still present in the tree and safe to remove:

| File | Item |
|---|---|
| `st506.c` | `#include "SEGGER_RTT.h"` — no RTT calls remain |
| `st506.c` | `LT_D("RDGT pulse[0]=…")` — twice, in `st506_start` and `on_seek` |
| `st506.c` | 5-iteration DMA transfer-count loop with `sleep_ms(4)` — adds 20 ms to boot |
| `st506.c` | `LT_D("on_head: hd=…")` — fires on every head select |
| `st506.c` | WRITE_GATE wait + `sleep_us(300)` at the top of `on_seek` — added on a theory later disproved by measurement (`WG` is already deasserted when `do_write` runs); costs 300 µs per seek |
| `st506.c` | `FRESULT fsr =` is unused, and `goto end` jumps over its initialiser |
| `st506.c` | commented-out `// uint target = …` / `// pico->st506.cyl = CYL_INVALID;` |

## Known issues

* `st506_on_write` uses `block <= end_block` where `end_block` is exclusive, so it
  marks one block into the next head's range. Harmless — that head marks it anyway —
  but `<` is what was intended.
* `config.h` comments are stale in two places: the geometry header says
  `153 x 4 x 17 x 512` while `CYLINDERS` is 306, and the GP3 note says
  "left undriven" although the code now drives it low (and must).
* `PIN_LED` (GP19) is defined but unused; `PIN_BUZZER` is commented "swapped to led
  for testing".
* Occasionally an intermediate cylinder is loaded before the real target, wasting a
  ~104 ms read.
