# Tab5: what was left, and where each item stands

Items 1, 3, 4 and 5 are done and verified on the unit; 2 (sound) is written but
blocked by the chip's I2S, with the whole trail below; 6 (the radio) is
deliberately not brought up, with the argument for that in its section. The two
things that are *unverified* rather than undone are at the end. Every number
here is measured on the bench unit - ST7123 glass, silicon rev v1.3 - not
estimated.

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

**Where the time goes now, if anyone wants more** (not needed for the bar): the
*single-token* path still unpacks each 4-bit group in C (`unpack_group`), and the
PIE dot is now fast enough that the unpack dominates the decode - one w1 matrix
costs 0.52 ms of dotting against 12.0 ms total. The S3 answers this with its
`Q4_PK` kernel (`ee.andq` + `ee.vsr.32`, unpacking inside the vector unit); the
P4 arm sets `Q4_PK 0`. A port is *possible* but was not done: the P4 exposes
shifts only through its `src`/`srci` family, and a hardware probe (feeding
`0x10..0x2f` through `esp.srci.2q q, q, 4` and `esp.src.q` with `sar.bytes`)
matched no simple shift interpretation - `srci.2q` added 8 to the first half's
bytes and the funnel shift returned zeros - so those semantics want a datasheet
and not a guess. The low-risk version of the same idea is a 256-entry
byte -> two centred int8 values lookup table in `unpack_group`.

## 2. Sound: the port is written, the chip's I2S is the block

**Measured:** the ES8388 answers at I2C 0x10 and the tank's codec device opens
("ES8388: Start on mode:2", "Adev_Codec: Open codec device OK", 17 of 24 cues,
518 KB bank), but **I2S TX does not start on this unit**: every write times out
with 0 bytes done (`ESP_ERR_TIMEOUT`/263), the DMA's descriptor queue never
frees, and the BCLK pad is dead (0 of 4000 samples high, 0 transitions). Tried
and all identical: I2S0 and I2S1; `I2S_CLK_SRC_XTAL` and `I2S_CLK_SRC_APLL`; 16
kHz and 48 kHz; MONO and STEREO; the BSP's own channel and this port's; and
*before* the display/PPA/DSI are initialised as well as after. The APLL does
engage ("APLL is occupied already, it is working at 8191999 Hz"), so the clock
tree's source selection is not the problem - the peripheral never shifts a frame.

**One real trap found and fixed on the way** (keep it even once the above is
solved): `I2S_CLK_SRC_DEFAULT` (0) means "auto-select the fastest source", which
on the P4 is `I2S_CLK_SRC_PLL_160M` - and `clk_tree_defs.h` marks that source
"only supported on P4 hw_ver3". This unit is **pre-v3** (rev v1.3), so
`audio_port_tab5.c` asks for the APLL explicitly, both in its own `std` config
and in the codec dev's `audio_codec_i2s_cfg_t.clk_src` (which is what the codec
dev reconfigures with). The same trap exists in the Zephyr port of this driver
([zephyr#114566](https://github.com/zephyrproject-rtos/zephyr/issues/114566)).

**What the port does about it:** `bring_up()` bounds itself - the first failed
write ends the attempt, `i2s_dead()` says why once, and later cues are answered
without touching the I2S again (`snd` on the console prints `i2s-dead`). Without
that, one bring-up costs 15 x 1 s of timeouts and the tank stalls a second per
cue with nothing to show for it.

**What is left, in the order worth trying:** (1) build a minimal I2S TX test
against another IDF line (5.4.x, or a newer 5.5.x than the v5.5.6 used here) to
see whether this is a regression or a pre-v3 gap in the I2S HAL - the tank must
stay on 5.5.x for the BSP's `usb` dependency, so a fix has to come from IDF;
(2) ask Espressif/M5 whether pre-v3 P4 silicon needs an I2S workaround (the
"huge hardware difference" note in `Kconfig.hw_support` suggests it may);
(3) in the meantime the tank is silent but not slowed, and this board's other
four ports below are the ones to finish first.

## 3. The clock: done - the RX8130CE

**Shipped:** `rtc_port_rx8130.c` in `firmware/components/tankcore/`, chosen by
target in that component's CMakeLists (the four Waveshare boards keep the
PCF85063). This board has a Seiko **RX8130CE at 0x32**, a different register map
(0x10..0x16 BCD, 0x17 bit 7 = the "supply dipped" VBL flag, 0x1F = the
supplied-in reset/test state).

**Verified on the unit:** the first boot seeded it from the build time
(2026-10-16 22:43:57), the next reset read it back and set the system clock
(22:44:16, ~19 s later - so the chip is ticking, not just holding).

**Two things the first version got wrong**, both now guarded: a factory-fresh
chip answers with bytes that are not BCD at all (0xA7 reads through bcd2bin as
year 107 = "2107", which passes a "since 2024" test), so every register is
checked nibble-wise with the year windowed to 2024..2100; and a chip left in its
reset/test state (0x1F bit 6/7) does not tick, so that state is cleared at init,
loudly.

## 4. The flip: done - the BMI270

**Shipped:** `imu_port_bmi270.c`, chosen by target in `main/CMakeLists.txt`. The
driver is Espressif's `espressif/bmi270` (declared for the P4 in
`idf_component.yml`), because this chip does not run its data path until an 8 KB
configuration blob has been downloaded into it - not something to do by hand.

