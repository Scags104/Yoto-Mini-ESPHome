#include "storage_media_source.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_http_client.h>
#if __has_include(<esp_crt_bundle.h>)
#include <esp_crt_bundle.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace esphome {
namespace yoto_storage {

static const char *const TAG = "yoto_storage.source";

static constexpr char STORAGE_PREFIX[] = "storage://";
static constexpr char RADIO_PREFIX[] = "radio://";
static constexpr size_t STORAGE_PREFIX_LEN = sizeof(STORAGE_PREFIX) - 1;
static constexpr uint32_t AUDIO_WRITE_TIMEOUT_MS = 50;
static constexpr uint32_t PAUSE_POLL_DELAY_MS = 20;
static constexpr uint32_t RESUME_SAVE_INTERVAL_MS = 30000;
// Bytes the server has sent but the speaker hasn't played yet (decoder ring
// buffer + transfer buffers + a couple of seconds), subtracted when saving
static constexpr uint32_t RESUME_BACKOFF_BYTES = 96 * 1024;
static constexpr char RESUME_FILE[] = ".resume";

static std::string parent_of(const std::string &rel) {
  const size_t slash = rel.rfind('/');
  return slash == 0 || slash == std::string::npos ? std::string("/") : rel.substr(0, slash);
}
static std::string basename_of(const std::string &rel) { return rel.substr(rel.rfind('/') + 1); }
static bool is_mp3(const std::string &rel) {
  const size_t dot = rel.rfind('.');
  return dot != std::string::npos && strcasecmp(rel.c_str() + dot, ".mp3") == 0;
}

void StorageMediaSource::setup() {
  this->disable_loop();

  micro_decoder::DecoderConfig config;
  config.audio_write_timeout_ms = AUDIO_WRITE_TIMEOUT_MS;
  // Slightly above ESPHome's own sources (2): local playback competes with the
  // file server, display and Wi-Fi on the original ESP32
  config.decoder_priority = 4;
  config.reader_priority = 4;
  config.decoder_stack_size = 5120;
  config.decoder_stack_in_psram = this->stack_in_psram_;
  // Bigger cushion between reading the file and decoding (~4 s of 128 kbps audio)
  config.ring_buffer_size = 64 * 1024;
  config.transfer_buffer_size = 8192;
  config.http_rx_buffer_size = 4096;
  config.http_timeout_ms = 5000;

  this->decoder_ = std::make_unique<micro_decoder::DecoderSource>(config);
  if (this->decoder_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate decoder");
    this->mark_failed();
    return;
  }
  this->decoder_->set_listener(this);
}

void StorageMediaSource::loop() {
  this->decoder_->loop();
  if (this->get_state() == media_source::MediaSourceState::PLAYING && !this->resume_dir_.empty() &&
      millis() - this->last_save_ms_ > RESUME_SAVE_INTERVAL_MS)
    this->save_resume_();
}

void StorageMediaSource::dump_config() {
  ESP_LOGCONFIG(TAG, "Storage media source:");
  ESP_LOGCONFIG(TAG, "  URIs: storage://<path>%s", this->handle_http_ ? ", http(s)://" : "");
  ESP_LOGCONFIG(TAG, "  Decoder task stack in PSRAM: %s", YESNO(this->stack_in_psram_));
}

bool StorageMediaSource::can_handle(const std::string &uri) const {
  if (uri.starts_with(STORAGE_PREFIX) || uri.starts_with(RADIO_PREFIX))
    return true;
  return this->handle_http_ && (uri.starts_with("http://") || uri.starts_with("https://"));
}

// --- Internet radio helpers -------------------------------------------------
static std::string lower_path(const std::string &url) {
  std::string p = url.substr(0, url.find('?'));
  std::transform(p.begin(), p.end(), p.begin(), ::tolower);
  return p;
}

// Fetch a small .m3u / .pls playlist and return the first stream URL in it
// ("" if none). Follows up to 3 redirects. Blocks for at most ~3 s.
static std::string resolve_playlist(const std::string &playlist_url) {
  std::string url = playlist_url;
  for (int hop = 0; hop < 3; hop++) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 3000;
    cfg.buffer_size = 1024;
    cfg.disable_auto_redirect = true;
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && __has_include(<esp_crt_bundle.h>)
    cfg.crt_bundle_attach = esp_crt_bundle_attach;  // https playlists
#endif
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == nullptr)
      return "";
    if (esp_http_client_open(c, 0) != ESP_OK) {
      esp_http_client_cleanup(c);
      return "";
    }
    esp_http_client_fetch_headers(c);
    const int status = esp_http_client_get_status_code(c);
    if (status >= 300 && status < 400) {
      char *loc = nullptr;
      std::string next;
      if (esp_http_client_get_header(c, "Location", &loc) == ESP_OK && loc != nullptr)
        next = loc;
      esp_http_client_close(c);
      esp_http_client_cleanup(c);
      if (next.empty())
        return "";
      url = next;
      continue;
    }
    std::string body;
    char buf[512];
    int n;
    while (body.size() < 4096 && (n = esp_http_client_read(c, buf, sizeof(buf))) > 0)
      body.append(buf, n);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    // .pls: "File1=http://..."; .m3u: first line starting with http
    size_t pos = 0;
    while (pos < body.size()) {
      size_t end = body.find('\n', pos);
      if (end == std::string::npos)
        end = body.size();
      std::string line = body.substr(pos, end - pos);
      pos = end + 1;
      while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        line.pop_back();
      const size_t eq = line.find('=');
      if (line.size() > 4 && strncasecmp(line.c_str(), "file", 4) == 0 && eq != std::string::npos)
        line = line.substr(eq + 1);
      if (line.rfind("http://", 0) == 0 || line.rfind("https://", 0) == 0)
        return line;
    }
    return "";
  }
  return "";
}

