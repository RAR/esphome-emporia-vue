#include "emporia_vue.h"

#include <cstring>
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace emporia_vue {

static const char *const TAG = "emporia_vue";

#if defined(EMPORIA_VUE_VARIANT_VUE3)
static constexpr float EMPORIA_VUE_FREQUENCY_CONSTANT = 19610.0f;
#else
static constexpr float EMPORIA_VUE_FREQUENCY_CONSTANT = 25310.0f;
#endif

void EmporiaVueComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Emporia Vue");
#if defined(EMPORIA_VUE_VARIANT_VUE3)
  ESP_LOGCONFIG(TAG, "  Variant: Vue 3");
#else
  ESP_LOGCONFIG(TAG, "  Variant: Vue 2");
#endif
  LOG_I2C_DEVICE(this);

  for (auto *phase : this->phases_) {
    std::string wire;
    switch (phase->get_input_wire()) {
      case PhaseInputWire::BLACK:
        wire = "BLACK";
        break;
      case PhaseInputWire::RED:
        wire = "RED";
        break;
      case PhaseInputWire::BLUE:
        wire = "BLUE";
        break;
    }
    ESP_LOGCONFIG(TAG, "  Phase");
    ESP_LOGCONFIG(TAG, "    Wire: %s", wire.c_str());
    ESP_LOGCONFIG(TAG, "    Calibration: %f", phase->get_calibration());
    LOG_SENSOR("    ", "Voltage", phase->get_voltage_sensor());
  }

  for (auto *ct_clamp : this->ct_clamps_) {
    ESP_LOGCONFIG(TAG, "  CT Clamp");
    ESP_LOGCONFIG(TAG, "    Phase Calibration: %f", ct_clamp->get_phase()->get_calibration());
    ESP_LOGCONFIG(TAG, "    CT Port Index: %d", ct_clamp->get_input_port());
    LOG_SENSOR("    ", "Power", ct_clamp->get_power_sensor());
    LOG_SENSOR("    ", "Current", ct_clamp->get_current_sensor());
    LOG_SENSOR("    ", "Apparent Power", ct_clamp->get_apparent_power_sensor());
    LOG_SENSOR("    ", "Power Factor", ct_clamp->get_power_factor_sensor());
  }
  ESP_LOGCONFIG(TAG, "  Circuits: %u", (unsigned) this->circuits_.size());
  ESP_LOGCONFIG(TAG, "  Publish interval: %u ms", (unsigned) this->publish_interval_ms_);
  LOG_SENSOR("  ", "Frame Rate", this->frame_rate_sensor_);
  LOG_SENSOR("  ", "Missed Frames", this->missed_frames_sensor_);
}

void EmporiaVueComponent::update() {
  SensorReading sensor_reading;

  i2c::ErrorCode err = read(reinterpret_cast<uint8_t *>(&sensor_reading), sizeof(sensor_reading));

  if (err != i2c::ErrorCode::ERROR_OK) {
    ESP_LOGE(TAG, "Failed to read from sensor due to I2C error %d", err);
    return;
  }

  if (sensor_reading.end != 0) {
    ESP_LOGE(TAG, "Failed to read from sensor due to a malformed reading, should end in null bytes but is %d",
             sensor_reading.end);
    return;
  }

  if (!sensor_reading.is_unread) {
    ESP_LOGV(TAG, "Ignoring sensor reading that is marked as read");
    return;
  }

  ESP_LOGV(TAG, "Received sensor reading with sequence #%d", sensor_reading.sequence_num);

  if (this->have_sequence_) {
    // The sequence number is one byte, so count across its wrap.
    uint8_t missed = static_cast<uint8_t>(sensor_reading.sequence_num - this->last_sequence_num_ - 1);
    if (missed > 16) {
      // Too far to be a few dropped frames: the meter chip restarted its count.
      ESP_LOGW(TAG, "Reading sequence jumped from #%d to #%d; the meter chip may have restarted",
               this->last_sequence_num_, sensor_reading.sequence_num);
    } else if (missed != 0) {
      ESP_LOGW(TAG, "Detected %d missing reading(s), data may not be accurate!", missed);
      this->missed_frames_ += missed;
    }
  }
  this->have_sequence_ = true;
  this->last_sequence_num_ = sensor_reading.sequence_num;

  const uint32_t now = millis();
  for (auto *phase : this->phases_) {
    phase->update_from_reading(sensor_reading);
  }
  for (auto *ct_clamp : this->ct_clamps_) {
    ct_clamp->update_from_reading(sensor_reading);
  }
  for (auto *circuit : this->circuits_) {
    circuit->update_from_clamps();
  }
  for (auto *ct_clamp : this->ct_clamps_) {
    if (const LoadEvent *event = ct_clamp->detect(now)) {
      this->report_load_event(*event);
      while ((event = ct_clamp->detect_more()) != nullptr)
        this->report_load_event(*event);
    }
  }
  for (auto *circuit : this->circuits_) {
    if (const LoadEvent *event = circuit->detect(now)) {
      this->report_load_event(*event);
      while ((event = circuit->detect_more()) != nullptr)
        this->report_load_event(*event);
    }
  }
  if (this->unmetered_ != nullptr) {
    // What the mains carry that no listed branch clamp does: loads on breakers without a clamp.
    float watts = 0;
    for (auto *ct_clamp : this->ct_clamps_)
      if (ct_clamp->is_mains())
        watts += ct_clamp->get_power();
    for (auto *ct_clamp : this->unmetered_under_)
      watts -= ct_clamp->get_power();
    if (this->unmetered_->feed(watts, NAN, now)) {
      do
        this->report_load_event(this->unmetered_->event());
      while (this->unmetered_->next());
    }
  }

  if (this->publish_interval_ms_ == 0 || now - this->last_flush_ms_ >= this->publish_interval_ms_) {
    this->last_flush_ms_ = now;
    this->flush_();
  }
  this->frames_since_++;
  this->publish_diagnostics_(now);

  this->callback_.call();
}

