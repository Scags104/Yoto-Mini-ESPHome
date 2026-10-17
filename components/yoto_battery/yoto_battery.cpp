#include "yoto_battery.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cstdio>

namespace esphome {
namespace yoto_battery {

static const char *const TAG = "yoto_battery";

// CW2215B fuel gauge
static const uint8_t CW_REG_ID = 0x00;
static const uint8_t CW_REG_VCELL = 0x02;
static const uint8_t CW_REG_SOC = 0x04;
static const uint8_t CW_REG_MODE = 0x08;
static const uint8_t CW_REG_GPIO_CONFIG = 0x0A;
static const uint8_t CW_REG_SOC_ALERT = 0x0B;
static const uint8_t CW_REG_PROFILE = 0x10;
static const uint8_t CW_REG_IC_STATE = 0xA7;
static const uint8_t CW_CHIP_ID = 0xA0;
static const uint8_t CW_MODE_RESTART = 0x30;
static const uint8_t CW_MODE_ACTIVE = 0x00;
static const uint8_t CW_MODE_SLEEP = 0xF0;
static const uint8_t CW_UPDATE_FLAG = 0x80;
static const uint8_t CW_IC_READY_MASK = 0x0C;
static const size_t CW_PROFILE_SIZE = 80;

// SGM41513 charger
static const uint8_t SGM_REG_INPUT_SOURCE = 0x00;
static const uint8_t SGM_REG_POWER_ON = 0x01;
static const uint8_t SGM_REG_CHARGE_CURRENT = 0x02;
static const uint8_t SGM_REG_CHARGE_VOLTAGE = 0x04;
static const uint8_t SGM_REG_TIMER_CONTROL = 0x05;
static const uint8_t SGM_REG_OVP_VINDPM = 0x06;
static const uint8_t SGM_REG_MISC_CONTROL = 0x07;
static const uint8_t SGM_REG_STATUS = 0x08;
static const uint8_t SGM_REG_FAULT = 0x09;
static const uint8_t SGM_REG_STOCK_CONTROL = 0x0C;
static const uint8_t SGM_CHRG_STAT_MASK = 0x18;
static const uint8_t SGM_PG_STAT = 0x04;
// Stock rev #04 values (Open Yoto battery.c)
static const uint8_t SGM_EN_HIZ = 0x80;
static const uint8_t SGM_STAT_CTRL_MASK = 0x60;
static const uint8_t SGM_OTG_CONFIG = 0x20;
static const uint8_t SGM_CHG_CONFIG = 0x10;
static const uint8_t SGM_Q1_FULLON = 0x40;
static const uint8_t SGM_ICHG_MASK = 0x3F;
static const uint8_t SGM_ICHG_CODE_REV04 = 0x36;      // 2220 mA, as stock requests
static const uint8_t SGM_VRECHG = 0x01;
static const uint8_t SGM_WATCHDOG_MASK = 0x30;
static const uint8_t SGM_OVP_VINDPM_REV04 = 0xC5;
static const uint8_t SGM_BATFET_DIS = 0x20;
static const uint8_t AUDIT_EVERY_UPDATES = 10;        // ~5 min at 30 s

bool YotoBattery::gauge_read_(uint8_t reg, uint8_t *val) { return this->read_register(reg, val, 1) == i2c::ERROR_OK; }

bool YotoBattery::gauge_write_(uint8_t reg, uint8_t val) {
  return this->write_register(reg, &val, 1) == i2c::ERROR_OK;
}

bool YotoBattery::charger_read_(uint8_t reg, uint8_t *val) {
  return this->bus_->write_readv(this->charger_address_, &reg, 1, val, 1) == i2c::ERROR_OK;
}

bool YotoBattery::charger_write_(uint8_t reg, uint8_t val) {
  const uint8_t data[2] = {reg, val};
  return this->bus_->write_readv(this->charger_address_, data, 2, nullptr, 0) == i2c::ERROR_OK;
}

bool YotoBattery::charger_update_(uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t cur;
  if (!this->charger_read_(reg, &cur))
    return false;
  const uint8_t updated = (cur & static_cast<uint8_t>(~mask)) | (value & mask);
  if (updated == cur)
    return true;
  return this->charger_write_(reg, updated);
}

void YotoBattery::log_charger_regs_(const char *when) {
  char line[64];
  size_t pos = 0;
  for (uint8_t reg = 0x00; reg <= 0x0C; reg++) {
    uint8_t v = 0;
    const bool ok = this->charger_read_(reg, &v);
    pos += snprintf(line + pos, sizeof(line) - pos, ok ? " %02X" : " --", v);
  }
  ESP_LOGD(TAG, "SGM41513 regs 00-0C (%s):%s", when, line);
}

// Port of Open Yoto's sgm41513_configure() for rev #04. Charging is enabled
// only after every current/voltage/safety register is in place.
bool YotoBattery::configure_charger_now_(bool fresh_input) {
  // Clear HIZ and STAT mode; keep the charger's own input-current detection
  if (!this->charger_update_(SGM_REG_INPUT_SOURCE, SGM_EN_HIZ | SGM_STAT_CTRL_MASK, 0))
    return false;
  if (!this->charger_update_(SGM_REG_POWER_ON, SGM_OTG_CONFIG, 0))
    return false;
  if (!this->charger_update_(SGM_REG_CHARGE_CURRENT, SGM_Q1_FULLON | SGM_ICHG_MASK,
                             SGM_Q1_FULLON | SGM_ICHG_CODE_REV04))
    return false;
  if (!this->charger_update_(SGM_REG_CHARGE_VOLTAGE, SGM_VRECHG, SGM_VRECHG))
    return false;
  // Nothing feeds the charger watchdog, so disable it (as stock does)
  if (!this->charger_update_(SGM_REG_TIMER_CONTROL, SGM_WATCHDOG_MASK, 0))
    return false;
  if (!this->charger_update_(SGM_REG_OVP_VINDPM, 0xCF, SGM_OVP_VINDPM_REV04))
    return false;
  // BATFET_DIS can be latched by over-current protection: only clear it on a
  // fresh USB connection, never during periodic re-checks
  if (fresh_input && !this->charger_update_(SGM_REG_MISC_CONTROL, SGM_BATFET_DIS, 0))
    return false;
  // Two additional stock writes on the rev #04 (no USB-PD) path
  if (!this->charger_update_(SGM_REG_TIMER_CONTROL, 0x01, 0))
    return false;
  if (!this->charger_write_(SGM_REG_STOCK_CONTROL, 0x45))
    return false;
  // Enable charging last - unless battery care is holding the charge
  return this->set_charging_enabled_(!this->charge_paused_);
}

bool YotoBattery::set_charging_enabled_(bool enabled) {
  return this->charger_update_(SGM_REG_POWER_ON, SGM_CHG_CONFIG, enabled ? SGM_CHG_CONFIG : 0);
}

bool YotoBattery::profile_matches_() {
  if (this->profile_.size() != CW_PROFILE_SIZE)
    return false;
  for (size_t i = 0; i < CW_PROFILE_SIZE; i++) {
    uint8_t v;
    if (!this->gauge_read_(CW_REG_PROFILE + i, &v) || v != this->profile_[i])
      return false;
  }
  return true;
}

bool YotoBattery::load_profile_() {
  ESP_LOGI(TAG, "Writing battery profile to CW2215B");
  // Restart -> sleep, write profile, clear GPIO config, set update flag
  if (!this->gauge_write_(CW_REG_MODE, CW_MODE_RESTART))
    return false;
  delay(20);
  if (!this->gauge_write_(CW_REG_MODE, CW_MODE_SLEEP))
    return false;
  delay(10);
  for (size_t i = 0; i < CW_PROFILE_SIZE; i++) {
    if (!this->gauge_write_(CW_REG_PROFILE + i, this->profile_[i]))
      return false;
  }
  return this->gauge_write_(CW_REG_GPIO_CONFIG, 0x00) && this->gauge_write_(CW_REG_SOC_ALERT, CW_UPDATE_FLAG);
}

bool YotoBattery::gauge_init_() {
  uint8_t id = 0, mode = 0, alert = 0;
  if (!this->gauge_read_(CW_REG_ID, &id) || id != CW_CHIP_ID) {
    ESP_LOGE(TAG, "CW2215B not found (ID 0x%02X, expected 0x%02X)", id, CW_CHIP_ID);
    return false;
  }
  if (!this->gauge_read_(CW_REG_MODE, &mode) || !this->gauge_read_(CW_REG_SOC_ALERT, &alert))
    return false;

  const bool has_profile_flag = (alert & CW_UPDATE_FLAG) != 0;
  ESP_LOGD(TAG, "CW2215B mode=0x%02X profile flag=%s", mode, YESNO(has_profile_flag));

  if (!this->profile_.empty()) {
    // Same policy as stock/Open Yoto: reload if the flag is clear or contents differ
    const bool matches = has_profile_flag && this->profile_matches_();
    if (mode == CW_MODE_ACTIVE && matches)
      return true;
    if (!matches && !this->load_profile_())
      return false;
  } else {
    if (!has_profile_flag)
      ESP_LOGW(TAG, "No battery profile loaded in the gauge; the percentage may be inaccurate. "
                    "Add battery_profile to load one.");
    if (mode == CW_MODE_ACTIVE)
      return true;
  }

  // Bring the gauge to active mode and wait for it to report ready
  if (!this->gauge_write_(CW_REG_MODE, CW_MODE_RESTART))
    return false;
  delay(20);
  if (!this->gauge_write_(CW_REG_MODE, CW_MODE_ACTIVE))
    return false;
  for (int attempt = 0; attempt < 50; attempt++) {
    delay(100);
    uint8_t state;
    if (!this->gauge_read_(CW_REG_IC_STATE, &state))
      return false;
    if ((state & CW_IC_READY_MASK) == CW_IC_READY_MASK)
      return true;
  }
  ESP_LOGW(TAG, "CW2215B did not report ready");
  return false;
}

void YotoBattery::setup() {
  this->gauge_ok_ = this->gauge_init_();
  uint8_t status;
  this->charger_ok_ = this->charger_read_(SGM_REG_STATUS, &status);
  if (!this->gauge_ok_ || !this->charger_ok_)
    this->status_set_warning("Battery gauge or charger not responding");
}

void YotoBattery::update() {
  // --- Fuel gauge ---
  uint8_t buf[2];
  if (this->read_register(CW_REG_VCELL, buf, 2) == i2c::ERROR_OK) {
    const uint16_t raw = static_cast<uint16_t>((buf[0] << 8) | buf[1]) & 0x3FFF;
    const float volts = raw * 0.3125f / 1000.0f;
    if (this->voltage_sensor_ != nullptr)
      this->voltage_sensor_->publish_state(volts);
  }
  uint8_t soc;
  if (this->gauge_read_(CW_REG_SOC, &soc) && soc <= 100) {
    this->level_ = soc;
    if (this->level_sensor_ != nullptr)
      this->level_sensor_->publish_state(soc);
  }

  // --- Charger status (and optional configuration) ---
  uint8_t status;
  if (this->charger_read_(SGM_REG_STATUS, &status)) {
    this->external_power_ = (status & SGM_PG_STAT) != 0;

    if (this->configure_charger_) {
      if (!this->external_power_) {
        if (this->charger_configured_)
          ESP_LOGI(TAG, "USB power removed");
        this->charger_configured_ = false;
      } else if (!this->charger_configured_) {
        this->log_charger_regs_("before");
        if (this->configure_charger_now_(true)) {
          this->charger_configured_ = true;
          this->audit_counter_ = 0;
          uint8_t fault = 0;
          this->charger_read_(SGM_REG_FAULT, &fault);
          delay(50);
          this->charger_read_(SGM_REG_STATUS, &status);
          ESP_LOGI(TAG, "Charger configured, charging enabled (status 0x%02X, fault 0x%02X)", status, fault);
          this->log_charger_regs_("after");
        } else {
          ESP_LOGW(TAG, "Charger configuration failed; will retry");
        }
      } else if (++this->audit_counter_ >= AUDIT_EVERY_UPDATES) {
        this->audit_counter_ = 0;
        if (!this->configure_charger_now_(false))
          ESP_LOGW(TAG, "Charger re-check failed");
      }

      // Battery care: hold the charge at the limit (with 5 points of hysteresis).
      // Only charging is switched off; the Yoto still runs from USB.
      if (this->charger_configured_ && this->level_ >= 0) {
        const bool limit_on = this->charge_limit_ < 99.5f;
        if (limit_on && !this->charge_paused_ && this->level_ >= this->charge_limit_) {
          if (this->set_charging_enabled_(false)) {
            this->charge_paused_ = true;
            ESP_LOGI(TAG, "Battery at %d%%: holding at the %.0f%% charge limit", this->level_, this->charge_limit_);
          }
        } else if (this->charge_paused_ && (!limit_on || this->level_ <= this->charge_limit_ - 5.0f)) {
          if (this->set_charging_enabled_(true)) {
            this->charge_paused_ = false;
            ESP_LOGI(TAG, "Battery at %d%%: charging resumed", this->level_);
          }
        }
      }
    }
    if (!this->external_power_)
      this->charge_paused_ = false;

    const uint8_t chrg = (status & SGM_CHRG_STAT_MASK) >> 3;
    static const char *const STATES[] = {"Not charging", "Pre-charging", "Charging", "Charged"};
    if (this->charge_state_sensor_ != nullptr)
      this->charge_state_sensor_->publish_state(
          !this->external_power_ ? "On battery" : (this->charge_paused_ ? "Holding at limit" : STATES[chrg]));
    if (this->charging_sensor_ != nullptr)
      this->charging_sensor_->publish_state(chrg == 1 || chrg == 2);
    if (this->external_power_sensor_ != nullptr)
      this->external_power_sensor_->publish_state(this->external_power_);
    this->charger_ok_ = true;
  } else {
    this->charger_ok_ = false;
  }

  if (this->gauge_ok_ && this->charger_ok_)
    this->status_clear_warning();
}

void YotoBattery::dump_config() {
  ESP_LOGCONFIG(TAG, "Yoto battery (CW2215B + SGM41513):");
  LOG_I2C_DEVICE(this);
  ESP_LOGCONFIG(TAG, "  Charger address: 0x%02X", this->charger_address_);
  ESP_LOGCONFIG(TAG, "  Configure charger: %s", YESNO(this->configure_charger_));
  ESP_LOGCONFIG(TAG, "  Gauge OK: %s, charger OK: %s", YESNO(this->gauge_ok_), YESNO(this->charger_ok_));
  ESP_LOGCONFIG(TAG, "  Battery profile: %s", this->profile_.empty() ? "not set (read-only)" : "set");
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "Voltage", this->voltage_sensor_);
  LOG_SENSOR("  ", "Level", this->level_sensor_);
}

}  // namespace yoto_battery
}  // namespace esphome
