# Tab5: what is left

The board works (docs/board-tab5.md is the port and its traps; this file is the
work queue). Six things, in the order they are worth doing, plus the two things
that are *unverified* rather than undone. Every number here is measured on the
bench unit - ST7123 glass, silicon rev v1.3 - not estimated.

## 1. The advisor LLM on the P4: done, 2.2 -> 10.6 tok/s

**Measured:** a decision went from ~19.5 s at 2.2 tok/s to **4.06 s at 10.6
tok/s** (the 1.8 board: ~12 tok/s, ~3.7 s) - the project's own line is that
"decisions past 4 s are too slow", so this board is now on it rather than far
past it.

**What landed:** `common/llm/q4_model.c` has a third kernel arm for this chip.
The S3's asm translated one mnemonic at a time - `esp.zero.xacc`,
`esp.vld.128.ip`, `esp.vmulas.s8.xacc` (the same 40-bit scalar accumulator the
MAC sums its 16 int8 products into) and `esp.srs.s.xacc rd, rshift` reading it
out where the S3 says `rur.accx_0 rd`. Boot bench, before -> after:

```
dot100k        164706 us ->   8462 us   (19.5x)
batch44 flash  415717 us ->  35340 us   (11.8x)
batch44 ram    412987 us ->  32608 us
decode flash    21593 us ->  12008 us   ( 1.8x)
decode ram      18999 us ->   9322 us
```

**The trap, for whoever touches this next:** every general-purpose operand of a
PIE instruction must be one of **twelve registers** - a0-a5, t3-t6, s0, s1. The
assembler's `illegal operands` is the only warning, and `zero` (x0) is not one of
them, so the read-out's shift needs a register holding 0. The pointers are pinned
with local register variables; esp-dsp's `*_arp4.S` moves its argument registers
into t3/t4 for the same reason.

**What is left here:** the *single-token* path still unpacks each 4-bit group in
C (`unpack_group`), and that is now the decode bottleneck - the group dot is fast
enough that 32 scalar nibble-splits per 64 weights cost more than the dot does.
The S3 answers it with its `Q4_PK` kernel (`ee.andq` + `ee.vsr.32`, unpacking
inside the vector unit); the P4 arm sets `Q4_PK 0` and takes the packed pointer
path. Porting that is the next real win on this board, and the same trick applies
to `matmul_batch`'s unpack.

## 2. Sound: the ES8388 (silence now)

**Measured:** `codec: no ES8311 at 0x18/0x19` - the port probes the Waveshare
boards' codec. This board's codec is an **ES8388 at I2C 0x10** (the I2C scan
finds it), with the speaker amp on the expander's **SPEAKER_EN (0x43 bit 1)**,
which this port currently leaves LOW (deliberately: `display_port_tab5.c`'s
`expander_panel_on`).

**Where:** a new `audio_port_es8388.c` alongside `audio_port_es8311.c` (the
same `audio_port.h` contract: `audio_port_init/play/stop/...`), plus the I2S
pins in `board_pins.h` (`T_PIN_I2S_*`, already named) and `CONFIG_POCKET_TANK_AUDIO_*`
selection in `firmware/main/CMakeLists.txt`. `esp_codec_dev` (already in the
tree through the BSP) has an ES8388 driver, or the register sequence can be
written the way the ES8311's was.

**How to verify:** `docs/AUDIO.md`'s cues through the settings page's volume
row, and the amp's idle-off behaviour (the port's whole point is that the codec
and the amp are down when nothing plays).

## 3. The clock: the RX8130CE (a real RTC at last)

**Measured:** `rtc: no RTC chip on this board` - `tankcore/rtc_port_pcf85063.c`
knows only the PCF85063 (0x51). This board has an **RX8130CE at 0x32**.

**Why it matters:** the progression layer's "ravenous after an absence" rule
wants a clock that survives a power-off. Without one this board takes the
clockless path (`net_port_stub` cannot give the time back either, since there is
no radio).

**Where:** `rtc_port_rx8130.c` in `firmware/components/tankcore/`, selected by
Kconfig; the port interface is `rtc_port.h` (init / now / set).

