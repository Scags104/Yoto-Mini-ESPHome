#pragma once

#include <cstddef>
#include <cstdint>

#include "esphome/components/i2c/i2c.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"

namespace esphome {
namespace yoto_aw88194 {

// Awinic AW88194A smart speaker amp on the Yoto Mini rev #04 (mono, I2C 0x34).
// Port of Open Yoto's factory-matched driver: pactrl reset, 35-entry register
// table, DSP firmware + config upload, VCALB, PLL checks, unmute.
//
// setup()           reset pulse + chip ID check (no I2S clocks needed)
// ensure_running()  cold start on first call, PLL re-check afterwards.
//                   Call only while I2S clocks are running (during playback).
// set_muted(bool)   power the speaker output down/up (keeps DSP state)
class AW88194Component : public Component, public i2c::I2CDevice {
 public:
  void setup() override;
  void dump_config() override;
  // After the IO expander has driven level converter / vinhold
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_reset_switch(switch_::Switch *s) { this->reset_switch_ = s; }
  void set_firmware(const uint8_t *data, size_t len) {
    this->fw_ = data;
    this->fw_len_ = len;
  }
  void set_dsp_config(const uint8_t *data, size_t len) {
    this->cfg_ = data;
    this->cfg_len_ = len;
  }

  bool ensure_running();
  bool set_muted(bool muted);
  bool is_identified() const { return this->identified_; }
  bool is_started() const { return this->started_; }

 protected:
  bool write16_(uint8_t reg, uint16_t value);
  bool read16_(uint8_t reg, uint16_t *value);
  bool update_bits_(uint8_t reg, uint16_t mask, uint16_t value);
  bool identify_();
  bool load_factory_regs_();
  bool upload_dsp_(uint16_t base, const uint8_t *data, size_t len);
  bool iis_check_();
  bool syspll_check_();
  bool program_vcalb_();
  bool cold_start_();
  bool validate_ready_();

  switch_::Switch *reset_switch_{nullptr};
  const uint8_t *fw_{nullptr};
  size_t fw_len_{0};
  const uint8_t *cfg_{nullptr};
  size_t cfg_len_{0};

  bool identified_{false};
  bool started_{false};
  bool muted_{true};
  uint16_t chip_id_{0};
};

}  // namespace yoto_aw88194
}  // namespace esphome
