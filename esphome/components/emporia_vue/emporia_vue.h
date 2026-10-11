#pragma once

#ifdef USE_ESP32

#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"
#include "step_detector.h"

namespace esphome {
namespace emporia_vue {

struct __attribute__((__packed__)) ReadingPowerEntry {
  int32_t phase_black;
  int32_t phase_red;
  int32_t phase_blue;
};

struct __attribute__((__packed__)) SensorReading {
  bool is_unread;
  uint8_t checksum;
  uint8_t unknown;
  uint8_t sequence_num;

  ReadingPowerEntry power[19];

  uint16_t voltage[3];
  uint16_t frequency;
  uint16_t degrees[2];

  uint16_t current[19];

  uint16_t end;
};

class PhaseConfig;
class CTClampConfig;
class CircuitConfig;

// A sensor fed every frame that publishes either each frame or the mean over
// the component's publish_interval.
struct Averaged {
  sensor::Sensor *sensor{nullptr};
  double sum{0};
  uint32_t n{0};

  void add(float v) {
    if (this->sensor != nullptr && !std::isnan(v)) {
      this->sum += v;
      this->n++;
    }
  }
  void flush() {
    if (this->sensor != nullptr && this->n)
      this->sensor->publish_state(this->sum / this->n);
    this->sum = 0;
    this->n = 0;
  }
};

// Power factor over the same span: the summed real power over the summed apparent power.
struct AveragedPowerFactor {
  sensor::Sensor *sensor{nullptr};
  double watts{0}, va{0};

  void add(float w, float s) {
    if (this->sensor != nullptr && !std::isnan(w) && !std::isnan(s)) {
      this->watts += std::fabs(w);
      this->va += s;
    }
  }
  void flush() {
    if (this->sensor != nullptr && this->va > 1.0)
      this->sensor->publish_state(std::min(1.0, this->watts / this->va));
    this->watts = 0;
    this->va = 0;
  }
};

// The power factor of one frame, or NAN when there is too little current to tell.
inline float power_factor(float watts, float va) {
  if (std::isnan(watts) || std::isnan(va) || va < 5.0f)
    return NAN;
  return std::min(1.0f, std::fabs(watts) / va);
}

class EmporiaVueComponent : public PollingComponent, public i2c::I2CDevice {
 public:
  void dump_config() override;

  float get_setup_priority() const override { return esphome::setup_priority::HARDWARE; }

  void set_phases(std::vector<PhaseConfig *> phases) { this->phases_ = std::move(phases); }
  void set_ct_clamps(std::vector<CTClampConfig *> ct_clamps) { this->ct_clamps_ = std::move(ct_clamps); }
  void set_circuits(std::vector<CircuitConfig *> circuits) { this->circuits_ = std::move(circuits); }
  void set_publish_interval(uint32_t ms) { this->publish_interval_ms_ = ms; }
  void set_frame_rate_sensor(sensor::Sensor *s) { this->frame_rate_sensor_ = s; }
  void set_missed_frames_sensor(sensor::Sensor *s) { this->missed_frames_sensor_ = s; }

  void update() override;

  void add_on_update_callback(std::function<void()> &&callback);
  void add_on_load_event_callback(std::function<void(const LoadEvent &)> &&callback);
  void report_load_event(const LoadEvent &event);

 protected:
  void flush_();
  void publish_diagnostics_(uint32_t now);

  bool have_sequence_{false};
  uint8_t last_sequence_num_ = 0;
  std::vector<PhaseConfig *> phases_;
  std::vector<CTClampConfig *> ct_clamps_;
  std::vector<CircuitConfig *> circuits_;
  CallbackManager<void()> callback_;
  CallbackManager<void(const LoadEvent &)> load_event_callback_;

  uint32_t publish_interval_ms_{0};  // 0: every frame
  uint32_t last_flush_ms_{0};

  sensor::Sensor *frame_rate_sensor_{nullptr};
  sensor::Sensor *missed_frames_sensor_{nullptr};
  uint32_t frames_since_{0};
  uint32_t diagnostics_ms_{0};
  uint32_t missed_frames_{0};
};

enum PhaseInputWire : uint8_t {
  BLACK = 0,
  RED = 1,
  BLUE = 2,
};

class PhaseConfig {
 public:
  void set_input_wire(PhaseInputWire input_wire) { this->input_wire_ = input_wire; }
  PhaseInputWire get_input_wire() const { return this->input_wire_; }
  void set_calibration(float calibration) { this->calibration_ = calibration; }
  float get_calibration() const { return this->calibration_; }
  void set_voltage_sensor(sensor::Sensor *voltage_sensor) { this->voltage_.sensor = voltage_sensor; }
  sensor::Sensor *get_voltage_sensor() const { return this->voltage_.sensor; }
  void set_frequency_sensor(sensor::Sensor *frequency_sensor) { this->frequency_.sensor = frequency_sensor; }
  sensor::Sensor *get_frequency_sensor() const { return this->frequency_.sensor; }
  void set_phase_angle_sensor(sensor::Sensor *phase_angle_sensor) { this->phase_angle_.sensor = phase_angle_sensor; }
  sensor::Sensor *get_phase_angle_sensor() const { return this->phase_angle_.sensor; }

