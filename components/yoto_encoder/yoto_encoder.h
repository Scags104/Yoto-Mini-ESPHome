#pragma once

#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#include <driver/pulse_cnt.h>

namespace esphome {
namespace yoto_encoder {

class StepTrigger : public Trigger<> {};

// Rotary encoder decoded by the ESP32's hardware pulse counter (PCNT) in full
// quadrature, with a hardware glitch filter. Unlike interrupt-based decoding,
// it can't miss or mis-order edges on a fast spin, contact bounce cancels out,
// and it isn't affected by the GPIO36/39 interrupt glitch erratum.
class YotoEncoder : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  void set_pins(int a, int b) {
    this->pin_a_ = a;
    this->pin_b_ = b;
  }
  void set_invert(bool v) { this->invert_ = v; }
  void set_glitch_filter_ns(uint32_t ns) { this->glitch_ns_ = ns; }
  void set_counts_per_detent(int c) { this->counts_per_detent_ = c; }
  void set_max_steps_per_loop(int m) { this->max_steps_per_loop_ = m; }
  void add_on_clockwise_trigger(StepTrigger *t) { this->cw_triggers_.push_back(t); }
  void add_on_anticlockwise_trigger(StepTrigger *t) { this->ccw_triggers_.push_back(t); }

 protected:
  int pin_a_{-1}, pin_b_{-1};
  bool invert_{false};
  uint32_t glitch_ns_{10000};
  int counts_per_detent_{4};
  int max_steps_per_loop_{3};

  pcnt_unit_handle_t unit_{nullptr};
  int last_count_{0};
  int accum_{0};

  std::vector<StepTrigger *> cw_triggers_;
  std::vector<StepTrigger *> ccw_triggers_;
};

}  // namespace yoto_encoder
}  // namespace esphome