## 4. Auto-flip: the BMI270

**Measured:** `imu: no QMI8658` - `imu_port_qmi8658.c` probes the Waveshare
board's part. This board has a **BMI270 at 0x68**.

**Why it matters:** `main.c` calls `imu_port_inverted()` every frame to turn the
picture with the device; today it always says "upright", so the tank ignores
being turned over. The **settings page's ROTATION lock** works already, so this
is only the *live* flip.

**Where:** a new `imu_port_bmi270.c` against `imu_port.h` (`init / poll /
moving / handled / inverted / sleep`); the BSP's `espressif/bmi270` component is
already in the tree.

## 5. The battery meter: the INA226

**Measured:** `battery: no AXP2101` - the port is AXP2101-only, and this board
has none. It has an **INA226 at 0x41** (bus voltage, shunt current) plus the
tab5's charge state on the expander. The meter is hidden ("battery -1%").

**Where:** `battery_port_ina226.c` against `battery_port.h`. Note what the
interface wants beyond a voltage: `BAT_ON_BATTERY / BAT_CHARGING / BAT_FULL /
BAT_PLUGGED` (the charger's phase), `battery_port_poweroff()` (this board has no
PMIC to cut - the honest answer is "cannot", which is what the port is allowed
to say), and the battery page's history in NVS.

## 6. Radio: the ESP32-C6 over SDIO

The P4 has no Wi-Fi of its own: this board's Wi-Fi 6 is an ESP32-C6 wired to the
P4 over SDIO, and the expander's **WIFI_EN (0x44 bit 0)** powers it. Bringing it
up means `espressif/esp_hosted` (with the C6's slave firmware in a partition)
plus `esp_wifi_remote`, and then `net_port_esp.c` could be built for this target
instead of `net_port_stub.c`. Until then: no updates over the air (the cable
works), no Wi-Fi step in the first-run page, and no clock from the internet.

**Why it is last:** it is the only item that needs a new flash partition and a
second chip's firmware, and nothing else in the tank waits on it.

---

## Unverified, not undone

1. **The five-world gate.** BOARDS.md: nothing in `common/` is done until
   `make -C sim check-all` passes in every world. `make TAB5=1` and `check-all`'s
   fifth line exist, but the bench machine has **no host C toolchain and no
   SDL2** (no make, gcc, clang, msys), so the Tab5 world's selftests have never
   run. The geometry this port adds is small (`TANK_W/H` 720x405,
   `ALGAE_CELLS` 576, `VEG_SEG_PX` 3.5), but the pages' layout in a 720 x 405
   world has only been *seen*, not asserted. Run it on a machine with a host
   toolchain before this is called finished.
2. **The touch's landing calibration.** The 1.8, the bowl, the watch and the
   square each carry a fitted 3x3 ("reported = a x + b"); this one uses the
   shared 10 px bias only. A fingertip's own scatter on a 5-inch glass is ~10 px,
   so it is usable as it stands - `director touch bias <px>` tunes it live, and
   `tools/touch_calib.py` does the fitting when someone wants it exact.

## The port's own numbers, for the next reader

```
Discovered board version 2 (LCD ST7123, Touch ST7123, FW 3)
panel up (720 x 1280, 2 frame buffers)
touch ready (ST712x at 400 kHz)
25.7 fps | render 17.7 ms flush 1.6 ms
frame 38.9 ms = polls 0.3 + tick 0.7 + prefetch wait 0.0 + render 17.7 + flush 1.6 + tail 0.0 + sleep 18.6
flush parts (Tab5): prefetch issue 0.0 + port 1.1 + touch 0.5 = 1.6 of 1.6
```

25.7 fps is this project's own ceiling (`FRAME_MIN_MS`, 25 fps: frames and LLM
decisions share one budget), so the tank is not frame-limited here - the render
uses 17.7 ms of the ~24 ms it is allowed, and the 18.6 ms "sleep" is the cap
working. The frame *before* item 1 lands is what limits this board.
