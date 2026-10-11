#pragma once

// Finds steps in one channel's power, frame by frame: a load switching on or
// off, its inrush peak, how long it took to settle, and the real and apparent
// power on either side. Plain C++ with no ESPHome dependencies, so it is
// tested on the host (tests/emporia_vue/test_step_detector.cpp).
//
// The Vue's meter chip delivers a frame about every 500 ms (Vue 3), so the
// defaults mean: a step must hold for 1 s and settle over 2 s.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace esphome {
namespace emporia_vue {

// Below this apparent power the current reading is mostly the clamp's noise
// floor (an idle clamp reads ~0.12 A), so a power factor means nothing.
static constexpr float MIN_POWER_FACTOR_VA = 25.0f;

// The power factor of real and apparent power, or NAN when there is too little to tell.
inline float power_factor(float watts, float va) {
  if (std::isnan(watts) || std::isnan(va) || va < MIN_POWER_FACTOR_VA)
    return NAN;
  return std::min(1.0f, std::fabs(watts) / va);
}

struct StepConfig {
  float min_step_w = 30.0f;    // a step must move at least this far...
  float rel_step = 0.02f;      // ...and this fraction of the smaller of the two levels...
  float noise_factor = 4.0f;   // ...and this many times the level's usual wobble
  float settle_w = 10.0f;      // settled once the last frames span no more than this...
  float settle_rel = 0.03f;    // ...or this fraction of their level, or twice the wobble
  uint8_t confirm = 2;         // frames away from the level before it counts as a step
  uint8_t settle_frames = 4;   // frames that must agree to call it settled (at most 8)
  uint32_t timeout_ms = 10000; // a step that has not settled by then is reported as is
};

struct LoadEvent {
  static constexpr uint8_t MAX_SAMPLES = 48;

  const char *key{""};
  float from_w{0}, to_w{0}, peak_w{0};
  float va_from{NAN}, va_to{NAN};  // apparent power either side; NAN without a current reading
  float noise_w{0};                // the level's usual wobble (mean absolute deviation)
  uint32_t start_ms{0};            // the first frame that moved
  uint32_t settle_ms{0};           // from start_ms to the first of the settled frames
  bool settled{false};             // false when it timed out still moving
  bool pulse{false};               // it came back to where it was: an inrush or a brief load
  uint32_t first_ms{0}, last_ms{0};  // the first and last stored samples
  uint32_t reported_ms{0};           // when it was handed on (set by the component)
  uint8_t count{0};
  float samples[MAX_SAMPLES]{};    // frames from just before the step onwards (W)

  float pf_from() const { return power_factor(this->from_w, this->va_from); }
  float pf_to() const { return power_factor(this->to_w, this->va_to); }

  // As JSON, with age_ms counted to when it was reported.
  std::string json() const { return this->to_json(this->reported_ms); }

  std::string to_json(uint32_t now_ms) const {
    std::string out;
    out.reserve(220 + 7 * this->count);
    char buf[64];
    auto num = [&](const char *name, float v, const char *fmt) {
      if (std::isnan(v)) {
        snprintf(buf, sizeof(buf), ",\"%s\":null", name);
      } else {
        int n = snprintf(buf, sizeof(buf), ",\"%s\":", name);
        snprintf(buf + n, sizeof(buf) - n, fmt, v);
      }
      out += buf;
    };
    out += "{\"key\":\"";
    for (const char *c = this->key; *c; c++) {
      if (*c == '"' || *c == '\\')
        out += '\\';
      out += *c;
    }
    out += '"';
    snprintf(buf, sizeof(buf), ",\"age_ms\":%u", (unsigned) (now_ms - this->start_ms));
    out += buf;
    num("from_w", this->from_w, "%.1f");
    num("to_w", this->to_w, "%.1f");
    num("peak_w", this->peak_w, "%.1f");
    num("va_from", this->va_from, "%.1f");
    num("va_to", this->va_to, "%.1f");
    num("pf_from", this->pf_from(), "%.2f");
    num("pf_to", this->pf_to(), "%.2f");
    num("noise_w", this->noise_w, "%.1f");
    snprintf(buf, sizeof(buf), ",\"settle_ms\":%u,\"settled\":%s,\"pulse\":%s", (unsigned) this->settle_ms,
             this->settled ? "true" : "false", this->pulse ? "true" : "false");
    out += buf;
    unsigned dt = this->count > 1 ? (unsigned) ((this->last_ms - this->first_ms) / (this->count - 1)) : 0;
    snprintf(buf, sizeof(buf), ",\"dt_ms\":%u,\"samples\":[", dt);
    out += buf;
    for (uint8_t i = 0; i < this->count; i++) {
      snprintf(buf, sizeof(buf), i ? ",%.0f" : "%.0f", this->samples[i]);
      out += buf;
    }
    out += "]}";
    return out;
  }
};

class StepDetector {
 public:
  static constexpr uint8_t PRE = 3;     // steady frames kept to lead into a step
  static constexpr uint8_t WINDOW = 8;  // the most frames the settle check looks back over

