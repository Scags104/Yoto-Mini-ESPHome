#pragma once
// On-device menu for the Yoto Mini (ESPHome external component "yoto_menu").
// Open/close: both knob buttons together. Turn a knob: move. Left click: select.
// Right click: back (closes at the top level). Closes itself after 30 s idle.
//
// All device actions are hooks (std::function) bound from YAML in on_boot, so this
// header has no dependency on entity ids.

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "esphome/components/display/display.h"
#include "esphome/components/yoto_storage/yoto_storage.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_ota_ops.h>  // esp_ota_mark_app_valid_cancel_rollback() for the power-off script

namespace yoto_menu {

using esphome::Color;
using esphome::millis;
using esphome::display::BaseFont;
using esphome::display::Display;
using esphome::display::TextAlign;
using esphome::yoto_storage::DirEntry;
using esphome::yoto_storage::Station;

static constexpr uint32_t IDLE_CLOSE_MS = 30000;
static constexpr int ROWS = 6;
static constexpr int ROW_H = 30;
static constexpr int LIST_Y = 60;

struct Item {
  std::string label;
  std::string value;             // shown right-aligned, e.g. "On", "15 min", ">"
  std::function<void()> action;  // run on select (empty = not selectable)
};

enum class Page : uint8_t { ROOT, BLUETOOTH, STORAGE, RADIO, RADIO_GENRE, SLEEP, SETTINGS, INFO, POWER, CONFIRM };

class Menu {
 public:
  // ---- Hooks, bound from YAML ------------------------------------------------
  std::function<bool()> bt_enabled;
  std::function<void(bool)> set_bt;
  std::function<void()> bt_pair;
  std::function<bool()> away_mode;
  std::function<void(bool)> set_away;
  std::function<bool(const std::string &, std::vector<DirEntry> &)> list_dir;
  std::function<void(const std::string &)> play_storage;
  std::function<bool(const std::string &)> has_resume;  // folder has a saved position
  // Internet radio: stations from /stations.txt, and play one (name, url)
  std::function<bool(std::vector<Station> &)> list_stations;
  std::function<void(const std::string &, const std::string &)> play_radio;
  std::function<void()> stop_playback;
  std::function<float()> screen_timeout;
  std::function<void(float)> set_screen_timeout;
  std::function<float()> auto_off;
  std::function<void(float)> set_auto_off;
  std::function<float()> brightness;  // 0..1
  std::function<void(float)> set_brightness;
  std::function<std::vector<std::pair<std::string, std::string>>()> info;
  std::function<int()> theme;              // 0 Dark, 1 Light, 2 Night, 3 Colourful
  std::function<void(int)> set_theme;
  std::function<int()> clock_style;        // 0 Digital, 1 Analog, 2 Minimal
  std::function<void(int)> set_clock_style;
  std::function<bool()> night_auto;
  std::function<void(bool)> set_night_auto;

  // Colours used when drawing (set from the active theme before draw())
  void set_palette(Color fg, Color muted, Color highlight, Color dim) {
    this->fg_ = fg;
    this->muted_ = muted;
    this->hl_ = highlight;
    this->dim_ = dim;
  }
  std::function<void()> restart;
  std::function<void()> power_off;

  // ---- Navigation ---------------------------------------------------------------
  bool is_open() const { return this->open_; }

  void open() {
    this->open_ = true;
    this->page_ = Page::ROOT;
    this->sel_ = 0;
    this->top_ = 0;
    this->stack_.clear();
    this->touch_();
  }
  void close() { this->open_ = false; }
  void toggle() { this->open_ ? this->close() : this->open(); }

  void move(int delta) {
    if (!this->open_)
      return;
    const int n = static_cast<int>(this->build_().size());
    if (n == 0)
      return;
    this->sel_ = std::max(0, std::min(n - 1, this->sel_ + delta));
    this->clamp_scroll_(n);
    this->touch_();
  }

