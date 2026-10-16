# Tab5: what is left

The board works (docs/board-tab5.md is the port and its traps; this file is the
work queue). Six things, in the order they are worth doing, plus the two things
that are *unverified* rather than undone. Every number here is measured on the
bench unit - ST7123 glass, silicon rev v1.3 - not estimated.

## 1. The advisor LLM on the P4: 2.2 tok/s (the big one)

**Measured:** a decision takes ~19 s at 2.2 tok/s. The 1.8 board (ESP32-S3,
240 MHz) does the same model at ~12 tok/s, ~3.7 s - the same code, the same
7.56 MB model, read the same way (mmap'd from the model partition).

**Why:** `common/llm/q4_model.c`'s group dot product runs on the ESP32-S3's PIE
128-bit unit (`#if defined(CONFIG_IDF_TARGET_ESP32S3) && !defined(POCKET_TANK_NO_PIE)`).
On this target `CONFIG_POCKET_TANK_NO_PIE=y`, so every weight is unpacked and
dotted **scalar**. The P4 has a PIE unit of its own - the toolchain is already
built for it (`-march=rv32imafc_zicsr_zifencei_xesppie` in every compile line) -
but the kernel has never been written for it.

**Where:** `common/llm/q4_model.c`, the `Q4_PIE` block and the group-dot
functions around it (`q4_model.c:19-219` is the S3 assembly and its scalar
fallback). A new `#elif defined(CONFIG_IDF_TARGET_ESP32P4)` arm, using the P4's
SIMD intrinsics or inline asm (esp-dsp's `dsps_dotprod_*` for the P4, or
`esp.cpu.simd` intrinsics), behind the same interface the scalar path uses.

**How to verify:** the boot log's own line
(`t=... asks N decisions M last <ms> tok/s`), plus `model/runq4.c` on the host
for correctness against the scalar path (bit-exactness is not required, the
goal/urgency picks are what matter - the selftests already check those).

**Watch out:** the P4's PIE is a different ISA from the S3's (RISC-V vs
Xtensa). The S3 arm's assumptions - 16-byte alignment, `gs == 64`, the
unpack-once-dot-many shape - are worth keeping, the instructions are not. And
`POP_CAP=5` in `firmware/components/tankcore/CMakeLists.txt` exists because
advisor latency has never been measured on hardware that could afford 5 fish;
if this lands, that cap is the next thing to re-read.

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
