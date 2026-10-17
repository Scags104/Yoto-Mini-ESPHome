#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/audio/audio.h"
#include "esphome/components/media_source/media_source.h"
#include "esphome/components/yoto_storage/yoto_storage.h"
#include "esphome/core/component.h"

#include <micro_decoder/decoder_source.h>
#include <micro_decoder/types.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace esphome {
namespace yoto_storage {

// Plays audio files from the Yoto's storage through the media player.
//   storage:///Books/Hobbit/01.mp3   one file
//   storage:///Books/Hobbit          every audio file in the folder, in name order
//                                    (next/previous move between files)
// Files are streamed from the device's own file server over 127.0.0.1 and
// decoded by ESPHome's micro-decoder (MP3, FLAC, Opus, WAV).
class StorageMediaSource final : public Component,
                                 public media_source::MediaSource,
                                 public micro_decoder::DecoderListener {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_storage(YotoStorage *s) { this->storage_ = s; }
  void set_handle_http(bool b) { this->handle_http_ = b; }
  void set_task_stack_in_psram(bool b) { this->stack_in_psram_ = b; }

  // MediaSource
  bool play_uri(const std::string &uri) override;
  void handle_command(media_source::MediaSourceCommand command) override;
  bool can_handle(const std::string &uri) const override;
  bool has_internal_playlist() const override { return true; }

  // DecoderListener
  size_t on_audio_write(const uint8_t *data, size_t length, uint32_t timeout_ms) override;
  void on_stream_info(const micro_decoder::AudioStreamInfo &info) override;
  void on_state_change(micro_decoder::DecoderState state) override;

  // Resume: save the current position now (e.g. before powering off)
  void save_position();
  // Does this folder have a saved position? (folder path relative to storage)
  bool has_resume(const std::string &folder) const;

  // Cover art for the current storage playback: loopback URL of cover.jpg /
  // folder.jpg / cover.png in the track's folder, or "" if none. The generation
  // counter changes whenever the URL changes.
  const std::string &cover_url() const { return this->cover_url_; }
  // Last storage:// URI played (without ?from_start), for "resume last thing"
  const std::string &last_uri() const { return this->last_uri_; }
  uint32_t cover_generation() const { return this->cover_gen_; }

  // Internet radio (radio://<name>|<url>): the station playing, if any
  bool is_radio() const { return this->is_http_ && !this->radio_name_.empty(); }

  // For the screen: "<file name>" of the current track, and position in the playlist
  std::string current_title() const;
  size_t track_number() const { return this->playlist_.empty() ? 0 : this->index_ + 1; }
  size_t track_count() const { return this->playlist_.size(); }

 protected:
  enum class Pending : uint8_t { NONE, ADVANCE, STOP };

  bool start_track_();
  bool is_ogg_vorbis_(const std::string &rel) const;
  void save_resume_();
  void clear_resume_();
  bool load_resume_(const std::string &folder, std::string &name, uint32_t &offset) const;
  void update_cover_(const std::string &folder);
  void finish_(media_source::MediaSourceState final_state);

  std::unique_ptr<micro_decoder::DecoderSource> decoder_;
  audio::AudioStreamInfo stream_info_;
  YotoStorage *storage_{nullptr};

  std::vector<std::string> playlist_;  // storage-relative paths, or full URLs when is_http_
  size_t index_{0};
  bool is_http_{false};
  Pending pending_{Pending::NONE};

  std::string resume_dir_;      // folder whose .resume file tracks this playback ("" = none)
  uint32_t track_offset_{0};    // byte offset the current track started from
  uint32_t last_save_ms_{0};

  std::string last_uri_;
  std::string radio_name_;
  std::string cover_url_;
  std::string cover_dir_;
  uint32_t cover_gen_{0};

  std::atomic<bool> pause_{false};
  bool handle_http_{true};
  bool stack_in_psram_{false};
};

}  // namespace yoto_storage
}  // namespace esphome

#endif  // USE_ESP32
