#pragma once

#include <cstdint>
#include <vector>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"

namespace esphome {
namespace yoto_battery {

// Yoto Mini rev #04 battery monitoring:
//   CW2215B fuel gauge (this I2C device, 0x64): cell voltage + state of charge
//   SGM41513 charger (0x1A): charge state, input power; optionally applies the
//   stock rev #04 charger configuration and enables charging (configure_charger)
// Register usage follows Open Yoto's battery driver.
class YotoBattery : public PollingComponent, public i2c::I2CDevice {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_charger_address(uint8_t a) { this->charger_address_ = a; }
  void set_configure_charger(bool b) { this->configure_charger_ = b; }
  // Battery care: stop charging at this level (%), resume 5 points below. 100 = off.
  void set_charge_limit(float pct) { this->charge_limit_ = pct; }
  bool is_holding_at_limit() const { return this->charge_paused_; }
  void set_profile(const std::vector<uint8_t> &p) { this->profile_ = p; }
  void set_voltage_sensor(sensor::Sensor *s) { this->voltage_sensor_ = s; }
  void set_level_sensor(sensor::Sensor *s) { this->level_sensor_ = s; }
  void set_charge_state_sensor(text_sensor::TextSensor *s) { this->charge_state_sensor_ = s; }
  void set_charging_sensor(binary_sensor::BinarySensor *s) { this->charging_sensor_ = s; }
  void set_external_power_sensor(binary_sensor::BinarySensor *s) { this->external_power_sensor_ = s; }

  // For automations: last good readings (-1 / false if unknown)
  int get_level() const { return this->level_; }
  bool is_external_power() const { return this->external_power_; }

 protected:
  bool gauge_read_(uint8_t reg, uint8_t *val);
  bool gauge_write_(uint8_t reg, uint8_t val);
  bool charger_read_(uint8_t reg, uint8_t *val);
  bool charger_write_(uint8_t reg, uint8_t val);
  bool charger_update_(uint8_t reg, uint8_t mask, uint8_t value);
  bool configure_charger_now_(bool fresh_input);
  bool set_charging_enabled_(bool enabled);
  void log_charger_regs_(const char *when);
  bool gauge_init_();
  bool profile_matches_();
  bool load_profile_();

  uint8_t charger_address_{0x1A};
  bool configure_charger_{false};
  bool charger_configured_{false};
  uint8_t audit_counter_{0};
  float charge_limit_{100.0f};
  bool charge_paused_{false};
  std::vector<uint8_t> profile_;

  bool gauge_ok_{false};
  bool charger_ok_{false};
  int level_{-1};
  bool external_power_{false};

  sensor::Sensor *voltage_sensor_{nullptr};
  sensor::Sensor *level_sensor_{nullptr};
  text_sensor::TextSensor *charge_state_sensor_{nullptr};
  binary_sensor::BinarySensor *charging_sensor_{nullptr};
  binary_sensor::BinarySensor *external_power_sensor_{nullptr};
};

}  // namespace yoto_battery
}  // namespace esphome
