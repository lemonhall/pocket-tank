/* battery_port_tab5.c - the Tab5 has no battery, so this port does not pretend
 * to be a fuel gauge: battery_port_read() always says no, and the tank hides the
 * meter exactly as it does on a board with no PMIC. What the board *does* have
 * is an INA226 (I2C 0x41) on its DC input, and that is worth reporting: the log
 * line's "battery -1% <mV>" then shows the supply (5 V present and steady, or
 * sagging under load), and the director's `battery` dumps the chip's bus/shunt
 * voltage, current and power.
 *
 * The INA226 is a shunt monitor, so its current and power registers need the
 * board's shunt resistance: 5 mOhm is *assumed* here (a common choice for a
 * 5 A-capable input monitor, and the Tab5's schematic is not in this repo). The
 * bus and shunt voltages need no such assumption - they are what the chip
 * measures - so the derived current is logged with the assumption spelled out,
 * and a schematic check can correct the one constant below. */
#include "battery_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include <string.h>

static const char *TAG = "battery";

#define INA226_ADDR    0x41
#define REG_CONFIG     0x00
#define REG_SHUNT_V    0x01     /* 2.5 uV/LSB, signed */
#define REG_BUS_V      0x02     /* 1.25 mV/LSB */
#define REG_POWER      0x03
#define REG_CURRENT    0x04
#define REG_CAL        0x05
#define REG_MFG_ID     0xFE     /* 0x5449 = "TI" */
#define REG_DIE_ID     0xFF     /* 0x2260 */

#define SHUNT_MOHM     5        /* ASSUMED (see the header) */
#define CURRENT_LSB_MA 1        /* 1 mA per LSB: CAL = 0.00512 / (lsb_A * R_ohm) */
#define CAL_VALUE      (int)(0.00512f / (0.001f * (SHUNT_MOHM / 1000.0f)))

/* AVG=16 samples, 1.1 ms conversions, shunt+bus continuous */
#define CONFIG_VALUE   0x4927

static i2c_master_dev_handle_t s_dev;
static int64_t s_next_us;
static int s_bus_mv;            /* cached ~1 s, like the AXP2101 port */

static bool rd16(uint8_t reg, uint16_t *out) {
    uint8_t b[2];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, b, 2, 100) != ESP_OK) return false;
    *out = (uint16_t)((b[0] << 8) | b[1]);
    return true;
}
static bool wr16(uint8_t reg, uint16_t val) {
    uint8_t b[3] = { reg, (uint8_t)(val >> 8), (uint8_t)val };
    return i2c_master_transmit(s_dev, b, 3, 100) == ESP_OK;
}

static void refresh(void) {
    int64_t now = esp_timer_get_time();
    if (now < s_next_us) return;
    s_next_us = now + 1000000;
    uint16_t bus = 0;
    if (rd16(REG_BUS_V, &bus)) s_bus_mv = (int)(bus * 1.25f);   /* 1.25 mV/LSB */
    else s_bus_mv = 0;
}

bool battery_port_init(i2c_master_bus_handle_t bus) {
    if (!bus || i2c_master_probe(bus, INA226_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "no INA226 at 0x%02x: no supply monitor", INA226_ADDR);
        return false;
    }
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = INA226_ADDR, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) return false;
    uint16_t mfg = 0, die = 0;
    if (!rd16(REG_MFG_ID, &mfg) || !rd16(REG_DIE_ID, &die) || mfg != 0x5449 || die != 0x2260) {
        ESP_LOGW(TAG, "INA226 ids 0x%04x/0x%04x (want 0x5449/0x2260)", mfg, die);
        s_dev = NULL; return false;
    }
    wr16(REG_CONFIG, CONFIG_VALUE);
    wr16(REG_CAL, (uint16_t)CAL_VALUE);
    refresh();
    ESP_LOGI(TAG, "INA226 at 0x%02x: this board has no battery, so the meter stays hidden; its bus reads %d mV "
                  "(the rail it watches; current assumes a %d mOhm shunt)", INA226_ADDR, s_bus_mv, SHUNT_MOHM);
    return true;
}

/* no battery: the tank hides the pill and the low-battery notices, and reads the
 * supply from battery_port_vbat_mv() instead */
bool battery_port_read(float *frac, bool *charging) {
    if (frac) *frac = -1.0f;
    if (charging) *charging = false;
    return false;
}
int battery_port_state(void) {
    refresh();
    return s_bus_mv >= 4000 ? BAT_PLUGGED : BAT_ON_BATTERY;   /* only ever "on the cable" or nothing */
}
int battery_port_vbat_mv(void) { refresh(); return s_bus_mv; }
bool battery_port_poweroff(void) { return false; }            /* no PMIC to cut */

/* the side button is PWR/RST/BOOT in hardware; the tank gets no key events */
void battery_port_key_init(void) {}
int  battery_port_key_poll(void) { return 0; }
void battery_port_key_trace(int seconds) { (void)seconds; }

/* no PMIC rails: the Tab5's rails are fixed. The audio and display ports on this
 * board do not call these (board-tab5.md), so they only have to be honest. */
bool battery_port_set_rail(const char *name, bool on) { (void)name; (void)on; return false; }
void battery_port_trim_rails(void) {}
void battery_port_pin_rail(const char *name) { (void)name; }

int  battery_port_charge_ma(void) { return 0; }
bool battery_port_set_charge_ma(int ma) { (void)ma; return false; }
void battery_port_charge_report(void) { ESP_LOGI(TAG, "no charger on this board (USB-C input only)"); }

void battery_port_dump(void) {
    if (!s_dev) { ESP_LOGI(TAG, "no INA226"); return; }
    uint16_t cfg = 0, bus = 0, sh = 0, cur = 0, pwr = 0, cal = 0;
    rd16(REG_CONFIG, &cfg); rd16(REG_BUS_V, &bus); rd16(REG_SHUNT_V, &sh);
    rd16(REG_CURRENT, &cur); rd16(REG_POWER, &pwr); rd16(REG_CAL, &cal);
    float shunt_uv = (float)(int16_t)sh * 2.5f;
    int ma_shunt = (int)(shunt_uv / SHUNT_MOHM);            /* uV / mOhm = mA */
    ESP_LOGI(TAG, "INA226 cfg 0x%04x cal %u | bus %d mV | shunt %.1f uV = %d mA at %d mOhm | "
                  "current reg %d mA | power reg %u x25 mW (assumed shunt: check the schematic)",
             cfg, cal, (int)(bus * 1.25f), shunt_uv, ma_shunt, SHUNT_MOHM, (int)(int16_t)cur, pwr);
}