bool StorageMediaSource::play_uri(const std::string &uri) {
  if (!this->is_ready() || this->is_failed() || !this->has_listener())
    return false;
  if (this->get_state() == media_source::MediaSourceState::ERROR)
    this->finish_(media_source::MediaSourceState::IDLE);  // recover from a failed previous play
  if (this->get_state() != media_source::MediaSourceState::IDLE) {
    ESP_LOGE(TAG, "Cannot play '%s': source is busy", uri.c_str());
    return false;
  }

  this->playlist_.clear();
  this->index_ = 0;
  this->pending_ = Pending::NONE;

  this->resume_dir_.clear();
  this->track_offset_ = 0;

  if (uri.starts_with(STORAGE_PREFIX)) {
    if (this->storage_ == nullptr || !this->storage_->is_mounted()) {
      ESP_LOGE(TAG, "Storage is not mounted");
      return false;
    }
    // storage://<path>?from_start ignores any saved position
    std::string path_part = uri.substr(STORAGE_PREFIX_LEN);
    bool from_start = false;
    const size_t q = path_part.find('?');
    if (q != std::string::npos) {
      from_start = path_part.find("from_start", q) != std::string::npos;
      path_part.resize(q);
    }
    this->last_uri_ = std::string(STORAGE_PREFIX) + path_part;
    std::string rel;
    if (!YotoStorage::sanitize(YotoStorage::url_decode(path_part), rel)) {
      ESP_LOGE(TAG, "Invalid path in '%s'", uri.c_str());
      return false;
    }
    if (this->storage_->is_dir(rel)) {
      std::vector<DirEntry> entries;
      this->storage_->list_dir(rel, entries, 500);
      for (const auto &e : entries) {
        if (e.is_dir || !YotoStorage::is_audio_file(e.name))
          continue;
        const std::string item = (rel == "/" ? "" : rel) + "/" + e.name;
        if (this->is_ogg_vorbis_(item)) {
          ESP_LOGW(TAG, "Skipping Ogg Vorbis file (not supported, convert to Opus or MP3): %s", item.c_str());
          continue;
        }
        this->playlist_.push_back(item);
      }
      if (this->playlist_.empty()) {
        ESP_LOGE(TAG, "No playable audio files in '%s'", rel.c_str());
        return false;
      }
      ESP_LOGI(TAG, "Folder '%s': %u track(s)", rel.c_str(), (unsigned) this->playlist_.size());
      this->resume_dir_ = rel;
    } else {
      if (!YotoStorage::is_audio_file(rel)) {
        ESP_LOGE(TAG, "Not a supported audio file (mp3/flac/opus/ogg/wav): '%s'", rel.c_str());
        return false;
      }
      if (this->is_ogg_vorbis_(rel)) {
        ESP_LOGE(TAG, "'%s' is Ogg Vorbis, which ESPHome can't decode. Convert it to Opus or MP3.", rel.c_str());
        return false;
      }
      this->playlist_.push_back(rel);
      // A single file doesn't track a position of its own, but if it is the
      // folder's saved track it continues from the saved point
    }
    this->is_http_ = false;
    const std::string folder = this->resume_dir_.empty() ? parent_of(rel) : this->resume_dir_;
    this->update_cover_(folder);

    // Resume: continue the saved track (and position, for MP3) unless asked not to
    std::string saved_name;
    uint32_t saved_offset = 0;
    if (from_start) {
      this->clear_resume_();
    } else if (this->load_resume_(folder, saved_name, saved_offset)) {
      for (size_t i = 0; i < this->playlist_.size(); i++) {
        if (basename_of(this->playlist_[i]) == saved_name) {
          this->index_ = i;
          this->track_offset_ = is_mp3(saved_name) ? saved_offset : 0;
          ESP_LOGI(TAG, "Resuming '%s' at %u KB", saved_name.c_str(), (unsigned) (this->track_offset_ / 1024));
          break;
        }
      }
    }
  } else {
    // radio://<station name>|<stream or playlist URL>, or a plain http(s) URL
    std::string url = uri;
    this->radio_name_.clear();
    if (uri.starts_with(RADIO_PREFIX)) {
      const std::string rest = uri.substr(sizeof(RADIO_PREFIX) - 1);
      const size_t bar = rest.find('|');
      if (bar == std::string::npos) {
        ESP_LOGE(TAG, "Bad radio URI: %s", uri.c_str());
        return false;
      }
      this->radio_name_ = rest.substr(0, bar);
      url = rest.substr(bar + 1);
    }
    const std::string lp = lower_path(url);
    if (lp.ends_with(".m3u8")) {
      ESP_LOGE(TAG, "HLS (.m3u8) streams aren't supported - use the station's MP3 stream URL");
      return false;
    }
    if (lp.ends_with(".m3u") || lp.ends_with(".pls")) {
      const std::string resolved = resolve_playlist(url);
      if (resolved.empty()) {
        ESP_LOGE(TAG, "No stream URL found in playlist %s", url.c_str());
        return false;
      }
      ESP_LOGI(TAG, "Playlist %s -> %s", url.c_str(), resolved.c_str());
      url = resolved;
    }
    if (!this->radio_name_.empty())
      ESP_LOGI(TAG, "Radio: %s", this->radio_name_.c_str());
    this->playlist_.push_back(url);
    this->is_http_ = true;
    this->update_cover_("");
  }

  return this->start_track_();
}