void EmporiaVueComponent::flush_() {
  for (auto *phase : this->phases_)
    phase->flush();
  for (auto *ct_clamp : this->ct_clamps_)
    ct_clamp->flush();
  for (auto *circuit : this->circuits_)
    circuit->flush();
}

// The frame rate and missed frames, every publish interval but no more often than every 10 s.
void EmporiaVueComponent::publish_diagnostics_(uint32_t now) {
  if (this->diagnostics_ms_ == 0) {
    this->diagnostics_ms_ = now;
    this->frames_since_ = 0;
    return;
  }
  uint32_t elapsed = now - this->diagnostics_ms_;
  if (elapsed < std::max<uint32_t>(this->publish_interval_ms_, 10000))
    return;
  if (this->frame_rate_sensor_ != nullptr)
    this->frame_rate_sensor_->publish_state(this->frames_since_ * 1000.0f / elapsed);
  if (this->missed_frames_sensor_ != nullptr)
    this->missed_frames_sensor_->publish_state(this->missed_frames_);
  this->diagnostics_ms_ = now;
  this->frames_since_ = 0;
}

void EmporiaVueComponent::add_on_load_event_callback(std::function<void(const LoadEvent &)> &&callback) {
  this->load_event_callback_.add(std::move(callback));
}

static constexpr uint32_t WITH_WITHIN_MS = 700;  // events this close at their start began together

void EmporiaVueComponent::report_load_event(const LoadEvent &event) {
  ESP_LOGD(TAG, "Load event on %s: %.0f W -> %.0f W (peak %.0f W, settled in %u ms)", event.key, event.from_w,
           event.to_w, event.peak_w, (unsigned) event.settle_ms);
  LoadEvent reported = event;
  reported.reported_ms = millis();
  reported.seq = ++this->seq_;
  reported.replay = false;
  // Other channels' events that began within a frame of this one, from those just sent.
  reported.with_count = 0;
  for (uint8_t i = 0; i < this->sent_n_ && reported.with_count < LoadEvent::MAX_WITH; i++) {
    const LoadEvent &other = this->sent_[(this->sent_head_ + REPLAY - 1 - i) % REPLAY];
    uint32_t apart = other.start_ms > event.start_ms ? other.start_ms - event.start_ms : event.start_ms - other.start_ms;
    if (apart <= WITH_WITHIN_MS && strcmp(other.key, event.key) != 0 && !other.cycling && !event.cycling)
      reported.with_keys[reported.with_count++] = other.key;
  }
  this->sent_[this->sent_head_] = reported;
  this->sent_head_ = (this->sent_head_ + 1) % REPLAY;
  this->sent_n_ = std::min<uint8_t>(this->sent_n_ + 1, REPLAY);
  this->load_event_callback_.call(reported);
}

void EmporiaVueComponent::replay_load_events() {
  ESP_LOGD(TAG, "Replaying the last %u load events", (unsigned) this->sent_n_);
  const uint32_t now = millis();
  for (uint8_t i = 0; i < this->sent_n_; i++) {
    LoadEvent again = this->sent_[(this->sent_head_ + REPLAY - this->sent_n_ + i) % REPLAY];
    again.replay = true;
    again.reported_ms = now;  // its age is still counted from when it began
    this->load_event_callback_.call(again);
  }
}

void EmporiaVueComponent::add_on_update_callback(std::function<void()> &&callback) {
  this->callback_.add(std::move(callback));
}

static constexpr float CURRENT_FLOOR_IDLE_W = 3.0f;   // below this a frame's current is the clamp's noise
static constexpr uint32_t CURRENT_FLOOR_FRAMES = 60;  // idle frames seen before the floor is taken out

static inline bool is_mains_port(CTInputPort port) {
  return port == CTInputPort::A || port == CTInputPort::B || port == CTInputPort::C;
}

bool CTClampConfig::is_mains() const { return is_mains_port(this->input_port_); }