  void select() {
    if (!this->open_)
      return;
    this->touch_();
    auto items = this->build_();
    if (this->sel_ >= 0 && this->sel_ < static_cast<int>(items.size()) && items[this->sel_].action)
      items[this->sel_].action();
  }

  void back() {
    if (!this->open_)
      return;
    this->touch_();
    if (this->stack_.empty()) {
      this->close();
      return;
    }
    const Frame f = this->stack_.back();
    this->stack_.pop_back();
    this->page_ = f.page;
    this->path_ = f.path;
    this->sel_ = f.sel;
    this->top_ = f.top;
    if (this->page_ == Page::STORAGE)
      this->load_dir_();
  }

  // Call regularly (e.g. every 0.5 s): idle close + sleep timer
  void tick() {
    if (this->open_ && millis() - this->last_input_ > IDLE_CLOSE_MS)
      this->close();
    if (this->sleep_until_ != 0 && static_cast<int32_t>(millis() - this->sleep_until_) >= 0) {
      this->sleep_until_ = 0;
      ESP_LOGI("yoto_menu", "Sleep timer finished: stopping playback");
      if (this->stop_playback)
        this->stop_playback();
    }
  }

  uint32_t sleep_remaining_s() const {
    if (this->sleep_until_ == 0)
      return 0;
    const int32_t left = static_cast<int32_t>(this->sleep_until_ - millis());
    return left > 0 ? static_cast<uint32_t>(left) / 1000 : 0;
  }

  // ---- Drawing (below the 32 px status bar) -------------------------------------
  void draw(Display &it, BaseFont *item_font, BaseFont *small_font) {
    const Color WHITE = this->fg_, GREY = this->muted_, HL = this->hl_, DIM = this->dim_;
    auto items = this->build_();
    const int n = static_cast<int>(items.size());
    if (this->sel_ >= n)
      this->sel_ = std::max(0, n - 1);
    this->clamp_scroll_(n);

    it.print(120, 46, small_font, GREY, TextAlign::CENTER, this->title_().c_str());

    if (n == 0) {
      it.print(120, 130, item_font, GREY, TextAlign::CENTER, "Empty");
      return;
    }

    for (int i = this->top_; i < std::min(n, this->top_ + ROWS); i++) {
      const int y = LIST_Y + (i - this->top_) * ROW_H;
      const int cy = y + ROW_H / 2 - 1;
      const bool selected = i == this->sel_;
      const Item &item = items[i];
      if (selected)
        it.filled_rectangle(4, y, 228, ROW_H - 2, HL);

      int value_w = 0;
      if (!item.value.empty()) {
        int x1, y1, h;
        it.get_text_bounds(226, cy, item.value.c_str(), small_font, TextAlign::CENTER_RIGHT, &x1, &y1, &value_w, &h);
        it.print(226, cy, small_font, selected ? WHITE : GREY, TextAlign::CENTER_RIGHT, item.value.c_str());
      }
      const int max_w = 226 - 10 - value_w - (value_w ? 8 : 0);
      const std::string label = this->fit_(it, item_font, item.label, max_w, selected);
      it.print(10, cy, item_font, item.action || this->page_ == Page::INFO ? WHITE : GREY, TextAlign::CENTER_LEFT,
               label.c_str());
    }

    // Scroll bar for long lists
    if (n > ROWS) {
      const int track_h = ROWS * ROW_H - 4;
      const int bar_h = std::max(12, track_h * ROWS / n);
      const int bar_y = LIST_Y + (track_h - bar_h) * this->top_ / (n - ROWS);
      it.filled_rectangle(234, LIST_Y, 3, track_h, DIM);
      it.filled_rectangle(234, bar_y, 3, bar_h, WHITE);
    }
  }

 protected:
  struct Frame {
    Page page;
    std::string path;
    int sel;
    int top;
  };

  void touch_() { this->last_input_ = millis(); }

  void clamp_scroll_(int n) {
    if (this->sel_ < this->top_)
      this->top_ = this->sel_;
    if (this->sel_ >= this->top_ + ROWS)
      this->top_ = this->sel_ - ROWS + 1;
    this->top_ = std::max(0, std::min(this->top_, std::max(0, n - ROWS)));
  }