bool StorageMediaSource::start_track_() {
  if (this->index_ >= this->playlist_.size())
    return false;
  const std::string &item = this->playlist_[this->index_];
  std::string url = this->is_http_ ? item : this->storage_->loopback_url(item);
  if (!this->is_http_ && this->track_offset_ > 0)
    url += "?offset=" + std::to_string(this->track_offset_);
  ESP_LOGI(TAG, "Playing %u/%u: %s", (unsigned) (this->index_ + 1), (unsigned) this->playlist_.size(), item.c_str());
  if (this->decoder_->play_url(url)) {
    this->pause_.store(false, std::memory_order_relaxed);
    this->enable_loop();
    this->last_save_ms_ = millis();
    return true;
  }
  ESP_LOGE(TAG, "Failed to start '%s'", item.c_str());
  return false;
}

void StorageMediaSource::finish_(media_source::MediaSourceState final_state) {
  this->playlist_.clear();
  this->index_ = 0;
  this->pending_ = Pending::NONE;
  this->set_state_(final_state);
  if (final_state == media_source::MediaSourceState::IDLE)
    this->disable_loop();
}

void StorageMediaSource::handle_command(media_source::MediaSourceCommand command) {
  using media_source::MediaSourceCommand;
  using media_source::MediaSourceState;
  const MediaSourceState st = this->get_state();
  switch (command) {
    case MediaSourceCommand::STOP:
      if (st == MediaSourceState::PLAYING || st == MediaSourceState::PAUSED)
        this->save_resume_();
      this->pending_ = Pending::STOP;
      this->decoder_->stop();
      break;
    case MediaSourceCommand::PAUSE:
      if (st != MediaSourceState::PLAYING)
        break;
      this->set_state_(MediaSourceState::PAUSED);
      this->pause_.store(true, std::memory_order_relaxed);
      this->save_resume_();
      break;
    case MediaSourceCommand::PLAY:
      if (st != MediaSourceState::PAUSED)
        break;
      this->set_state_(MediaSourceState::PLAYING);
      this->pause_.store(false, std::memory_order_relaxed);
      break;
    case MediaSourceCommand::NEXT:
      if ((st == MediaSourceState::PLAYING || st == MediaSourceState::PAUSED) &&
          this->index_ + 1 < this->playlist_.size()) {
        this->index_++;
        this->track_offset_ = 0;
        this->pending_ = Pending::ADVANCE;
        this->pause_.store(false, std::memory_order_relaxed);
        this->decoder_->stop();
      }
      break;
    case MediaSourceCommand::PREVIOUS:
      if (st == MediaSourceState::PLAYING || st == MediaSourceState::PAUSED) {
        if (this->index_ > 0)
          this->index_--;  // otherwise restart the current track
        this->track_offset_ = 0;
        this->pending_ = Pending::ADVANCE;
        this->pause_.store(false, std::memory_order_relaxed);
        this->decoder_->stop();
      }
      break;
    default:
      break;
  }
}

