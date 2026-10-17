#include "yoto_storage.h"
#include "web_page.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cctype>
#include <strings.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <driver/sdmmc_host.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_vfs_fat.h>
#include <ff.h>

namespace esphome {
namespace yoto_storage {

static const char *const TAG = "yoto_storage";
static constexpr size_t IO_CHUNK = 4096;

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------
bool YotoStorage::sanitize(const std::string &rel, std::string &out) {
  std::string p = rel.empty() ? "/" : rel;
  if (p[0] != '/')
    p = "/" + p;
  // Reject any ".." segment and control characters
  size_t start = 0;
  while (start < p.size()) {
    size_t end = p.find('/', start);
    if (end == std::string::npos)
      end = p.size();
    const std::string seg = p.substr(start, end - start);
    if (seg == "..")
      return false;
    start = end + 1;
  }
  for (char c : p)
    if (static_cast<unsigned char>(c) < 0x20)
      return false;
  // Collapse a trailing slash (except root)
  while (p.size() > 1 && p.back() == '/')
    p.pop_back();
  out = p;
  return true;
}

std::string YotoStorage::full_path(const std::string &rel) const {
  std::string clean;
  if (!sanitize(rel, clean))
    return "";
  return clean == "/" ? std::string(MOUNT_POINT) : std::string(MOUNT_POINT) + clean;
}

bool YotoStorage::is_dir(const std::string &rel) const {
  const std::string full = this->full_path(rel);
  struct stat st;
  return !full.empty() && stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool YotoStorage::is_audio_file(const std::string &name) {
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos)
    return false;
  std::string ext = name.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  return ext == "mp3" || ext == "flac" || ext == "opus" || ext == "ogg" || ext == "wav";
}

bool YotoStorage::list_dir(const std::string &rel, std::vector<DirEntry> &out, size_t max_entries,
                           bool *truncated) const {
  out.clear();
  if (truncated != nullptr)
    *truncated = false;
  const std::string full = this->full_path(rel);
  if (full.empty())
    return false;
  DIR *d = opendir(full.c_str());
  if (d == nullptr)
    return false;
  struct dirent *e;
  while ((e = readdir(d)) != nullptr) {
    if (e->d_name[0] == '.')
      continue;  // ".", "..", our .resume files, macOS "._" junk
    if (out.size() >= max_entries) {
      if (truncated != nullptr)
        *truncated = true;
      break;
    }
    out.push_back(DirEntry{e->d_name, e->d_type == DT_DIR, 0});
  }
  closedir(d);
  // Sizes only for what we return (each stat walks the FAT directory)
  for (auto &de : out) {
    if (!de.is_dir) {
      struct stat st;
      const std::string fp = full + "/" + de.name;
      if (stat(fp.c_str(), &st) == 0)
        de.size = static_cast<uint32_t>(st.st_size);
    }
  }
  std::sort(out.begin(), out.end(), [](const DirEntry &a, const DirEntry &b) {
    if (a.is_dir != b.is_dir)
      return a.is_dir;
    return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  return true;
}

static std::string trim(const std::string &s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos)
    return "";
  const size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

bool YotoStorage::load_stations(std::vector<std::pair<std::string, std::string>> &out, size_t max_entries) const {
  out.clear();
  const std::string path = std::string(MOUNT_POINT) + "/stations.txt";
  FILE *f = fopen(path.c_str(), "r");
  if (f == nullptr)
    return false;
  char line[512];
  while (out.size() < max_entries && fgets(line, sizeof(line), f) != nullptr) {
    const std::string l = trim(line);
    if (l.empty() || l[0] == '#')
      continue;
    const size_t bar = l.find('|');
    if (bar == std::string::npos)
      continue;
    const std::string name = trim(l.substr(0, bar));
    const std::string url = trim(l.substr(bar + 1));
    if (!name.empty() && (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0))
      out.emplace_back(name, url);
  }
  fclose(f);
  return true;
}

std::string YotoStorage::url_encode_path(const std::string &s) {
  static const char *HEX = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += HEX[c >> 4];
      out += HEX[c & 0x0F];
    }
  }
  return out;
}

std::string YotoStorage::url_decode(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit(s[i + 1]) && isxdigit(s[i + 2])) {
      out += static_cast<char>(strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else if (s[i] == '+') {
      out += ' ';
    } else {
      out += s[i];
    }
  }
  return out;
}

int YotoStorage::ogg_is_opus(const std::string &rel) const {
  const std::string full = this->full_path(rel);
  if (full.empty())
    return -1;
  FILE *f = fopen(full.c_str(), "rb");
  if (f == nullptr)
    return -1;
  uint8_t head[96];
  const size_t n = fread(head, 1, sizeof(head), f);
  fclose(f);
  if (n < 36 || memcmp(head, "OggS", 4) != 0)
    return -1;
  for (size_t i = 0; i + 8 <= n; i++) {
    if (memcmp(head + i, "OpusHead", 8) == 0)
      return 1;
  }
  return 0;
}

std::string YotoStorage::loopback_url(const std::string &rel) const {
  // Audio files go through the dedicated streaming server; everything else
  // (cover art, ...) through the main file server
  const uint16_t port = is_audio_file(rel) && this->stream_server_ != nullptr ? this->stream_port_ : this->port_;
  return "http://127.0.0.1:" + std::to_string(port) + "/file" + url_encode_path(rel);
}

static const char *mime_for(const std::string &path) {
  const size_t dot = path.rfind('.');
  std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  if (ext == "mp3") return "audio/mpeg";
  if (ext == "flac") return "audio/flac";
  if (ext == "opus") return "audio/opus";
  if (ext == "ogg") return "audio/ogg";
  if (ext == "wav") return "audio/wav";
  if (ext == "json") return "application/json";
  if (ext == "txt") return "text/plain";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "png") return "image/png";
  return "application/octet-stream";
}

static bool mkdir_p(const std::string &full) {
  // full is an absolute VFS path under MOUNT_POINT
  std::string cur;
  size_t pos = strlen(MOUNT_POINT);
  cur = full.substr(0, pos);
  while (pos < full.size()) {
    size_t next = full.find('/', pos + 1);
    if (next == std::string::npos)
      next = full.size();
    cur = full.substr(0, next);
    struct stat st;
    if (stat(cur.c_str(), &st) != 0 && mkdir(cur.c_str(), 0775) != 0 && errno != EEXIST)
      return false;
    pos = next;
  }
  return true;
}

// Read up to `batch` child names of a folder (bounded memory for huge folders)
static bool read_batch(const std::string &full, std::vector<std::string> &names, size_t batch) {
  names.clear();
  DIR *d = opendir(full.c_str());
  if (d == nullptr)
    return false;
  struct dirent *e;
  while (names.size() < batch && (e = readdir(d)) != nullptr) {
    if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
      names.push_back(e->d_name);
  }
  closedir(d);
  return true;
}

static bool rm_rf(const std::string &full, unsigned *count, int depth = 0);

// Delete everything inside a folder in batches of 32 names, so a folder with
// thousands of files never needs more than a few KB of RAM.
static bool clear_dir(const std::string &full, unsigned *count, int depth = 0) {
  std::vector<std::string> names;
  names.reserve(32);
  bool ok = true;
  size_t failed_in_a_row = 0;
  while (read_batch(full, names, 32) && !names.empty()) {
    size_t deleted = 0;
    for (const auto &n : names) {
      if (rm_rf(full + "/" + n, count, depth + 1))
        deleted++;
      else
        ok = false;
    }
    if (deleted == 0 && ++failed_in_a_row >= 2)
      break;  // nothing deletable left in this batch: stop instead of looping
    if (deleted > 0)
      failed_in_a_row = 0;
    vTaskDelay(1);  // let Wi-Fi, audio and the watchdog breathe
  }
  return ok;
}

// Delete a file, or a folder and everything in it
static bool rm_rf(const std::string &full, unsigned *count, int depth) {
  struct stat st;
  if (stat(full.c_str(), &st) != 0)
    return true;  // already gone
  if (!S_ISDIR(st.st_mode)) {
    if (unlink(full.c_str()) != 0)
      return false;
    if (count != nullptr)
      (*count)++;
    return true;
  }
  if (depth > 16)
    return false;
  const bool ok = clear_dir(full, count, depth);
  if (!ok || rmdir(full.c_str()) != 0)
    return false;
  if (count != nullptr)
    (*count)++;
  return true;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
bool YotoStorage::mount_() {
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
  mount_config.format_if_mount_failed = false;  // never wipe the eMMC automatically
  mount_config.max_files = 6;
  mount_config.allocation_unit_size = 16 * 1024;

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = this->freq_khz_;

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 4;
#ifdef SOC_SDMMC_USE_GPIO_MATRIX
  slot.clk = static_cast<gpio_num_t>(this->clk_);
  slot.cmd = static_cast<gpio_num_t>(this->cmd_);
  slot.d0 = static_cast<gpio_num_t>(this->d0_);
  slot.d1 = static_cast<gpio_num_t>(this->d1_);
  slot.d2 = static_cast<gpio_num_t>(this->d2_);
  slot.d3 = static_cast<gpio_num_t>(this->d3_);
#else
  // Original ESP32: SDMMC slot 1 uses fixed pins CLK14 CMD15 D0 2 D1 4 D2 12 D3 13
  if (this->clk_ != 14 || this->cmd_ != 15 || this->d0_ != 2 || this->d1_ != 4 || this->d2_ != 12 || this->d3_ != 13)
    ESP_LOGW(TAG, "Original ESP32 SDMMC slot 1 has fixed pins (14/15/2/4/12/13); configured pins are ignored");
#endif
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  const esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mount_config, &this->card_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Mounting storage failed: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "Storage mounted at %s (%s, %llu MB)", MOUNT_POINT, this->card_->cid.name,
           static_cast<unsigned long long>(this->card_->csd.capacity) * this->card_->csd.sector_size / (1024 * 1024));
  return true;
}

void YotoStorage::setup() {
  this->mounted_ = this->mount_();
  if (!this->mounted_) {
    this->status_set_error(LOG_STR("Storage not mounted"));
    return;
  }
  if (!this->start_http_())
    this->status_set_warning("File server not started");
}

void YotoStorage::update() {
  if (!this->mounted_)
    return;
  FATFS *fs = nullptr;
  DWORD free_clusters = 0;
  if (f_getfree("0:", &free_clusters, &fs) != FR_OK || fs == nullptr)
    return;
#if FF_MAX_SS != FF_MIN_SS
  const uint64_t sector = fs->ssize;
#else
  const uint64_t sector = FF_MIN_SS;
#endif
  const uint64_t total = static_cast<uint64_t>(fs->n_fatent - 2) * fs->csize * sector;
  const uint64_t free_b = static_cast<uint64_t>(free_clusters) * fs->csize * sector;
  if (this->total_sensor_ != nullptr)
    this->total_sensor_->publish_state(total / (1024.0f * 1024.0f));
  if (this->free_sensor_ != nullptr)
    this->free_sensor_->publish_state(free_b / (1024.0f * 1024.0f));
}

void YotoStorage::dump_config() {
  ESP_LOGCONFIG(TAG, "Yoto storage (eMMC, SDMMC 4-bit):");
  ESP_LOGCONFIG(TAG, "  Mounted: %s at %s", YESNO(this->mounted_), MOUNT_POINT);
  ESP_LOGCONFIG(TAG, "  Bus frequency: %d kHz", this->freq_khz_);
  ESP_LOGCONFIG(TAG, "  File server: %s on port %u", YESNO(this->server_ != nullptr), this->port_);
  ESP_LOGCONFIG(TAG, "  Audio stream server: %s on port %u", YESNO(this->stream_server_ != nullptr), this->stream_port_);
  LOG_UPDATE_INTERVAL(this);
}

void YotoStorage::request_play(const std::string &rel) {
  // Runs in the HTTP server task: hand over to the main loop
  this->defer([this, rel]() {
    ESP_LOGI(TAG, "Play requested: %s", rel.c_str());
    for (auto *t : this->play_triggers_)
      t->trigger(rel);
  });
}

// ---------------------------------------------------------------------------
// HTTP server
// ---------------------------------------------------------------------------
bool YotoStorage::start_http_() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = this->port_;
  cfg.ctrl_port = 32768 + 77;  // must differ from other httpd instances (e.g. Sendspin)
  cfg.max_open_sockets = 3;
  cfg.lru_purge_enable = true;
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  cfg.stack_size = 6144;
  cfg.max_uri_handlers = 10;
  cfg.recv_wait_timeout = 15;
  cfg.send_wait_timeout = 15;