  void enter_(Page p, const std::string &path = "") {
    this->stack_.push_back({this->page_, this->path_, this->sel_, this->top_});
    this->page_ = p;
    this->path_ = path;
    this->sel_ = 0;
    this->top_ = 0;
    if (p == Page::STORAGE)
      this->load_dir_();
    if (p == Page::RADIO) {
      this->stations_.clear();
      if (this->list_stations)
        this->list_stations(this->stations_);
    }
  }

  void load_dir_() {
    this->entries_.clear();
    if (this->list_dir)
      this->list_dir(this->path_.empty() ? "/" : this->path_, this->entries_);
  }

  void confirm_(const std::string &title, const std::string &yes_label, std::function<void()> action) {
    this->confirm_title_ = title;
    this->confirm_yes_ = yes_label;
    this->confirm_action_ = std::move(action);
    this->enter_(Page::CONFIRM);
  }

  Item station_item_(const Station &st) {
    const std::string name = st.name, url = st.url;
    return {name, "", [this, name, url] {
              if (this->play_radio)
                this->play_radio(name, url);
              this->close();
            }};
  }

  static std::string join_(const std::string &dir, const std::string &name) {
    return (dir.empty() || dir == "/" ? "" : dir) + "/" + name;
  }

  static std::string on_off_(bool b) { return b ? "On" : "Off"; }

  static std::string minutes_(float m, const char *zero) {
    if (!(m > 0))
      return zero;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d min", static_cast<int>(m + 0.5f));
    return buf;
  }

  // Next value in a cycle list after the current one (wraps around)
  static float cycle_(float current, const std::vector<float> &values) {
    for (float v : values) {
      if (v > current + 0.001f)
        return v;
    }
    return values.front();
  }

  std::string title_() const {
    switch (this->page_) {
      case Page::ROOT:
        return "Menu";
      case Page::BLUETOOTH:
        return "Bluetooth";
      case Page::STORAGE: {
        if (this->path_.empty() || this->path_ == "/")
          return "Storage";
        return this->path_.substr(this->path_.rfind('/') + 1);
      }
      case Page::RADIO:
        return "Radio";
      case Page::RADIO_GENRE:
        return this->path_;
      case Page::SLEEP:
        return "Sleep timer";
      case Page::SETTINGS:
        return "Settings";
      case Page::INFO:
        return "Info";
      case Page::POWER:
        return "Power";
      case Page::CONFIRM:
        return this->confirm_title_;
    }
    return "";
  }

  // Fit text into max_w pixels: truncate with "...", or scroll when selected
  std::string fit_(Display &it, BaseFont *font, const std::string &s, int max_w, bool scroll) const {
    auto width = [&](const std::string &t) {
      int x1, y1, w, h;
      it.get_text_bounds(0, 0, t.c_str(), font, TextAlign::TOP_LEFT, &x1, &y1, &w, &h);
      return w;
    };
    if (max_w <= 0 || width(s) <= max_w)
      return s;
    std::string t;
    if (scroll) {
      const size_t off = (millis() / 300) % (s.size() + 4);
      const std::string loop = s + "    " + s;
      t = loop.substr(off);
    } else {
      t = s;
    }
    const char *ellipsis = scroll ? "" : "...";
    while (t.size() > 1 && width(t + ellipsis) > max_w)
      t.pop_back();
    return t + ellipsis;
  }