void PhaseConfig::update_from_reading(const SensorReading &sensor_reading) {
  this->voltage_now_ = sensor_reading.voltage[this->input_wire_] * this->calibration_;
  this->voltage_.add(this->voltage_now_);

  uint16_t raw_frequency = sensor_reading.frequency;
  if (raw_frequency == 0)
    return;
  // validation that these sensors are allowed on this phase is done in the codegen stage
  if (this->frequency_.sensor) {
    // see https://github.com/emporia-vue-local/esphome/pull/88 for constant explanation
    this->frequency_.add(EMPORIA_VUE_FREQUENCY_CONSTANT / static_cast<float>(raw_frequency));
  }
  if (this->phase_angle_.sensor) {
    uint16_t raw_phase_angle = sensor_reading.degrees[((uint8_t) this->input_wire_) - 1];
    // this is truncated to a uint16_t on the vue 2
    this->phase_angle_.add(((float) raw_phase_angle) * 360.0f / ((float) raw_frequency));
  }
}

void PhaseConfig::flush() {
  this->voltage_.flush();
  this->frequency_.flush();
  this->phase_angle_.flush();
}

int32_t PhaseConfig::extract_power_for_phase(const ReadingPowerEntry &power_entry) {
  switch (this->input_wire_) {
    case PhaseInputWire::BLACK:
      return power_entry.phase_black;
    case PhaseInputWire::RED:
      return power_entry.phase_red;
    case PhaseInputWire::BLUE:
      return power_entry.phase_blue;
    default:
      ESP_LOGE(TAG, "Unsupported phase input wire, this should never happen");
      return -1;
  }
}

void CTClampConfig::update_from_reading(const SensorReading &sensor_reading) {
  ReadingPowerEntry power_entry = sensor_reading.power[this->input_port_];
  int32_t raw_power = this->phase_->extract_power_for_phase(power_entry);
  this->power_now_ = this->get_calibrated_power(raw_power) * (this->reverse_ ? -1.0f : 1.0f);

  uint16_t raw_current = sensor_reading.current[this->input_port_];
  double scalar;
  if (is_mains_port(this->input_port_)) {
    scalar = 775.0 / 42624.0;
  } else {
    scalar = 775.0 / 170496.0;
  }
  float amps = raw_current * scalar;
  if (this->current_floor_) {
    // With next to no power through it, what the clamp reads is its noise: learn its square, and take it
    // out in quadrature (noise adds to a load's current as RMS does), once it has been seen for long enough.
    if (std::fabs(this->power_now_) < CURRENT_FLOOR_IDLE_W) {
      this->floor_sq_ += (amps * amps - this->floor_sq_) / (this->floor_n_ < 50 ? this->floor_n_ + 1 : 50);
      this->floor_n_++;
    }
    if (this->floor_n_ >= CURRENT_FLOOR_FRAMES)
      amps = std::sqrt(std::max(amps * amps - this->floor_sq_, 0.0f));
  }
  this->current_now_ = amps;
  this->va_now_ = this->current_now_ * this->phase_->get_voltage();

  this->power_.add(this->power_now_);
  this->current_.add(this->current_now_);
  this->apparent_power_.add(this->va_now_);
  this->power_factor_.add(this->power_now_, this->va_now_);
}

const LoadEvent *CTClampConfig::detect_more() {
  if (this->detector_ == nullptr || !this->detector_->next())
    return nullptr;
  return &this->detector_->event();
}

const LoadEvent *CTClampConfig::detect(uint32_t now) {
  if (this->detector_ == nullptr)
    return nullptr;
  if (!this->detector_->feed(this->power_now_, this->va_now_, now))
    return nullptr;
  return &this->detector_->event();
}

void CTClampConfig::flush() {
  this->power_.flush();
  this->current_.flush();
  this->apparent_power_.flush();
  this->power_factor_.flush();
}

void CircuitConfig::update_from_clamps() {
  float watts = 0, va = 0, amps = 0;
  for (auto *clamp : this->clamps_) {
    watts += clamp->get_power();
    va += clamp->get_apparent_power();
    amps = std::max(amps, clamp->get_current());
  }
  this->power_now_ = watts;
  this->va_now_ = va;
  this->power_.add(watts);
  this->current_.add(amps);
  this->apparent_power_.add(va);
  this->power_factor_.add(watts, va);
}

const LoadEvent *CircuitConfig::detect_more() {
  if (this->detector_ == nullptr || !this->detector_->next())
    return nullptr;
  return &this->detector_->event();
}

const LoadEvent *CircuitConfig::detect(uint32_t now) {
  if (this->detector_ == nullptr)
    return nullptr;
  if (!this->detector_->feed(this->power_now_, this->va_now_, now))
    return nullptr;
  return &this->detector_->event();
}

void CircuitConfig::flush() {
  this->power_.flush();
  this->current_.flush();
  this->apparent_power_.flush();
  this->power_factor_.flush();
}

float CTClampConfig::get_calibrated_power(int32_t raw_power) const {
  float calibration = this->phase_->get_calibration();

  float correction_factor = (this->input_port_ < 3) ? 5.5 : 22;

  return (raw_power * calibration) / correction_factor;
}

}  // namespace emporia_vue
}  // namespace esphome