**The physics is the QMI8658 port's, unchanged**, in g instead of counts: flip
0.214 g, motion 0.0134 g, a 250 ms poll, three polls the other way up before
flipping, "handled" requiring two polls in a row. The component works in g and
the contract wants counts at +-2 g (the director's `imu` prints them), so the
scaling happens at that boundary.

**Calibrated on the unit** (standing in its stand, tilted back ~30 degrees):
g = [+0.866 +0.070 -0.514] - the screen's vertical in landscape is the panel's
short axis X, so up = +X. Verified: the driver initialises, the samples are
stable, no spurious flip fires.

## 5. The gauge: there is no battery, so the port reports the rail

**Measured:** the Tab5 has no battery (USB-C/DC input only), and Espressif's BSP
has no battery code at all. The AXP2101 port answered "no PMIC" and the meter
stayed hidden - which was already correct, but it told nobody anything.

**What landed:** `battery_port_tab5.c` - the board's **INA226** (I2C 0x41,
verified by its TI ids 0x5449/0x2260) configured for 16-sample averages,
1.1 ms conversions, shunt+bus continuous. `battery_port_read()` always says no,
so the pill and the low-battery notices stay hidden exactly as before; the log's
`battery -1% <mV>` now carries the INA226's own bus voltage (measured 8161 mV on
the bench, which is the rail it watches - the schematic would say which), and
the director's `battery` dumps bus/shunt/current/power. Its current and power
registers need the board's shunt resistance: **5 mOhm is assumed and said so** -
the bus and shunt voltages need no assumption, so the derived current is logged
with that caveat rather than presented as measured.
`set_rail`/`trim_rails`/`pin_rail` are no-ops (no PMIC: this board's rails are
fixed), `poweroff()` is false (nothing to cut), and the key hooks are empty - the
side button is PWR/RST/BOOT in hardware and the tank gets no key events from it.

## 6. The radio: deliberately not brought up, and here is the argument

**The tank does not need the internet.** The advisor is on-device (7.5 MB Q4
model in flash, 10.6 tok/s on this chip), the save is local NVS, and every cue
and every rule is local. What the radio adds is exactly three conveniences, in
order of value:

1. **over-the-air updates** (from 0.3 a tank updates itself) - without it the
   cable is the only path, which works today (`tools/flash_tab5.ps1`);
2. **first-time provisioning over the installer page** (Improv: tell it your
   Wi-Fi from a browser) - without it the cable is again the path;
3. **network time** - and the Tab5 now has its own RX8130CE (item 3), so this
   one is already covered.

**Nothing hangs or lies without it.** `net_port_stub.c` answers *every* network
call at once with `NET_FAILED`/`NET_ERR_RADIO` (its own comment: "so no page
waits on it"), and `update.c` turns that reason into its own page: "NO RADIO -
THE WI-FI RADIO DID NOT START. TRY AGAIN LATER". The scan/connect/check/install
calls all return immediately, and `net_time_sync` returns false.

**What it would take**, if it is ever wanted: the P4 has no radio of its own, so
Wi-Fi 6 comes from the board's **ESP32-C6 over SDIO** - `espressif/esp_hosted`
with the C6's slave firmware in a new partition, `esp_wifi_remote`, the
expander's WIFI_EN (0x44 bit 0), and a replacement for `net_port_stub.c` with
`net_port_esp.c` on this target. That is a multi-hour, partition-touching job
whose only payoff is items 1 and 2 above, and - given that the I2S peripheral
turned out not to start on this pre-v3 silicon (item 2) - it may well meet its
own platform surprises. Left undone on purpose, not by oversight.

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