// Decoder task -> orchestrator
size_t StorageMediaSource::on_audio_write(const uint8_t *data, size_t length, uint32_t timeout_ms) {
  if (this->pause_.load(std::memory_order_relaxed)) {
    vTaskDelay(pdMS_TO_TICKS(PAUSE_POLL_DELAY_MS));
    return 0;
  }
  return this->write_output(data, length, timeout_ms, this->stream_info_);
}

void StorageMediaSource::on_stream_info(const micro_decoder::AudioStreamInfo &info) {
  this->stream_info_ = audio::AudioStreamInfo(info.get_bits_per_sample(), info.get_channels(), info.get_sample_rate());
}

// Runs on the main loop (from decoder_->loop())
void StorageMediaSource::on_state_change(micro_decoder::DecoderState state) {
  using media_source::MediaSourceState;
  switch (state) {
    case micro_decoder::DecoderState::PLAYING:
      if (this->get_state() != MediaSourceState::PAUSED)
        this->set_state_(MediaSourceState::PLAYING);
      break;

    case micro_decoder::DecoderState::IDLE:
      if (this->pending_ == Pending::ADVANCE) {
        this->pending_ = Pending::NONE;
        if (!this->start_track_())
          this->finish_(MediaSourceState::IDLE);
        else
          this->save_resume_();
        return;
      }
      if (this->pending_ == Pending::STOP) {
        this->finish_(MediaSourceState::IDLE);
        return;
      }
      // Natural end of a track: continue through the folder
      if (this->index_ + 1 < this->playlist_.size()) {
        this->index_++;
        this->track_offset_ = 0;
        if (!this->start_track_())
          this->finish_(MediaSourceState::IDLE);
        else
          this->save_resume_();
        return;
      }
      // Finished the whole folder: next time starts from the beginning
      this->clear_resume_();
      this->finish_(MediaSourceState::IDLE);
      break;

    case micro_decoder::DecoderState::FAILED:
      ESP_LOGW(TAG, "Playback failed for track %u", (unsigned) (this->index_ + 1));
      if (this->pending_ != Pending::STOP && this->index_ + 1 < this->playlist_.size()) {
        this->index_++;
        if (this->start_track_())
          return;
      }
      this->finish_(MediaSourceState::ERROR);
      break;

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Resume position: <folder>/.resume holds "<file name>\n<byte offset>\n"
// ---------------------------------------------------------------------------
void StorageMediaSource::save_resume_() {
  this->last_save_ms_ = millis();
  if (this->is_http_ || this->resume_dir_.empty() || this->index_ >= this->playlist_.size())
    return;
  const std::string &item = this->playlist_[this->index_];
  uint32_t pos = this->track_offset_ + this->storage_->stream_bytes();
  pos = pos > RESUME_BACKOFF_BYTES ? pos - RESUME_BACKOFF_BYTES : 0;
  if (!is_mp3(item))
    pos = 0;  // only MP3 can start mid-file; other formats resume at the track start
  const std::string path = this->storage_->full_path(this->resume_dir_) + "/" + RESUME_FILE;
  FILE *f = fopen(path.c_str(), "w");
  if (f == nullptr)
    return;
  fprintf(f, "%s\n%u\n", basename_of(item).c_str(), (unsigned) pos);
  fclose(f);
  ESP_LOGD(TAG, "Saved position: %s @ %u KB", basename_of(item).c_str(), (unsigned) (pos / 1024));
}

void StorageMediaSource::clear_resume_() {
  if (this->resume_dir_.empty())
    return;
  const std::string path = this->storage_->full_path(this->resume_dir_) + "/" + RESUME_FILE;
  unlink(path.c_str());
}

bool StorageMediaSource::load_resume_(const std::string &folder, std::string &name, uint32_t &offset) const {
  if (folder.empty())
    return false;
  const std::string path = this->storage_->full_path(folder) + "/" + RESUME_FILE;
  FILE *f = fopen(path.c_str(), "r");
  if (f == nullptr)
    return false;
  char line[300];
  bool ok = false;
  if (fgets(line, sizeof(line), f) != nullptr) {
    name = line;
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
      name.pop_back();
    offset = 0;
    if (fgets(line, sizeof(line), f) != nullptr)
      offset = static_cast<uint32_t>(strtoul(line, nullptr, 10));
    ok = !name.empty();
  }
  fclose(f);
  return ok;
}

bool StorageMediaSource::has_resume(const std::string &folder) const {
  std::string clean;
  if (this->storage_ == nullptr || !YotoStorage::sanitize(folder, clean))
    return false;
  std::string name;
  uint32_t offset;
  return this->load_resume_(clean, name, offset);
}

void StorageMediaSource::save_position() {
  const auto st = this->get_state();
  if (st == media_source::MediaSourceState::PLAYING || st == media_source::MediaSourceState::PAUSED)
    this->save_resume_();
}

// Cover art: first of these names found in the folder
void StorageMediaSource::update_cover_(const std::string &folder) {
  // Checked on every play, so a cover added later is picked up next time
  this->cover_dir_ = folder;
  std::string url;
  if (!folder.empty() && this->storage_ != nullptr) {
    static const char *const NAMES[] = {"cover.jpg", "folder.jpg", "cover.jpeg", "cover.png", "folder.png"};
    for (const char *n : NAMES) {
      const std::string rel = (folder == "/" ? "" : folder) + "/" + n;
      struct stat st;
      const std::string full = this->storage_->full_path(rel);
      if (!full.empty() && stat(full.c_str(), &st) == 0 && !S_ISDIR(st.st_mode)) {
        url = this->storage_->loopback_url(rel);
        break;
      }
    }
  }
  if (url != this->cover_url_) {
    this->cover_url_ = url;
    this->cover_gen_++;
  }
}

bool StorageMediaSource::is_ogg_vorbis_(const std::string &rel) const {
  const size_t dot = rel.rfind('.');
  if (dot == std::string::npos || strcasecmp(rel.c_str() + dot, ".ogg") != 0)
    return false;
  return this->storage_->ogg_is_opus(rel) == 0;
}

std::string StorageMediaSource::current_title() const {
  if (this->playlist_.empty() || this->index_ >= this->playlist_.size())
    return "";
  const std::string &item = this->playlist_[this->index_];
  if (this->is_http_)
    return this->radio_name_;
  std::string name = item.substr(item.rfind('/') + 1);
  const size_t dot = name.rfind('.');
  if (dot != std::string::npos)
    name.resize(dot);
  return name;
}

}  // namespace yoto_storage
}  // namespace esphome

#endif  // USE_ESP32
