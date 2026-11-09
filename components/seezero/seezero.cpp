#include "seezero.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include <cmath>
#include <vector>

namespace esphome {
namespace seezero {

static const char *const TAG = "seezero";

// Zigbee Smart Energy clusters
static const uint16_t CL_PRICE = 0x0700;
static const uint16_t CL_METERING = 0x0702;
static const uint16_t CL_MESSAGING = 0x0703;
static const uint16_t CL_PREPAYMENT = 0x0705;

// Link message types
static const uint8_t TYPE_ATTRIBUTES = 0xB0;  // attribute values (readings, costs, history)
static const uint8_t TYPE_COMMAND = 0xD0;     // cluster commands (price, tariff, messages)

static const float COST_SCALE = 100000.0f;  // costs are in 1/100000 GBP

static uint16_t crc16_arc(const uint8_t *d, size_t n) {
  uint16_t c = 0;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++)
      c = (c & 1) ? (c >> 1) ^ 0xA001 : (c >> 1);
  }
  return c;
}

static uint64_t read_le(const uint8_t *p, uint16_t n) {
  if (n > 8) n = 8;
  uint64_t v = 0;
  for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

static int64_t read_le_signed(const uint8_t *p, uint16_t n) {
  if (n > 8) n = 8;
  uint64_t v = read_le(p, n);
  if (n < 8 && n > 0 && (v >> (8 * n - 1)) & 1) v |= ~0ULL << (8 * n);
  return (int64_t) v;
}

static float pow10i(int d) {
  float f = 1;
  for (int i = 0; i < d; i++) f *= 10;
  return f;
}

// A trailing-digit byte is either a plain count (0..9) or in the high nibble.
static int trailing_digits(uint8_t b) { return b <= 9 ? b : (b >> 4); }

static const uint32_t PERSIST_MAGIC = 0x5E2E0001;

void SeeZero::setup() {
  this->pref_ = global_preferences->make_preference<PersistState>(fnv1_hash("seezero_tariff_v1"));
  for (size_t i = 0; i < PERSIST_COUNT; i++) this->saved_.values[i] = NAN;
  PersistState loaded{};
  if (this->pref_.load(&loaded) && loaded.magic == PERSIST_MAGIC) {
    this->saved_ = loaded;
    ESP_LOGI(TAG, "Restored tariff / gas factors from flash");
    for (size_t i = 0; i < PERSIST_COUNT; i++) {
      float v = this->saved_.values[i];
      if (std::isnan(v)) continue;
      uint8_t key = PERSIST_KEYS[i];
      if (key == GAS_UNIT_RATE) this->gas_rate_ = v;
      if (key == GAS_CONVERSION) this->gas_cf_ = v;
      if (key == GAS_CALORIFIC) this->gas_cv_ = v;
      this->publish_(key, v, true);
    }
    this->update_gas_m3_price_();
  }
  this->saved_.magic = PERSIST_MAGIC;
}

void SeeZero::remember_(uint8_t key, float value) {
  for (size_t i = 0; i < PERSIST_COUNT; i++) {
    if (PERSIST_KEYS[i] != key) continue;
    if (this->saved_.values[i] == value) return;  // unchanged: no flash write
    this->saved_.values[i] = value;
    this->pref_.save(&this->saved_);
    ESP_LOGD(TAG, "Saved value %u to flash", key);
    return;
  }
}

void SeeZero::loop() {
  uint8_t c;
  int budget = 4096;  // don't hog the loop during the start-up burst
  while (budget-- > 0 && this->available()) {
    if (!this->read_byte(&c)) break;
    this->feed(c);
  }
  uint32_t now = millis();
  if (now - this->last_diag_ms_ > 60000) {
    this->last_diag_ms_ = now;
    this->publish_(FRAMES_OK, this->frames_ok_);
    this->publish_(CRC_ERRORS, this->crc_errors_);
  }
}

void SeeZero::feed(uint8_t c) {
  if (this->n_ == 0 && c != 0xAA) return;
  if (this->n_ == 1 && c != 0xAA) {
    this->n_ = 0;
    return;
  }
  this->buf_[this->n_++] = c;
  if (this->n_ < 5) return;

  uint16_t len = this->buf_[3] | (this->buf_[4] << 8);
  if (len > MAX_PAYLOAD) {
    this->resync_();
    return;
  }
  size_t total = 8 + (size_t) len;
  if (this->n_ < total) return;

  uint16_t want = (this->buf_[total - 2] << 8) | this->buf_[total - 1];
  if (crc16_arc(this->buf_ + 2, total - 4) != want) {
    this->crc_errors_++;
    ESP_LOGV(TAG, "CRC error (type %02X len %u)", this->buf_[2], len);
    this->resync_();
    return;
  }
  this->frames_ok_++;
  this->last_ok_ms_ = millis();
  this->n_ = 0;
  this->handle_frame_(this->buf_[2], this->buf_ + 6, len);
}

// Drop the first byte and look for the next AA AA header in what we already have.
void SeeZero::resync_() {
  size_t k = 1;
  while (k < this->n_ && !(this->buf_[k] == 0xAA && (k + 1 == this->n_ || this->buf_[k + 1] == 0xAA))) k++;
  std::vector<uint8_t> tail(this->buf_ + k, this->buf_ + this->n_);
  this->n_ = 0;
  for (uint8_t b : tail) this->feed(b);
}

void SeeZero::handle_frame_(uint8_t type, const uint8_t *p, uint16_t len) {
  if (type == TYPE_ATTRIBUTES) {
    // 00 | cluster (LE) | device | { attr (LE) | size (LE) | value[size] } ...
    if (len < 4) return;
    uint16_t cluster = p[1] | (p[2] << 8);
    uint8_t dev = p[3];
    uint16_t j = 4;
    while (j + 4 <= len) {
      uint16_t attr = p[j] | (p[j + 1] << 8);
      uint16_t n = p[j + 2] | (p[j + 3] << 8);
      if (j + 4 + n > len) break;
      this->handle_attr_(dev, cluster, attr, p + j + 4, n);
      j += 4 + n;
    }
  } else if (type == TYPE_COMMAND) {
    // cluster (LE) | command | device | size (LE) | body[size]
    if (len < 6) return;
    uint16_t cluster = p[0] | (p[1] << 8);
    uint16_t n = p[4] | (p[5] << 8);
    if (6 + n > len) return;
    this->handle_command_(cluster, p[2], p[3], p + 6, n);
  }
  // other types (01, 02, 7F, A1, A2, DA, E0) are link housekeeping; ignored
}

void SeeZero::handle_attr_(uint8_t dev, uint16_t cluster, uint16_t attr, const uint8_t *v, uint16_t n) {
  if (n == 0 || dev > 1) return;
  const bool gas = dev == 1;

  if (cluster == CL_METERING) {
    if (attr == 0x0301) {  // Multiplier
      uint32_t m = read_le(v, n);
      if (m) this->multiplier_[dev] = m;
      return;
    }
    if (attr == 0x0302) {  // Divisor
      uint32_t d = read_le(v, n);
      if (d) this->divisor_[dev] = d;
      return;
    }
    const float scale = (float) this->multiplier_[dev] / (float) this->divisor_[dev];
    if (attr == 0x0400) {  // InstantaneousDemand (signed, kW after scaling)
      if (!gas) this->publish_(ELEC_POWER, read_le_signed(v, n) * scale * 1000.0f, true);
      return;
    }
    const float val = (float) read_le(v, n) * scale;
    switch (attr) {
      case 0x0000: this->publish_(gas ? GAS_METER : ELEC_IMPORT, val); break;  // CurrentSummationDelivered
      case 0x0001: if (!gas) this->publish_(ELEC_EXPORT, val); break;          // CurrentSummationReceived
      case 0x0401: if (!gas) this->publish_(ELEC_TODAY, val); break;
      case 0x0403: if (!gas) this->publish_(ELEC_YESTERDAY, val); break;
      case 0x0430: if (!gas) this->publish_(ELEC_WEEK, val); break;
      case 0x0440: if (!gas) this->publish_(ELEC_MONTH, val); break;
      case 0x0C01: if (gas) this->publish_(GAS_TODAY, val); break;  // gas consumption in kWh
      case 0x0C03: if (gas) this->publish_(GAS_YESTERDAY, val); break;
      case 0x0C30: if (gas) this->publish_(GAS_WEEK, val); break;
      case 0x0C40: if (gas) this->publish_(GAS_MONTH, val); break;
      default: break;
    }
  } else if (cluster == CL_PREPAYMENT) {
    const float cost = (float) read_le(v, n) / COST_SCALE;
    switch (attr) {
      case 0x051C: this->publish_(gas ? GAS_COST_TODAY : ELEC_COST_TODAY, cost); break;
      case 0x051E: this->publish_(gas ? GAS_COST_YESTERDAY : ELEC_COST_YESTERDAY, cost); break;
      case 0x0530: this->publish_(gas ? GAS_COST_WEEK : ELEC_COST_WEEK, cost); break;
      case 0x0540: this->publish_(gas ? GAS_COST_MONTH : ELEC_COST_MONTH, cost); break;
      default: break;
    }
  } else if (cluster == CL_PRICE && gas) {
    switch (attr) {
      case 0x0301: this->publish_(GAS_STANDING, read_le(v, n) / COST_SCALE); break;  // StandingCharge
      case 0x0302: this->cf_raw_ = read_le(v, n); break;                              // ConversionFactor
      case 0x0303: this->cf_td_ = trailing_digits(v[0]); break;
      case 0x0304: this->cv_raw_ = read_le(v, n); break;                              // CalorificValue
      case 0x0306: this->cv_td_ = trailing_digits(v[0]); break;
      default: return;
    }
    if (this->cf_raw_ && this->cf_td_ >= 0) {
      this->gas_cf_ = this->cf_raw_ / pow10i(this->cf_td_);
      this->publish_(GAS_CONVERSION, this->gas_cf_);
    }
    if (this->cv_raw_ && this->cv_td_ >= 0) {
      this->gas_cv_ = this->cv_raw_ / pow10i(this->cv_td_);
      this->publish_(GAS_CALORIFIC, this->gas_cv_);
    }
    this->update_gas_m3_price_();
  }
}

void SeeZero::handle_command_(uint16_t cluster, uint8_t cmd, uint8_t dev, const uint8_t *s, uint16_t n) {
  if (cluster == CL_PRICE && dev <= 1) {
    if (cmd == 0x00 && n >= 88) {  // Publish Price: price @76 (uint32), trailing digits @87, GBP
      float rate = read_le(s + 76, 4) / pow10i(trailing_digits(s[87]));
      ESP_LOGD(TAG, "%s unit rate %.5f GBP/kWh", dev ? "Gas" : "Electricity", rate);
      if (dev) {
        this->gas_rate_ = rate;
        this->publish_(GAS_UNIT_RATE, rate);
        this->update_gas_m3_price_();
      } else {
        this->publish_(ELEC_UNIT_RATE, rate);
      }
    } else if (cmd == 0x04 && n >= 38) {  // Publish Tariff Information: standing charge @16, trailing digits @37
      float sc = read_le(s + 16, 4) / pow10i(trailing_digits(s[37]));
      ESP_LOGD(TAG, "%s standing charge %.5f GBP/day", dev ? "Gas" : "Electricity", sc);
      this->publish_(dev ? GAS_STANDING : ELEC_STANDING, sc);
    }
  } else if (cluster == CL_MESSAGING && cmd == 0x00 && n >= 12) {  // Display Message
    uint8_t tl = s[11];
    if (12 + tl > n) tl = n - 12;
    std::string text(reinterpret_cast<const char *>(s + 12), tl);
    if (text != this->last_message_) {
      this->last_message_ = text;
      ESP_LOGI(TAG, "Supplier message: %s", text.c_str());
      if (this->message_ != nullptr) this->message_->publish_state(text);
    }
  }
}

void SeeZero::update_gas_m3_price_() {
  if (std::isnan(this->gas_rate_) || std::isnan(this->gas_cf_) || std::isnan(this->gas_cv_)) return;
  // kWh per m3 = volume correction x calorific value (MJ/m3) / 3.6
  this->publish_(GAS_UNIT_RATE_M3, this->gas_rate_ * this->gas_cf_ * this->gas_cv_ / 3.6f);
}

void SeeZero::publish_(uint8_t key, float value, bool force) {
  if (!std::isnan(value)) this->remember_(key, value);
  sensor::Sensor *s = this->sensors_[key];
  if (s == nullptr) return;
  if (!force && s->has_state() && s->state == value) return;
  s->publish_state(value);
}

void SeeZero::dump_config() {
  ESP_LOGCONFIG(TAG, "geo SeeZero IHD tap:");
  int n = 0;
  for (auto *s : this->sensors_)
    if (s != nullptr) n++;
  ESP_LOGCONFIG(TAG, "  Sensors configured: %d", n);
}

}  // namespace seezero
}  // namespace esphome
