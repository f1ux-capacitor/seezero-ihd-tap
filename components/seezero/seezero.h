#pragma once

#include <cmath>
#include <string>
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

namespace esphome {
namespace seezero {

// Order must match SENSOR_KEYS in sensor.py
enum SensorKey : uint8_t {
  ELEC_POWER = 0,
  ELEC_IMPORT,
  ELEC_EXPORT,
  ELEC_TODAY,
  ELEC_YESTERDAY,
  ELEC_WEEK,
  ELEC_MONTH,
  ELEC_COST_TODAY,
  ELEC_COST_YESTERDAY,
  ELEC_COST_WEEK,
  ELEC_COST_MONTH,
  ELEC_UNIT_RATE,
  ELEC_STANDING,
  GAS_METER,
  GAS_TODAY,
  GAS_YESTERDAY,
  GAS_WEEK,
  GAS_MONTH,
  GAS_COST_TODAY,
  GAS_COST_YESTERDAY,
  GAS_COST_WEEK,
  GAS_COST_MONTH,
  GAS_UNIT_RATE,
  GAS_UNIT_RATE_M3,
  GAS_STANDING,
  GAS_CALORIFIC,
  GAS_CONVERSION,
  FRAMES_OK,
  CRC_ERRORS,
  SENSOR_KEY_COUNT
};

// Frame on the SeeZero's internal Zigbee-radio -> host UART (J7 TXD, ~3.57 Mbaud 8N1):
//   AA AA | type | len (uint16 LE) | seq | payload[len] | CRC-16/ARC over type..payload (big-endian)
static const size_t MAX_PAYLOAD = 1024;

// Values the meter only sends when the IHD powers up. Kept in flash so a C3
// reboot (the IHD runs on, on its battery) doesn't lose them.
static const uint8_t PERSIST_KEYS[] = {ELEC_UNIT_RATE, ELEC_STANDING, GAS_UNIT_RATE,
                                       GAS_STANDING,   GAS_CALORIFIC, GAS_CONVERSION};
static const size_t PERSIST_COUNT = sizeof(PERSIST_KEYS);
struct PersistState {
  uint32_t magic;
  float values[PERSIST_COUNT];
};

class SeeZero : public Component, public uart::UARTDevice {
 public:
  void set_sensor(uint8_t key, sensor::Sensor *s) {
    if (key < SENSOR_KEY_COUNT) this->sensors_[key] = s;
  }
  void set_message_sensor(text_sensor::TextSensor *t) { this->message_ = t; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // for lambdas, e.g. a "no data" binary_sensor
  uint32_t last_frame_ms() const { return this->last_ok_ms_; }
  uint32_t frames_ok() const { return this->frames_ok_; }
  uint32_t crc_errors() const { return this->crc_errors_; }

  // exposed for host-side testing
  void feed(uint8_t c);

 protected:
  void handle_frame_(uint8_t type, const uint8_t *p, uint16_t len);
  void handle_attr_(uint8_t dev, uint16_t cluster, uint16_t attr, const uint8_t *v, uint16_t n);
  void handle_command_(uint16_t cluster, uint8_t cmd, uint8_t dev, const uint8_t *s, uint16_t n);
  void publish_(uint8_t key, float value, bool force = false);
  void update_gas_m3_price_();
  void resync_();
  void remember_(uint8_t key, float value);

  ESPPreferenceObject pref_;
  PersistState saved_{};

  sensor::Sensor *sensors_[SENSOR_KEY_COUNT]{};
  text_sensor::TextSensor *message_{nullptr};
  std::string last_message_;

  uint8_t buf_[MAX_PAYLOAD + 8];
  size_t n_{0};

  uint32_t frames_ok_{0};
  uint32_t crc_errors_{0};
  uint32_t last_ok_ms_{0};
  uint32_t last_diag_ms_{0};

  // metering formatting per device (0 = electricity, 1 = gas), learned from the meter
  uint32_t multiplier_[2]{1, 1};
  uint32_t divisor_[2]{1000, 1000};

  // gas conversion inputs
  float gas_rate_{NAN};
  float gas_cf_{NAN};
  float gas_cv_{NAN};
  uint32_t cf_raw_{0}, cv_raw_{0};
  int cf_td_{-1}, cv_td_{-1};
};

}  // namespace seezero
}  // namespace esphome