  void update_from_reading(const SensorReading &sensor_reading);
  void flush();

  int32_t extract_power_for_phase(const ReadingPowerEntry &power_entry);
  // The line voltage in the last frame.
  float get_voltage() const { return this->voltage_now_; }

 protected:
  PhaseInputWire input_wire_;
  float calibration_;
  float voltage_now_{NAN};
  Averaged voltage_;
  Averaged frequency_;
  Averaged phase_angle_;
};

enum CTInputPort : uint8_t {
#if defined(EMPORIA_VUE_VARIANT_VUE3)
  A = 2,
  B = 1,
  C = 0,
#else
  A = 0,
  B = 1,
  C = 2,
#endif
  ONE = 3,
  TWO = 4,
  THREE = 5,
  FOUR = 6,
  FIVE = 7,
  SIX = 8,
  SEVEN = 9,
  EIGHT = 10,
  NINE = 11,
  TEN = 12,
  ELEVEN = 13,
  TWELVE = 14,
  THIRTEEN = 15,
  FOURTEEN = 16,
  FIFTEEN = 17,
  SIXTEEN = 18,
};

class CTClampConfig : public sensor::Sensor {
 public:
  void set_phase(PhaseConfig *phase) { this->phase_ = phase; };
  const PhaseConfig *get_phase() const { return this->phase_; }
  void set_input_port(CTInputPort input_port) { this->input_port_ = input_port; };
  CTInputPort get_input_port() const { return this->input_port_; }
  void set_power_sensor(sensor::Sensor *power_sensor) { this->power_.sensor = power_sensor; }
  sensor::Sensor *get_power_sensor() const { return this->power_.sensor; }
  void set_current_sensor(sensor::Sensor *current_sensor) { this->current_.sensor = current_sensor; }
  sensor::Sensor *get_current_sensor() const { return this->current_.sensor; }
  void set_apparent_power_sensor(sensor::Sensor *s) { this->apparent_power_.sensor = s; }
  sensor::Sensor *get_apparent_power_sensor() const { return this->apparent_power_.sensor; }
  void set_power_factor_sensor(sensor::Sensor *s) { this->power_factor_.sensor = s; }
  sensor::Sensor *get_power_factor_sensor() const { return this->power_factor_.sensor; }
  void set_reverse(bool reverse) { this->reverse_ = reverse; }
  void set_load_events(const char *key, StepConfig cfg) { this->detector_ = new StepDetector(key, cfg); }  // NOLINT

  void update_from_reading(const SensorReading &sensor_reading);
  float get_calibrated_power(int32_t raw_power) const;
  // A frame's step, if this clamp watches for them and one just finished.
  const LoadEvent *detect(uint32_t now);
  void flush();

  // The last frame, for circuits made of several clamps.
  float get_power() const { return this->power_now_; }
  float get_current() const { return this->current_now_; }
  float get_apparent_power() const { return this->va_now_; }

 protected:
  PhaseConfig *phase_;
  CTInputPort input_port_;
  bool reverse_{false};
  float power_now_{NAN}, current_now_{NAN}, va_now_{NAN};
  Averaged power_;
  Averaged current_;
  Averaged apparent_power_;
  AveragedPowerFactor power_factor_;
  StepDetector *detector_{nullptr};
};

// A circuit measured by more than one clamp, such as a 240 V breaker with one
// clamp on each leg: its power is their sum, its current the larger leg's.
class CircuitConfig {
 public:
  void set_clamps(std::vector<CTClampConfig *> clamps) { this->clamps_ = std::move(clamps); }
  void set_power_sensor(sensor::Sensor *s) { this->power_.sensor = s; }
  void set_current_sensor(sensor::Sensor *s) { this->current_.sensor = s; }
  void set_apparent_power_sensor(sensor::Sensor *s) { this->apparent_power_.sensor = s; }
  void set_power_factor_sensor(sensor::Sensor *s) { this->power_factor_.sensor = s; }
  void set_load_events(const char *key, StepConfig cfg) { this->detector_ = new StepDetector(key, cfg); }  // NOLINT

  void update_from_clamps();
  const LoadEvent *detect(uint32_t now);
  void flush();

 protected:
  std::vector<CTClampConfig *> clamps_;
  float power_now_{NAN}, va_now_{NAN};
  Averaged power_;
  Averaged current_;
  Averaged apparent_power_;
  AveragedPowerFactor power_factor_;
  StepDetector *detector_{nullptr};
};

}  // namespace emporia_vue
}  // namespace esphome

#endif  // ifdef USE_ESP32