  explicit StepDetector(const char *key = "", StepConfig cfg = {}) : cfg_(cfg) {
    this->event_.key = key;
    this->cfg_.settle_frames = std::max<uint8_t>(2, std::min<uint8_t>(this->cfg_.settle_frames, WINDOW));
    this->cfg_.confirm = std::max<uint8_t>(1, std::min<uint8_t>(this->cfg_.confirm, WINDOW));
  }

  const StepConfig &config() const { return this->cfg_; }
  const LoadEvent &event() const { return this->event_; }
  float level() const { return this->base_; }
  float noise() const { return this->noise_; }

  // Feed one frame (real power, apparent power or NAN); true when a step has
  // just finished (read it from event()).
  bool feed(float w, float va, uint32_t ms) {
    if (std::isnan(w))
      return false;
    if (!this->primed_) {
      this->primed_ = true;
      this->base_ = w;
      this->base_va_ = va;
      this->remember(w, va, ms);
      return false;
    }
    if (!this->moving_) {
      float off = std::fabs(w - this->base_);
      if (off <= this->threshold(this->base_, w)) {
        this->pending_ = 0;
        // Learn the level's usual wobble, and follow slow drift.
        this->noise_ += 0.05f * (off - this->noise_);
        this->base_ += 0.25f * (w - this->base_);
        if (!std::isnan(va))
          this->base_va_ = std::isnan(this->base_va_) ? va : this->base_va_ + 0.25f * (va - this->base_va_);
        this->remember(w, va, ms);
        return false;
      }
      // Away from the level: wait for `confirm` frames in a row before calling it a step.
      this->pend_w_[this->pending_] = w;
      this->pend_va_[this->pending_] = va;
      this->pend_ms_[this->pending_] = ms;
      if (++this->pending_ < this->cfg_.confirm)
        return false;
      this->start();
      return false;
    }
    this->capture(w, va, ms);
    return this->check(ms);
  }

 protected:
  // How far a frame must be from the level to count: symmetric, so a load
  // switching on and the same load switching off are both seen.
  float threshold(float level, float w) const {
    float smaller = std::min(std::fabs(level), std::fabs(w));
    return std::max({this->cfg_.min_step_w, this->cfg_.rel_step * smaller, this->cfg_.noise_factor * this->noise_});
  }
  float tolerance(float level) const {
    return std::max({this->cfg_.settle_w, this->cfg_.settle_rel * std::fabs(level), 2.0f * this->noise_});
  }

  void remember(float w, float va, uint32_t ms) {
    this->pre_w_[this->pre_head_] = w;
    this->pre_va_[this->pre_head_] = va;
    this->pre_ms_[this->pre_head_] = ms;
    this->pre_head_ = (this->pre_head_ + 1) % PRE;
    this->pre_n_ = std::min<uint8_t>(this->pre_n_ + 1, PRE);
  }

