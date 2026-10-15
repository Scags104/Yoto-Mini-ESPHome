#include "yoto_encoder.h"

#include "esphome/core/log.h"

#include <cstdlib>

namespace esphome {
namespace yoto_encoder {

static const char *const TAG = "yoto_encoder";
static constexpr int LIMIT = 1000;      // PCNT counter range (+/-)
static constexpr int RECENTER_AT = 500; // clear the hardware counter past this

void YotoEncoder::setup() {
  pcnt_unit_config_t unit_config = {};
  unit_config.low_limit = -LIMIT;
  unit_config.high_limit = LIMIT;
  if (pcnt_new_unit(&unit_config, &this->unit_) != ESP_OK) {
    ESP_LOGE(TAG, "No free PCNT unit");
    this->mark_failed();
    return;
  }

  pcnt_glitch_filter_config_t filter = {};
  filter.max_glitch_ns = this->glitch_ns_;
  if (pcnt_unit_set_glitch_filter(this->unit_, &filter) != ESP_OK)
    ESP_LOGW(TAG, "Glitch filter not applied (%u ns)", (unsigned) this->glitch_ns_);

  // Full (x4) quadrature: channel A counts edges on A gated by B, channel B the reverse
  pcnt_chan_config_t a_cfg = {};
  a_cfg.edge_gpio_num = this->pin_a_;
  a_cfg.level_gpio_num = this->pin_b_;
  pcnt_channel_handle_t ch_a = nullptr;
  pcnt_chan_config_t b_cfg = {};
  b_cfg.edge_gpio_num = this->pin_b_;
  b_cfg.level_gpio_num = this->pin_a_;
  pcnt_channel_handle_t ch_b = nullptr;
  if (pcnt_new_channel(this->unit_, &a_cfg, &ch_a) != ESP_OK ||
      pcnt_new_channel(this->unit_, &b_cfg, &ch_b) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create PCNT channels");
    this->mark_failed();
    return;
  }
  pcnt_channel_set_edge_action(ch_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
  pcnt_channel_set_level_action(ch_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
  pcnt_channel_set_edge_action(ch_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
  pcnt_channel_set_level_action(ch_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

  pcnt_unit_enable(this->unit_);
  pcnt_unit_clear_count(this->unit_);
  pcnt_unit_start(this->unit_);
  this->last_count_ = 0;
}

void YotoEncoder::loop() {
  int count = 0;
  if (pcnt_unit_get_count(this->unit_, &count) != ESP_OK)
    return;
  const int delta = count - this->last_count_;
  this->last_count_ = count;
  if (std::abs(count) > RECENTER_AT) {
    pcnt_unit_clear_count(this->unit_);
    this->last_count_ = 0;
  }
  if (delta == 0)
    return;

  this->accum_ += this->invert_ ? -delta : delta;

  // One step per full detent; partial movement stays in the accumulator
  int fired = 0;
  while (this->accum_ >= this->counts_per_detent_) {
    this->accum_ -= this->counts_per_detent_;
    if (fired++ < this->max_steps_per_loop_)
      for (auto *t : this->cw_triggers_)
        t->trigger();
  }
  while (this->accum_ <= -this->counts_per_detent_) {
    this->accum_ += this->counts_per_detent_;
    if (fired++ < this->max_steps_per_loop_)
      for (auto *t : this->ccw_triggers_)
        t->trigger();
  }
}

void YotoEncoder::dump_config() {
  ESP_LOGCONFIG(TAG, "Rotary encoder (PCNT quadrature):");
  ESP_LOGCONFIG(TAG, "  Pins: A=GPIO%d B=GPIO%d%s", this->pin_a_, this->pin_b_, this->invert_ ? " (inverted)" : "");
  ESP_LOGCONFIG(TAG, "  Glitch filter: %u ns, counts per detent: %d, max steps per loop: %d",
                (unsigned) this->glitch_ns_, this->counts_per_detent_, this->max_steps_per_loop_);
}

}  // namespace yoto_encoder
}  // namespace esphome
