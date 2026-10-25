#pragma once

#include <atomic>
#include <utility>
#include <cstdint>
#include <string>
#include <vector>

#include "esphome/components/sensor/sensor.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#include <esp_http_server.h>
#include <sdmmc_cmd.h>

namespace esphome {
namespace yoto_storage {

static constexpr const char *MOUNT_POINT = "/sdcard";

// One internet radio station from /stations.txt
struct Station {
  std::string genre;  // "" when the line has no genre
  std::string name;
  std::string url;
};

struct DirEntry {
  std::string name;
  bool is_dir;
  uint32_t size;
};

class PlayRequestTrigger : public Trigger<std::string> {};

// Yoto Mini built-in eMMC (SDMMC 4-bit) mounted as FAT at /sdcard, plus a small
// HTTP file server (browse / download / upload / delete / mkdir / play) on its
// own port. Files are streamed in chunks, so large audiobooks are fine.
class YotoStorage : public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_pins(int clk, int cmd, int d0, int d1, int d2, int d3) {
    clk_ = clk; cmd_ = cmd; d0_ = d0; d1_ = d1; d2_ = d2; d3_ = d3;
  }
  void set_frequency_khz(int f) { this->freq_khz_ = f; }
  void set_port(uint16_t p) { this->port_ = p; }
  void set_stream_port(uint16_t p) { this->stream_port_ = p; }
  void set_auth(const std::string &user, const std::string &pass);
  void set_total_sensor(sensor::Sensor *s) { this->total_sensor_ = s; }
  void set_free_sensor(sensor::Sensor *s) { this->free_sensor_ = s; }
  void add_on_play_request_trigger(PlayRequestTrigger *t) { this->play_triggers_.push_back(t); }

  bool is_mounted() const { return this->mounted_; }
  uint16_t get_port() const { return this->port_; }

  // Path helpers (paths are relative to the card root and start with '/')
  static bool sanitize(const std::string &rel, std::string &out);
  std::string full_path(const std::string &rel) const;  // "" if invalid
  bool is_dir(const std::string &rel) const;
  bool file_exists(const std::string &rel) const;
  // Lists at most max_entries items (folders like the stock Yoto "icons" cache
  // hold thousands of files; listing them all would exhaust the RAM).
  // *truncated is set when there were more.
  bool list_dir(const std::string &rel, std::vector<DirEntry> &out, size_t max_entries = 300,
                bool *truncated = nullptr) const;
  // Loopback URL the media source uses to stream a file
  std::string loopback_url(const std::string &rel) const;

  static std::string url_encode_path(const std::string &s);
  static std::string url_decode(const std::string &s);
  static bool is_audio_file(const std::string &name);
  // Ogg files can hold Opus (playable) or Vorbis (not supported by ESPHome).
  // Reads the first Ogg page: 1 = Opus, 0 = Vorbis/other, -1 = not Ogg / unreadable
  int ogg_is_opus(const std::string &rel) const;

  // Internet radio: reads /stations.txt, one station per line, # = comment:
  //   Genre | Name | URL     (grouped into a genre submenu)
  //   Name | URL             (no genre)
  bool load_stations(std::vector<Station> &out, size_t max_entries = 150) const;

  // Called (deferred to the main loop) when the web page asks to play a path
  void request_play(const std::string &rel);

  // Bytes sent so far by the current audio stream (reset when a new audio file
  // request starts). Used by the media source to estimate the resume position.
  uint32_t stream_bytes() const { return this->stream_bytes_.load(std::memory_order_relaxed); }

 protected:
  bool mount_();
  bool start_http_();

  static esp_err_t handle_root_(httpd_req_t *req);
  static esp_err_t handle_list_(httpd_req_t *req);
  static esp_err_t handle_file_get_(httpd_req_t *req);
  static esp_err_t handle_file_head_(httpd_req_t *req);
  static esp_err_t serve_file_(httpd_req_t *req, bool send_body);
  static esp_err_t handle_file_put_(httpd_req_t *req);
  static esp_err_t handle_file_delete_(httpd_req_t *req);
  static esp_err_t handle_mkdir_(httpd_req_t *req);
  static esp_err_t handle_clear_(httpd_req_t *req);
  static esp_err_t handle_play_(httpd_req_t *req);
  static esp_err_t handle_settings_(httpd_req_t *req);
  static esp_err_t handle_set_(httpd_req_t *req);
  // Main server: HTTP Basic login (if configured). Stream server: 127.0.0.1 only.
  // Sends the 401/403 response itself and returns false when access is denied.
  static bool authorize_(httpd_req_t *req);
  static bool query_path_(httpd_req_t *req, std::string &rel);
  static bool uri_path_(httpd_req_t *req, const char *prefix, std::string &rel);

  int clk_{14}, cmd_{15}, d0_{2}, d1_{4}, d2_{12}, d3_{13};
  int freq_khz_{20000};
  uint16_t port_{8080};
  uint16_t stream_port_{8081};
  httpd_handle_t stream_server_{nullptr};
  bool mounted_{false};
  sdmmc_card_t *card_{nullptr};
  httpd_handle_t server_{nullptr};

  std::atomic<uint32_t> stream_bytes_{0};
  std::string expected_auth_;  // "Basic <base64(user:pass)>", empty = no login

  sensor::Sensor *total_sensor_{nullptr};
  sensor::Sensor *free_sensor_{nullptr};
  std::vector<PlayRequestTrigger *> play_triggers_;
};

}  // namespace yoto_storage
}  // namespace esphome