  if (httpd_start(&this->server_, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start file server");
    this->server_ = nullptr;
    return false;
  }

  const httpd_uri_t uris[] = {
      {.uri = "/", .method = HTTP_GET, .handler = handle_root_, .user_ctx = this},
      {.uri = "/api/list", .method = HTTP_GET, .handler = handle_list_, .user_ctx = this},
      {.uri = "/api/mkdir", .method = HTTP_POST, .handler = handle_mkdir_, .user_ctx = this},
      {.uri = "/api/play", .method = HTTP_POST, .handler = handle_play_, .user_ctx = this},
      {.uri = "/api/clear", .method = HTTP_POST, .handler = handle_clear_, .user_ctx = this},
      {.uri = "/file/*", .method = HTTP_GET, .handler = handle_file_get_, .user_ctx = this},
      {.uri = "/file/*", .method = HTTP_HEAD, .handler = handle_file_head_, .user_ctx = this},
      {.uri = "/file/*", .method = HTTP_PUT, .handler = handle_file_put_, .user_ctx = this},
      {.uri = "/file/*", .method = HTTP_DELETE, .handler = handle_file_delete_, .user_ctx = this},
  };
  for (const auto &u : uris)
    httpd_register_uri_handler(this->server_, &u);
  ESP_LOGI(TAG, "File server on port %u", this->port_);

  // Streaming server: read-only, used by the player's loopback audio streams
  httpd_config_t scfg = HTTPD_DEFAULT_CONFIG();
  scfg.server_port = this->stream_port_;
  scfg.ctrl_port = 32768 + 78;
  scfg.max_open_sockets = 2;
  scfg.lru_purge_enable = true;
  scfg.uri_match_fn = httpd_uri_match_wildcard;
  scfg.stack_size = 6144;
  scfg.max_uri_handlers = 2;
  scfg.recv_wait_timeout = 15;
  scfg.send_wait_timeout = 15;
  if (httpd_start(&this->stream_server_, &scfg) == ESP_OK) {
    const httpd_uri_t stream_uris[] = {
        {.uri = "/file/*", .method = HTTP_GET, .handler = handle_file_get_, .user_ctx = this},
        {.uri = "/file/*", .method = HTTP_HEAD, .handler = handle_file_head_, .user_ctx = this},
    };
    for (const auto &u : stream_uris)
      httpd_register_uri_handler(this->stream_server_, &u);
    ESP_LOGI(TAG, "Audio stream server on port %u", this->stream_port_);
  } else {
    this->stream_server_ = nullptr;
    ESP_LOGW(TAG, "Audio stream server failed to start; streaming via port %u", this->port_);
  }
  return true;
}

bool YotoStorage::uri_path_(httpd_req_t *req, const char *prefix, std::string &rel) {
  std::string uri(req->uri);
  const size_t q = uri.find('?');
  if (q != std::string::npos)
    uri.resize(q);
  const size_t plen = strlen(prefix);
  if (uri.compare(0, plen, prefix) != 0)
    return false;
  return sanitize(url_decode(uri.substr(plen)), rel);
}

bool YotoStorage::query_path_(httpd_req_t *req, std::string &rel) {
  const size_t len = httpd_req_get_url_query_len(req);
  if (len == 0)
    return sanitize("/", rel);
  std::string q(len + 1, '\0');
  if (httpd_req_get_url_query_str(req, &q[0], q.size()) != ESP_OK)
    return false;
  std::string val(q.size(), '\0');
  if (httpd_query_key_value(q.c_str(), "path", &val[0], val.size()) != ESP_OK)
    return sanitize("/", rel);
  val.resize(strlen(val.c_str()));
  return sanitize(url_decode(val), rel);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  std::string body = std::string("{\"error\":\"") + msg + "\"}";
  return httpd_resp_send(req, body.c_str(), body.size());
}

static std::string json_escape(const std::string &s) {
  std::string out;
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

esp_err_t YotoStorage::handle_root_(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, WEB_PAGE, HTTPD_RESP_USE_STRLEN);
}

esp_err_t YotoStorage::handle_list_(httpd_req_t *req) {
  auto *self = static_cast<YotoStorage *>(req->user_ctx);
  std::string rel;
  if (!query_path_(req, rel))
    return send_error(req, "400 Bad Request", "invalid path");
  std::vector<DirEntry> entries;
  bool truncated = false;
  if (!self->list_dir(rel, entries, 300, &truncated))
    return send_error(req, "404 Not Found", "not a directory");

  httpd_resp_set_type(req, "application/json");
  std::string chunk = "{\"path\":\"" + json_escape(rel) + "\",\"entries\":[";
  bool first = true;
  for (const auto &e : entries) {
    chunk += first ? "" : ",";
    first = false;
    chunk += "{\"name\":\"" + json_escape(e.name) + "\",\"dir\":" + (e.is_dir ? "true" : "false") +
             ",\"size\":" + std::to_string(e.size) + ",\"audio\":" +
             (!e.is_dir && is_audio_file(e.name) ? "true" : "false") + "}";
    if (chunk.size() > 1024) {
      if (httpd_resp_send_chunk(req, chunk.c_str(), chunk.size()) != ESP_OK)
        return ESP_FAIL;
      chunk.clear();
    }
  }
  chunk += std::string("],\"truncated\":") + (truncated ? "true" : "false") + "}";
  httpd_resp_send_chunk(req, chunk.c_str(), chunk.size());
  return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t YotoStorage::handle_file_get_(httpd_req_t *req) { return serve_file_(req, true); }
esp_err_t YotoStorage::handle_file_head_(httpd_req_t *req) { return serve_file_(req, false); }

// Send all bytes on the raw socket (httpd_send may send partially)
static bool send_all(httpd_req_t *req, const char *data, size_t len) {
  while (len > 0) {
    const int n = httpd_send(req, data, len);
    if (n == HTTPD_SOCK_ERR_TIMEOUT)
      continue;
    if (n <= 0)
      return false;
    data += n;
    len -= n;
  }
  return true;
}

// Files are sent with a Content-Length header (not chunked), so clients such as
// ESPHome's image downloader can size their buffers for the whole file.
esp_err_t YotoStorage::serve_file_(httpd_req_t *req, bool send_body) {
  auto *self = static_cast<YotoStorage *>(req->user_ctx);
  std::string rel;
  if (!uri_path_(req, "/file", rel))
    return send_error(req, "400 Bad Request", "invalid path");
  const std::string full = self->full_path(rel);
  struct stat st;
  if (full.empty() || stat(full.c_str(), &st) != 0 || S_ISDIR(st.st_mode))
    return send_error(req, "404 Not Found", "file not found");

  FILE *f = fopen(full.c_str(), "rb");
  if (f == nullptr)
    return send_error(req, "500 Internal Server Error", "cannot open file");

  // ?offset=N starts the stream N bytes into the file (resume support)
  long offset = 0;
  const size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen > 0) {
    std::string q(qlen + 1, '\0');
    char val[16] = {0};
    if (httpd_req_get_url_query_str(req, &q[0], q.size()) == ESP_OK &&
        httpd_query_key_value(q.c_str(), "offset", val, sizeof(val)) == ESP_OK)
      offset = strtol(val, nullptr, 10);
  }
  if (offset > 0 && offset < st.st_size)
    fseek(f, offset, SEEK_SET);

  if (offset < 0 || offset >= st.st_size)
    offset = 0;
  const long length = st.st_size - offset;

  // Tell the decoder which codec an .ogg holds (it treats plain audio/ogg as Vorbis)
  const char *mime = mime_for(rel);
  if (strcmp(mime, "audio/ogg") == 0 && self->ogg_is_opus(rel) == 1)
    mime = "audio/ogg; codecs=opus";

  char header[192];
  const int hlen = snprintf(header, sizeof(header),
                            "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                            "Accept-Ranges: none\r\nConnection: close\r\n\r\n",
                            mime, length);
  if (!send_all(req, header, hlen) || !send_body) {
    fclose(f);
    return ESP_OK;
  }

  const bool is_audio = is_audio_file(rel);
  if (is_audio)
    self->stream_bytes_.store(0, std::memory_order_relaxed);

  char *buf = static_cast<char *>(heap_caps_malloc(IO_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (buf == nullptr) {
    fclose(f);
    return ESP_FAIL;
  }
  esp_err_t result = ESP_OK;
  size_t n;
  while ((n = fread(buf, 1, IO_CHUNK, f)) > 0) {
    if (!send_all(req, buf, n)) {
      result = ESP_FAIL;  // client went away (e.g. playback stopped)
      break;
    }
    if (is_audio)
      self->stream_bytes_.fetch_add(n, std::memory_order_relaxed);
  }
  fclose(f);
  heap_caps_free(buf);
  return result;
}

esp_err_t YotoStorage::handle_file_put_(httpd_req_t *req) {
  auto *self = static_cast<YotoStorage *>(req->user_ctx);
  std::string rel;
  if (!uri_path_(req, "/file", rel) || rel == "/")
    return send_error(req, "400 Bad Request", "invalid path");
  const std::string full = self->full_path(rel);
  const std::string parent = full.substr(0, full.rfind('/'));
  if (!mkdir_p(parent))
    return send_error(req, "500 Internal Server Error", "cannot create folder");

  const std::string tmp = full + ".part";
  FILE *f = fopen(tmp.c_str(), "wb");
  if (f == nullptr)
    return send_error(req, "500 Internal Server Error", "cannot create file");
  char *buf = static_cast<char *>(heap_caps_malloc(IO_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (buf == nullptr) {
    fclose(f);
    unlink(tmp.c_str());
    return send_error(req, "500 Internal Server Error", "out of memory");
  }

  size_t remaining = req->content_len;
  bool ok = true;
  int timeouts = 0;
  while (remaining > 0) {
    const int r = httpd_req_recv(req, buf, std::min(remaining, IO_CHUNK));
    if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5)
      continue;
    if (r <= 0 || fwrite(buf, 1, r, f) != static_cast<size_t>(r)) {
      ok = false;
      break;
    }
    timeouts = 0;
    remaining -= r;
  }
  fclose(f);
  heap_caps_free(buf);
  if (!ok) {
    unlink(tmp.c_str());
    return send_error(req, "500 Internal Server Error", "upload failed");
  }
  unlink(full.c_str());  // replace an existing file
  if (rename(tmp.c_str(), full.c_str()) != 0) {
    unlink(tmp.c_str());
    return send_error(req, "500 Internal Server Error", "cannot finalise file");
  }
  ESP_LOGI(TAG, "Uploaded %s (%u bytes)", rel.c_str(), (unsigned) req->content_len);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t YotoStorage::handle_file_delete_(httpd_req_t *req) {
  auto *self = static_cast<YotoStorage *>(req->user_ctx);
  std::string rel;
  if (!uri_path_(req, "/file", rel) || rel == "/")
    return send_error(req, "400 Bad Request", "invalid path");
  const std::string full = self->full_path(rel);
  struct stat st;
  if (stat(full.c_str(), &st) != 0)
    return send_error(req, "404 Not Found", "not found");
  // ?recursive=1 deletes a folder together with everything in it
  bool recursive = false;
  const size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen > 0) {
    std::string q(qlen + 1, '\0');
    char val[8] = {0};
    if (httpd_req_get_url_query_str(req, &q[0], q.size()) == ESP_OK &&
        httpd_query_key_value(q.c_str(), "recursive", val, sizeof(val)) == ESP_OK)
      recursive = strcmp(val, "1") == 0 || strcmp(val, "true") == 0;
  }
  bool ok;
  if (S_ISDIR(st.st_mode))
    ok = recursive ? rm_rf(full, nullptr) : rmdir(full.c_str()) == 0;
  else
    ok = unlink(full.c_str()) == 0;
  if (!ok)
    return send_error(req, "409 Conflict", S_ISDIR(st.st_mode) && !recursive ? "folder not empty" : "delete failed");
  ESP_LOGI(TAG, "Deleted %s", rel.c_str());
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t YotoStorage::handle_mkdir_(httpd_req_t *req) {
  auto *self = static_cast<YotoStorage *>(req->user_ctx);
  std::string rel;
  if (!query_path_(req, rel) || rel == "/")
    return send_error(req, "400 Bad Request", "invalid path");
  if (!mkdir_p(self->full_path(rel)))
    return send_error(req, "500 Internal Server Error", "cannot create folder");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t YotoStorage::handle_clear_(httpd_req_t *req) {
  auto *self = static_cast<YotoStorage *>(req->user_ctx);
  std::string rel;
  if (!query_path_(req, rel) || !self->is_dir(rel))
    return send_error(req, "400 Bad Request", "not a folder");
  unsigned count = 0;
  const bool ok = clear_dir(self->full_path(rel), &count);
  ESP_LOGI(TAG, "Cleared %s (%u file(s)/folder(s) deleted)", rel.c_str(), count);
  httpd_resp_set_type(req, "application/json");
  std::string body = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"deleted\":" + std::to_string(count) + "}";
  return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t YotoStorage::handle_play_(httpd_req_t *req) {
  auto *self = static_cast<YotoStorage *>(req->user_ctx);
  std::string rel;
  if (!query_path_(req, rel))
    return send_error(req, "400 Bad Request", "invalid path");
  struct stat st;
  const std::string full = self->full_path(rel);
  if (full.empty() || stat(full.c_str(), &st) != 0)
    return send_error(req, "404 Not Found", "not found");
  self->request_play(rel);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

}  // namespace yoto_storage
}  // namespace esphome