  void start() {
    LoadEvent &e = this->event_;
    e.from_w = this->base_;
    e.va_from = this->base_va_;
    e.noise_w = this->noise_;
    e.peak_w = this->base_;
    e.start_ms = this->pend_ms_[0];
    e.count = 0;
    for (uint8_t i = 0; i < this->pre_n_; i++) {
      uint8_t at = (this->pre_head_ + PRE - this->pre_n_ + i) % PRE;
      if (i == 0)
        e.first_ms = this->pre_ms_[at];
      e.samples[e.count++] = this->pre_w_[at];
      e.last_ms = this->pre_ms_[at];
    }
    if (e.count == 0)
      e.first_ms = this->pend_ms_[0];
    this->moving_ = true;
    this->win_n_ = 0;
    uint8_t n = this->pending_;
    this->pending_ = 0;
    for (uint8_t i = 0; i < n; i++)
      this->capture(this->pend_w_[i], this->pend_va_[i], this->pend_ms_[i]);
  }

  void capture(float w, float va, uint32_t ms) {
    LoadEvent &e = this->event_;
    if (e.count < LoadEvent::MAX_SAMPLES) {
      e.samples[e.count++] = w;
      e.last_ms = ms;
    }
    if (std::fabs(w - e.from_w) > std::fabs(e.peak_w - e.from_w))
      e.peak_w = w;
    uint8_t at = this->win_head_;
    this->win_w_[at] = w;
    this->win_va_[at] = va;
    this->win_ms_[at] = ms;
    this->win_head_ = (at + 1) % WINDOW;
    this->win_n_ = std::min<uint8_t>(this->win_n_ + 1, WINDOW);
  }

  // The last `n` captured frames, oldest first.
  uint8_t recent(uint8_t i, uint8_t n) const { return (this->win_head_ + WINDOW - n + i) % WINDOW; }

  bool check(uint32_t ms) {
    LoadEvent &e = this->event_;
    const uint8_t n = this->cfg_.settle_frames;
    bool timed_out = ms - e.start_ms >= this->cfg_.timeout_ms;
    if (this->win_n_ < n && !timed_out)
      return false;
    uint8_t have = std::min(this->win_n_, n);
    float lo = INFINITY, hi = -INFINITY, sum = 0, va_sum = 0;
    uint8_t va_n = 0;
    for (uint8_t i = 0; i < have; i++) {
      uint8_t at = this->recent(i, have);
      lo = std::min(lo, this->win_w_[at]);
      hi = std::max(hi, this->win_w_[at]);
      sum += this->win_w_[at];
      if (!std::isnan(this->win_va_[at])) {
        va_sum += this->win_va_[at];
        va_n++;
      }
    }
    float mean = sum / have;
    bool settled = have == n && hi - lo <= this->tolerance(mean);
    if (!settled && !timed_out)
      return false;
    e.to_w = mean;
    e.va_to = va_n ? va_sum / va_n : NAN;
    e.settled = settled;
    e.pulse = std::fabs(mean - e.from_w) < this->threshold(e.from_w, mean);
    e.settle_ms = this->win_ms_[this->recent(0, have)] - e.start_ms;
    // The settled frames are the new level and lead into the next step.
    this->base_ = mean;
    if (va_n)
      this->base_va_ = e.va_to;
    // A level that never settled wobbles at least this much: an oscillating
    // load would otherwise fire a timed-out event every timeout.
    if (!settled)
      this->noise_ = std::max(this->noise_, (hi - lo) / 2.0f);
    this->pre_n_ = 0;
    this->pre_head_ = 0;
    for (uint8_t i = std::max<int>(0, have - PRE); i < have; i++) {
      uint8_t at = this->recent(i, have);
      this->remember(this->win_w_[at], this->win_va_[at], this->win_ms_[at]);
    }
    this->moving_ = false;
    return true;
  }

  StepConfig cfg_;
  LoadEvent event_;
  bool primed_{false};
  bool moving_{false};
  float base_{0};
  float base_va_{NAN};
  float noise_{0};
  uint8_t pending_{0};
  float pend_w_[WINDOW]{}, pend_va_[WINDOW]{};
  uint32_t pend_ms_[WINDOW]{};
  float pre_w_[PRE]{}, pre_va_[PRE]{};
  uint32_t pre_ms_[PRE]{};
  uint8_t pre_head_{0}, pre_n_{0};
  float win_w_[WINDOW]{}, win_va_[WINDOW]{};
  uint32_t win_ms_[WINDOW]{};
  uint8_t win_head_{0}, win_n_{0};
};

}  // namespace emporia_vue
}  // namespace esphome
