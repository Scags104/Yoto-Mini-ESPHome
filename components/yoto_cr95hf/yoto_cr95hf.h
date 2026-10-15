#pragma once

#include <string>
#include <vector>

#include "esphome/components/uart/uart.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"

namespace esphome {
namespace yoto_cr95hf {

class CR95HFTagTrigger : public Trigger<std::string> {};

// ST CR95HF NFC transceiver over UART (Yoto Mini rev #04: UART1, 57600 8-N-2).
// Reads ISO14443-A tag UIDs and fires on_tag / on_tag_removed with the UID
// formatted like ESPHome's PN532/RC522 readers ("04-A1-B2-C3-D4-E5-F6").
class CR95HFComponent : public PollingComponent, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  // Run after the IO expander has enabled the level converter.
  float get_setup_priority() const override { return setup_priority::LATE; }

  void add_on_tag_trigger(CR95HFTagTrigger *t) { this->tag_triggers_.push_back(t); }
  void add_on_tag_removed_trigger(CR95HFTagTrigger *t) { this->removed_triggers_.push_back(t); }

 protected:
  enum class State { WAKING, READY };
  enum class ReadResult { NO_TAG, TAG, ERROR };

  bool init_reader_();
  bool transceive_(uint8_t cmd, const std::vector<uint8_t> &data, uint8_t &result, std::vector<uint8_t> &resp,
                   uint32_t timeout_ms = 60);
  bool send_recv_(const std::vector<uint8_t> &frame, uint8_t control, uint8_t &result,
                  std::vector<uint8_t> &tag_bytes);
  ReadResult read_uid_(std::vector<uint8_t> &uid);
  bool read_byte_until_(uint8_t &b, uint32_t deadline);
  void flush_rx_();
  void restart_wake_(const char *reason);
  static std::string format_uid_(const std::vector<uint8_t> &uid);

  void fire_tag_(const std::string &uid);
  void fire_removed_(const std::string &uid);

  State state_{State::WAKING};
  uint32_t wake_attempts_{0};
  uint32_t last_wake_tx_{0};
  uint32_t wake_hold_until_{0};
  std::string idn_;
  std::string current_uid_;
  uint8_t miss_count_{0};
  uint8_t error_count_{0};
  std::vector<CR95HFTagTrigger *> tag_triggers_;
  std::vector<CR95HFTagTrigger *> removed_triggers_;
};

}  // namespace yoto_cr95hf
}  // namespace esphome
