#include "yoto_aw88194.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace yoto_aw88194 {

static const char *const TAG = "yoto_aw88194";

static const uint8_t REG_ID = 0x00;
static const uint8_t REG_SYSST = 0x01;
static const uint8_t REG_SYSINT = 0x02;
static const uint8_t REG_SYSINTM = 0x03;
static const uint8_t REG_SYSCTRL = 0x04;
static const uint8_t REG_I2SCTRL = 0x05;
static const uint8_t REG_PWMCTRL = 0x08;
static const uint8_t REG_VOLUME = 0x0F;
static const uint8_t REG_SYSCTRL2 = 0x11;
static const uint8_t REG_DSP_ADDR = 0x40;
static const uint8_t REG_DSP_DATA = 0x41;
static const uint8_t REG_DSP_STATUS = 0x42;
static const uint8_t REG_PLLCTRL1 = 0x65;

static const uint16_t CHIP_ID = 0x1806;
static const int PLL_RETRIES = 5;
static const size_t DSP_CHUNK = 128;
static const uint16_t FW_BASE = 0x8C00;
static const uint16_t CFG_BASE = 0x8600;

struct RegCfg {
  uint8_t reg;
  uint16_t value;
};

// Factory table (Open Yoto: DRAM 0x3ffbee7a, 35 entries). Includes the rev #04
// mono mode and the I2S hardware parameters.
static const RegCfg FACTORY_REGS[] = {
    {0x03, 0xFFFF}, {0x05, 0x0C07}, {0x06, 0x0310}, {0x09, 0x6773}, {0x0A, 0x07ED}, {0x0B, 0x0F0F},
    {0x0C, 0x5F3C}, {0x0D, 0x001B}, {0x0E, 0x0A2D}, {0x0F, 0x1200}, {0x10, 0x3A10}, {0x11, 0x1BE5},
    {0x20, 0x0003}, {0x21, 0x2101}, {0x38, 0x0000}, {0x40, 0x87CE}, {0x41, 0x0000}, {0x43, 0x0000},
    {0x44, 0x0000}, {0x47, 0x00A0}, {0x60, 0x0A09}, {0x61, 0xC163}, {0x62, 0x2878}, {0x63, 0x2600},
    {0x64, 0x0588}, {0x65, 0x7F07}, {0x66, 0x0002}, {0x67, 0x0503}, {0x68, 0x0318}, {0x69, 0x2081},
    {0x6A, 0x1016}, {0x6B, 0x9CC4}, {0x6C, 0x12A2}, {0x04, 0x6440}, {0x08, 0x300E},
};