  std::vector<Item> build_() {
    std::vector<Item> v;
    switch (this->page_) {
      case Page::ROOT: {
        v.push_back({"Bluetooth", ">", [this] { this->enter_(Page::BLUETOOTH); }});
        v.push_back({"Storage", ">", [this] { this->enter_(Page::STORAGE, "/"); }});
        v.push_back({"Radio", ">", [this] { this->enter_(Page::RADIO); }});
        std::string sleep = "Off";
        if (const uint32_t s = this->sleep_remaining_s()) {
          char buf[16];
          snprintf(buf, sizeof(buf), "%u min", static_cast<unsigned>((s + 59) / 60));
          sleep = buf;
        }
        v.push_back({"Sleep timer", sleep, [this] { this->enter_(Page::SLEEP); }});
        v.push_back({"Settings", ">", [this] { this->enter_(Page::SETTINGS); }});
        v.push_back({"Info", ">", [this] { this->enter_(Page::INFO); }});
        v.push_back({"Power", ">", [this] { this->enter_(Page::POWER); }});
        break;
      }

      case Page::BLUETOOTH: {
        const bool on = this->bt_enabled && this->bt_enabled();
        v.push_back({"Bluetooth", on_off_(on), [this, on] {
                       if (this->set_bt)
                         this->set_bt(!on);
                     }});
        v.push_back({"Pair new device", "", [this] {
                       if (this->bt_pair)
                         this->bt_pair();
                       this->close();  // show the pairing screen
                     }});
        const bool away = this->away_mode && this->away_mode();
        v.push_back({"Bluetooth when away", on_off_(away), [this, away] {
                       if (this->set_away)
                         this->set_away(!away);
                     }});
        break;
      }

      case Page::STORAGE: {
        const std::string dir = this->path_.empty() ? "/" : this->path_;
        bool has_audio = false;
        for (const auto &e : this->entries_)
          has_audio |= !e.is_dir && esphome::yoto_storage::YotoStorage::is_audio_file(e.name);
        if (has_audio) {
          const bool resume = this->has_resume && this->has_resume(dir);
          v.push_back({resume ? "> Resume" : "> Play this folder", "", [this, dir] {
                         if (this->play_storage)
                           this->play_storage(dir);
                         this->close();
                       }});
          if (resume) {
            v.push_back({"> Play from start", "", [this, dir] {
                           if (this->play_storage)
                             this->play_storage(dir + "?from_start");
                           this->close();
                         }});
          }
        }
        for (const auto &e : this->entries_) {
          const std::string full = join_(dir, e.name);
          if (e.is_dir) {
            v.push_back({e.name, ">", [this, full] { this->enter_(Page::STORAGE, full); }});
          } else if (esphome::yoto_storage::YotoStorage::is_audio_file(e.name)) {
            v.push_back({e.name, "", [this, full] {
                           if (this->play_storage)
                             this->play_storage(full);
                           this->close();
                         }});
          } else {
            v.push_back({e.name, "", nullptr});  // shown, not playable
          }
        }
        break;
      }

      case Page::RADIO: {
        if (this->stations_.empty()) {
          v.push_back({"No stations.txt", "", nullptr});
          v.push_back({"Add one in the file manager", "", nullptr});
          break;
        }
        // Genres (in file order) as submenus; stations without a genre listed after them
        std::vector<std::string> genres;
        for (const auto &st : this->stations_) {
          if (!st.genre.empty() && std::find(genres.begin(), genres.end(), st.genre) == genres.end())
            genres.push_back(st.genre);
        }
        for (const auto &g : genres)
          v.push_back({g, ">", [this, g] { this->enter_(Page::RADIO_GENRE, g); }});
        for (const auto &st : this->stations_) {
          if (st.genre.empty())
            v.push_back(this->station_item_(st));
        }
        break;
      }

      case Page::RADIO_GENRE: {
        for (const auto &st : this->stations_) {
          if (st.genre == this->path_)
            v.push_back(this->station_item_(st));
        }
        break;
      }

      case Page::SLEEP: {
        const uint32_t remaining = this->sleep_remaining_s();
        for (int m : {0, 15, 30, 45, 60}) {
          std::string label = m == 0 ? std::string("Off") : std::to_string(m) + " min";
          const bool current = (m == 0 && remaining == 0);
          v.push_back({label, current ? "*" : "", [this, m] {
                         this->sleep_until_ = m == 0 ? 0 : (millis() + static_cast<uint32_t>(m) * 60000u) | 1u;
                         ESP_LOGI("yoto_menu", "Sleep timer: %d min", m);
                         this->back();
                       }});
        }
        break;
      }

      case Page::SETTINGS: {
        const float st = this->screen_timeout ? this->screen_timeout() : 0;
        v.push_back({"Screen timeout", minutes_(st, "Never"), [this, st] {
                       if (this->set_screen_timeout)
                         this->set_screen_timeout(cycle_(st, {0, 1, 2, 5, 10}));
                     }});
        const float ao = this->auto_off ? this->auto_off() : 0;
        v.push_back({"Auto power-off", minutes_(ao, "Never"), [this, ao] {
                       if (this->set_auto_off)
                         this->set_auto_off(cycle_(ao, {0, 5, 10, 15, 30, 60}));
                     }});
        const float br = this->brightness ? this->brightness() : 1.0f;
        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(br * 100.0f + 0.5f));
        v.push_back({"Brightness", pct, [this, br] {
                       if (this->set_brightness)
                         this->set_brightness(cycle_(br, {0.25f, 0.5f, 0.75f, 1.0f}));
                     }});
        static const char *const THEMES[] = {"Dark", "Light", "Night", "Colourful"};
        const int th = this->theme ? this->theme() : 0;
        v.push_back({"Theme", THEMES[(th >= 0 && th < 4) ? th : 0], [this, th] {
                       if (this->set_theme)
                         this->set_theme((th + 1) % 4);
                     }});
        static const char *const CLOCKS[] = {"Digital", "Analog", "Minimal"};
        const int cs = this->clock_style ? this->clock_style() : 0;
        v.push_back({"Clock style", CLOCKS[(cs >= 0 && cs < 3) ? cs : 0], [this, cs] {
                       if (this->set_clock_style)
                         this->set_clock_style((cs + 1) % 3);
                     }});
        const bool na = this->night_auto && this->night_auto();
        v.push_back({"Night mode", na ? "Auto" : "Off", [this, na] {
                       if (this->set_night_auto)
                         this->set_night_auto(!na);
                     }});
        break;
      }

      case Page::INFO: {
        if (this->info) {
          for (auto &kv : this->info())
            v.push_back({kv.first, kv.second, nullptr});
        }
        if (const uint32_t s = this->sleep_remaining_s()) {
          char buf[16];
          snprintf(buf, sizeof(buf), "%u:%02u", static_cast<unsigned>(s / 60), static_cast<unsigned>(s % 60));
          v.push_back({"Sleep timer", buf, nullptr});
        }
        break;
      }

      case Page::POWER: {
        v.push_back({"Restart", "", [this] {
                       this->confirm_("Restart?", "Yes, restart", [this] {
                         if (this->restart)
                           this->restart();
                       });
                     }});
        v.push_back({"Power off", "", [this] {
                       this->confirm_("Power off?", "Yes, power off", [this] {
                         this->close();
                         if (this->power_off)
                           this->power_off();
                       });
                     }});
        break;
      }

      case Page::CONFIRM: {
        v.push_back({"Cancel", "", [this] { this->back(); }});
        v.push_back({this->confirm_yes_, "", [this] {
                       auto action = this->confirm_action_;
                       if (action)
                         action();
                     }});
        break;
      }
    }
    return v;
  }

  bool open_{false};
  Page page_{Page::ROOT};
  std::string path_;
  int sel_{0};
  int top_{0};
  std::vector<Frame> stack_;
  std::vector<DirEntry> entries_;
  std::vector<Station> stations_;
  uint32_t last_input_{0};
  uint32_t sleep_until_{0};

  Color fg_{Color(255, 255, 255)};
  Color muted_{Color(150, 150, 150)};
  Color hl_{Color(30, 90, 170)};
  Color dim_{Color(70, 70, 70)};

  std::string confirm_title_, confirm_yes_;
  std::function<void()> confirm_action_;
};

inline Menu menu;  // single global instance used from YAML lambdas

}  // namespace yoto_menu