// ---------------------------------------------------------------------------
// I2C helpers: 8-bit register index, 16-bit big-endian values
// ---------------------------------------------------------------------------
bool AW88194Component::write16_(uint8_t reg, uint16_t value) {
  const uint8_t data[2] = {static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
  return this->write_register(reg, data, 2) == i2c::ERROR_OK;
}

bool AW88194Component::read16_(uint8_t reg, uint16_t *value) {
  uint8_t data[2] = {0, 0};
  if (this->read_register(reg, data, 2) != i2c::ERROR_OK)
    return false;
  *value = static_cast<uint16_t>((data[0] << 8) | data[1]);
  return true;
}

bool AW88194Component::update_bits_(uint8_t reg, uint16_t mask, uint16_t value) {
  uint16_t old;
  if (!this->read16_(reg, &old))
    return false;
  return this->write16_(reg, static_cast<uint16_t>((old & mask) | value));
}

// ---------------------------------------------------------------------------
// Setup: factory hardware reset (pactrl LOW 2 ms -> HIGH 2 ms) + identify
// ---------------------------------------------------------------------------
void AW88194Component::setup() {
  if (this->reset_switch_ == nullptr) {
    this->mark_failed();
    return;
  }
  this->reset_switch_->turn_off();
  delay(2);
  this->reset_switch_->turn_on();
  delay(2);

  this->identified_ = this->identify_();
  if (!this->identified_) {
    this->status_set_warning("AW88194 not identified");
    return;
  }
  // Keep the output powered down until the first cold start
  this->muted_ = true;
}

bool AW88194Component::identify_() {
  for (int attempt = 0; attempt < 5; attempt++) {
    if (this->read16_(REG_ID, &this->chip_id_) && this->chip_id_ == CHIP_ID) {
      ESP_LOGI(TAG, "AW88194 identified at 0x%02X (ID 0x%04X)", this->address_, this->chip_id_);
      return true;
    }
    delay(2);
  }
  ESP_LOGE(TAG, "AW88194 ID check failed (last ID 0x%04X)", this->chip_id_);
  return false;
}

// ---------------------------------------------------------------------------
// Cold start (needs I2S clocks)
// ---------------------------------------------------------------------------
bool AW88194Component::load_factory_regs_() {
  for (const auto &r : FACTORY_REGS) {
    if (!this->write16_(r.reg, r.value))
      return false;
  }
  return true;
}

bool AW88194Component::upload_dsp_(uint16_t base, const uint8_t *data, size_t len) {
  uint8_t tx[DSP_CHUNK];
  const uint32_t started = millis();
  if (!this->write16_(REG_DSP_ADDR, base))
    return false;
  delay(1);
  for (size_t offset = 0; offset < len;) {
    size_t chunk = len - offset;
    if (chunk > DSP_CHUNK)
      chunk = DSP_CHUNK;
    // Containers are stored little-endian per 16-bit word; the DSP data port
    // expects big-endian, so swap each byte pair (index ^ 1), as stock does.
    for (size_t i = 0; i < chunk; i++)
      tx[i] = progmem_read_byte(&data[(offset + i) ^ 1U]);
    if (this->write_register(REG_DSP_DATA, tx, chunk) != i2c::ERROR_OK)
      return false;
    offset += chunk;
    if (millis() - started > 2000) {
      ESP_LOGE(TAG, "DSP upload timed out");
      return false;
    }
  }
  return true;
}

bool AW88194Component::iis_check_() {
  for (int attempt = 0; attempt < PLL_RETRIES; attempt++) {
    uint16_t status;
    if (!this->read16_(REG_SYSST, &status))
      return false;
    if (status & 0x0001)
      return true;
    delay(2);
  }
  return false;
}

bool AW88194Component::syspll_check_() {
  if (this->iis_check_())
    return true;
  uint16_t pllctrl;
  if (!this->read16_(REG_PLLCTRL1, &pllctrl) || (pllctrl & 0x4000) == 0)
    return false;
  if (!this->update_bits_(REG_PLLCTRL1, 0xBFFF, 0x0000))
    return false;
  (void) this->iis_check_();
  if (!this->update_bits_(REG_PLLCTRL1, 0xBFFF, 0x4000))
    return false;
  delay(2);
  return this->iis_check_();
}

bool AW88194Component::program_vcalb_() {
  uint16_t raw_ical, raw_vcal;
  if (!this->read16_(0x78, &raw_ical) || !this->read16_(0x79, &raw_vcal))
    return false;
  int32_t cal0 = static_cast<int8_t>(raw_ical & 0xFF);
  int32_t cal1 = static_cast<int32_t>((raw_vcal >> 10) & 0x3F);
  if (cal1 & 0x20)
    cal1 -= 0x40;
  const int32_t icalk = cal0 + 500;
  const int32_t vcalk = cal1 * 4 + 1000;
  if (icalk == 0)
    return false;
  const uint16_t vcalb = static_cast<uint16_t>((vcalk * 0x3A37) / (2 * icalk));
  return this->write16_(REG_DSP_ADDR, 0x866D) && this->write16_(REG_DSP_DATA, vcalb);
}

#define AW_STEP(expr, what) \
  if (!(expr)) { \
    ESP_LOGE(TAG, "Cold start failed at: %s", what); \
    return false; \
  }

bool AW88194Component::cold_start_() {
  uint16_t status;

  AW_STEP(this->update_bits_(REG_SYSCTRL, 0xFFFE, 0x0000), "power up");
  delay(2);
  AW_STEP(this->load_factory_regs_(), "factory register table");
  AW_STEP(this->syspll_check_(), "I2S/PLL lock (are I2S clocks running?)");
  AW_STEP(this->update_bits_(REG_PWMCTRL, 0xFFFE, 0x0001), "PWM mute");
  AW_STEP(this->update_bits_(REG_SYSCTRL, 0xFFFB, 0x0004), "DSP bypass");
  AW_STEP(this->update_bits_(REG_SYSCTRL2, 0xEFFF, 0x1000), "SYSCTRL2");
  AW_STEP(this->upload_dsp_(FW_BASE, this->fw_, this->fw_len_), "DSP firmware upload");
  ESP_LOGD(TAG, "DSP firmware uploaded (%u bytes)", (unsigned) this->fw_len_);
  AW_STEP(this->upload_dsp_(CFG_BASE, this->cfg_, this->cfg_len_), "DSP config upload");
  ESP_LOGD(TAG, "DSP config uploaded (%u bytes)", (unsigned) this->cfg_len_);
  AW_STEP(this->program_vcalb_(), "VCALB");
  AW_STEP(this->update_bits_(REG_SYSCTRL, 0xFFFB, 0x0000), "DSP enable");
  AW_STEP(this->update_bits_(REG_SYSCTRL, 0xFFFD, 0x0000), "AMP enable");
  delay(2);
  AW_STEP(this->read16_(REG_DSP_STATUS, &status) && status != 0, "DSP status");
  ESP_LOGD(TAG, "DSP status 0x%04X", status);
  // Read-to-clear status/interrupts, then unmask the factory interrupt set
  AW_STEP(this->read16_(REG_SYSST, &status), "clear SYSST");
  AW_STEP(this->read16_(REG_SYSINT, &status), "clear SYSINT");
  AW_STEP(this->read16_(REG_SYSINTM, &status), "read SYSINTM");
  AW_STEP(this->update_bits_(REG_SYSINTM, 0xFFF4, 0x0000), "SYSINTM");
  AW_STEP(this->write16_(REG_I2SCTRL, 0x0C07), "I2SCTRL");
  AW_STEP(this->update_bits_(REG_SYSCTRL, 0xFFFB, 0x0000), "DSP enable (2)");
  AW_STEP(this->update_bits_(REG_SYSCTRL, 0xFFFE, 0x0000), "power up (2)");
  delay(2);
  AW_STEP(this->syspll_check_(), "I2S/PLL lock (2)");
  AW_STEP(this->update_bits_(REG_PWMCTRL, 0xFFFE, 0x0000), "PWM unmute");
  // Factory applies speaker volume 100% -> register 0x0F = 0 (no attenuation).
  // Overall volume is applied in software on the I2S stream.
  AW_STEP(this->write16_(REG_VOLUME, 0x0000), "volume");
  return true;
}

bool AW88194Component::validate_ready_() {
  uint16_t sysst, sysctrl, i2sctrl, pwmctrl, volume, dsp;
  if (!this->read16_(REG_SYSST, &sysst) || !this->read16_(REG_SYSCTRL, &sysctrl) ||
      !this->read16_(REG_I2SCTRL, &i2sctrl) || !this->read16_(REG_PWMCTRL, &pwmctrl) ||
      !this->read16_(REG_VOLUME, &volume) || !this->read16_(REG_DSP_STATUS, &dsp))
    return false;
  ESP_LOGI(TAG, "Ready state: SYSST=0x%04X SYSCTRL=0x%04X I2SCTRL=0x%04X PWMCTRL=0x%04X VOL=0x%04X DSP=0x%04X",
           sysst, sysctrl, i2sctrl, pwmctrl, volume, dsp);
  return (sysst & 0x0001) && (sysctrl & 0x0007) == 0 && i2sctrl == 0x0C07 && (pwmctrl & 0x0001) == 0 &&
         volume == 0 && dsp != 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool AW88194Component::ensure_running() {
  if (!this->identified_)
    return false;

  if (this->started_) {
    // Clocks may have stopped and restarted: confirm the PLL relocked
    if (!this->syspll_check_()) {
      ESP_LOGW(TAG, "PLL not locked; is I2S running?");
      return false;
    }
    return true;
  }

  const uint32_t t0 = millis();
  ESP_LOGI(TAG, "Cold-starting AW88194 (factory sequence)...");
  if (!this->cold_start_()) {
    this->status_set_warning("AW88194 cold start failed");
    return false;
  }
  if (!this->validate_ready_()) {
    ESP_LOGE(TAG, "AW88194 started but ready-state check failed");
    this->status_set_warning("AW88194 not ready");
    return false;
  }
  this->started_ = true;
  this->muted_ = false;
  this->status_clear_warning();
  ESP_LOGI(TAG, "AW88194 speaker amp ready (%u ms)", (unsigned) (millis() - t0));
  return true;
}

bool AW88194Component::set_muted(bool muted) {
  if (!this->started_)
    return false;
  if (muted == this->muted_)
    return true;
  // SYSCTRL bit 0 = run_pwd: set = speaker output powered down, DSP kept
  if (!this->update_bits_(REG_SYSCTRL, 0xFFFE, muted ? 0x0001 : 0x0000))
    return false;
  if (!muted)
    delay(2);
  this->muted_ = muted;
  ESP_LOGI(TAG, "Speaker %s", muted ? "muted" : "unmuted");
  return true;
}

void AW88194Component::dump_config() {
  ESP_LOGCONFIG(TAG, "AW88194 speaker amp:");
  LOG_I2C_DEVICE(this);
  ESP_LOGCONFIG(TAG, "  Identified: %s (ID 0x%04X)", YESNO(this->identified_), this->chip_id_);
  ESP_LOGCONFIG(TAG, "  Started: %s", YESNO(this->started_));
  ESP_LOGCONFIG(TAG, "  DSP firmware: %u bytes, config: %u bytes", (unsigned) this->fw_len_,
                (unsigned) this->cfg_len_);
}

}  // namespace yoto_aw88194
}  // namespace esphome
