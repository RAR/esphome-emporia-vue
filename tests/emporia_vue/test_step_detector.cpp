// Host tests for the step detector:
//   c++ -std=c++17 -Wall -Wextra -I esphome/components/emporia_vue \
//     tests/emporia_vue/test_step_detector.cpp -o /tmp/test_step && /tmp/test_step

#include "step_detector.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using esphome::emporia_vue::LoadEvent;
using esphome::emporia_vue::StepConfig;
using esphome::emporia_vue::StepDetector;

static int failures = 0;

#define CHECK(cond)                                                  \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      failures++;                                                    \
    }                                                                \
  } while (0)

#define NEAR(a, b, tol) CHECK(std::fabs((a) - (b)) <= (tol))

static constexpr uint32_t FRAME_MS = 200;

// Feeds the frames 200 ms apart and returns every event, in order.
struct Run {
  StepDetector det;
  uint32_t ms = 0;
  std::vector<LoadEvent> events;
  explicit Run(StepConfig cfg = {}) : det("test", cfg) {}
  // pf gives each frame an apparent power of w / pf.
  void feed(std::vector<float> ws, float pf = NAN) {
    for (float w : ws) {
      if (det.feed(w, std::isnan(pf) ? NAN : w / pf, ms))
        events.push_back(det.event());
      ms += FRAME_MS;
    }
  }
  void hold(float w, int frames, float pf = NAN, float jitter = 0) {
    for (int i = 0; i < frames; i++)
      feed({w + ((i % 2) ? jitter : -jitter)}, pf);
  }
};

static void a_heater_is_a_clean_step_up_and_down() {
  Run r;
  r.hold(46, 10, 0.98f, 1);
  r.hold(1530, 10, 1.0f, 2);
  r.hold(46, 10, 0.98f, 1);
  CHECK(r.events.size() == 2);
  if (r.events.size() != 2)
    return;
  const LoadEvent &on = r.events[0];
  NEAR(on.from_w, 46, 2);
  NEAR(on.to_w, 1530, 3);
  NEAR(on.peak_w, 1532, 0.5);
  CHECK(on.settled);
  CHECK(on.settle_ms == 0);                // the first frame was already the new level
  NEAR(on.pf_from(), 0.98, 0.01);
  NEAR(on.pf_to(), 1.0, 0.01);
  NEAR(on.va_to, 1530, 3);
  CHECK(on.start_ms == 10 * FRAME_MS);
  CHECK(on.count >= 3 + 4);                // the lead-in frames, then the step
  NEAR(on.samples[0], 46, 2);
  NEAR(on.samples[3], 1528, 0.5);
  CHECK(!on.pulse);
  const LoadEvent &off = r.events[1];
  NEAR(off.from_w, 1530, 3);
  NEAR(off.to_w, 46, 2);
}

static void a_motor_shows_its_inrush_and_how_long_it_took_to_settle() {
  Run r;
  r.hold(10, 10, 0.5f);
  r.feed({2400, 1200, 700, 650, 652, 648, 651, 650, 650}, 0.8f);
  CHECK(r.events.size() == 1);
  if (r.events.empty())
    return;
  const LoadEvent &e = r.events[0];
  NEAR(e.peak_w, 2400, 0.1);
  NEAR(e.to_w, 650, 3);
  CHECK(e.settled);
  CHECK(e.settle_ms == 3 * FRAME_MS);      // 650 was the first of the agreeing frames
  NEAR(e.pf_to(), 0.8, 0.01);
  NEAR(e.va_to, 650 / 0.8, 5);
}

static void one_odd_frame_is_not_a_step() {
  Run r;
  r.hold(10, 10);
  r.feed({400});
  r.hold(10, 10);
  CHECK(r.events.empty());
  NEAR(r.det.level(), 10, 0.5);
}

static void noise_and_slow_drift_are_not_steps() {
  Run r;
  r.hold(400, 50, NAN, 12);                // ±12 W is under 30 W
  for (int i = 0; i < 100; i++)
    r.feed({400 + i * 0.8f});              // 80 W over 20 s
  CHECK(r.events.empty());
  NEAR(r.det.level(), 479, 5);
}

static void a_brief_load_comes_back_as_a_pulse() {
  Run r;
  r.hold(10, 10);
  r.feed({800, 800});
  r.hold(10, 6);
  CHECK(r.events.size() == 1);
  if (r.events.empty())
    return;
  NEAR(r.events[0].peak_w, 800, 0.1);
  NEAR(r.events[0].to_w, 10, 0.5);
  CHECK(r.events[0].pulse);
}

static void a_load_that_never_settles_is_reported_when_it_times_out() {
  Run r;
  r.hold(10, 10);
  for (int i = 0; i < 60; i++)
    r.feed({(i % 2) ? 500.0f : 700.0f});
  CHECK(r.events.size() >= 1);
  if (r.events.empty())
    return;
  const LoadEvent &e = r.events[0];
  CHECK(!e.settled);
  CHECK(e.settle_ms >= 10000 - 8 * FRAME_MS);  // reported at the timeout, its last frames as the level
  NEAR(e.to_w, 600, 1);
  CHECK(e.count == LoadEvent::MAX_SAMPLES);  // the capture stops filling, it does not overflow
}

static void steps_follow_on_from_each_other() {
  Run r;
  r.hold(100, 10);
  r.hold(600, 8);
  r.hold(1800, 8);
  r.hold(100, 8);
  CHECK(r.events.size() == 3);
  if (r.events.size() != 3)
    return;
  NEAR(r.events[1].from_w, 600, 1);
  NEAR(r.events[1].to_w, 1800, 1);
  NEAR(r.events[2].from_w, 1800, 1);
  NEAR(r.events[2].samples[0], 1800, 1);   // the lead-in is the level it left
}

static void a_big_or_wobbly_level_needs_a_bigger_step() {
  Run r;
  r.hold(3000, 40, NAN, 20);
  r.hold(3050, 10, NAN, 20);               // under 2 %, and under 4 x its 20 W wobble
  CHECK(r.events.empty());
  r.hold(3400, 10, NAN, 20);
  CHECK(r.events.size() == 1);
}

static void the_event_reads_as_json() {
  StepDetector det("hot \"tub\"");
  uint32_t ms = 0;
  for (int i = 0; i < 5; i++)
    det.feed(46, NAN, ms += FRAME_MS);
  bool done = false;
  for (int i = 0; i < 6 && !done; i++)
    done = det.feed(1530, NAN, ms += FRAME_MS);
  CHECK(done);
  std::string json = det.event().to_json(ms + 1000);
  std::printf("  %s\n", json.c_str());
  CHECK(json.rfind("{\"key\":\"hot \\\"tub\\\"\",\"age_ms\":", 0) == 0);
  CHECK(json.find("\"from_w\":46.0") != std::string::npos);
  CHECK(json.find("\"to_w\":1530.0") != std::string::npos);
  CHECK(json.find("\"va_from\":null,\"va_to\":null,\"pf_from\":null,\"pf_to\":null") != std::string::npos);
  CHECK(json.find("\"settled\":true,\"pulse\":false") != std::string::npos);
  CHECK(json.find("\"dt_ms\":200,\"samples\":[46,46,46,1530,1530,1530,1530]}") != std::string::npos);
}

static void unknown_frames_are_skipped() {
  Run r;
  r.hold(10, 5);
  r.feed({NAN, NAN});
  r.hold(10, 5);
  CHECK(r.events.empty());
}

// A fixed pseudo-random sequence, so the noise tests always see the same frames.
struct Lcg {
  uint32_t state = 12345;
  float gauss() {  // roughly normal, mean 0, sd 1 (sum of 12 uniforms)
    float sum = 0;
    for (int i = 0; i < 12; i++) {
      state = state * 1664525u + 1013904223u;
      sum += (state >> 8) / 16777216.0f;
    }
    return sum - 6.0f;
  }
};

static void a_noisy_load_learns_its_wobble_and_stays_quiet() {
  Run r;
  Lcg rng;
  for (int i = 0; i < 7200; i++)           // an hour at 2 frames a second, sd 15 W
    r.feed({430 + 15 * rng.gauss()});
  CHECK(r.events.size() <= 3);
  std::printf("  %zu events in an hour of 15 W noise, wobble %.1f W\n", r.events.size(), r.det.noise());
  r.hold(1630, 10);                        // a real 1200 W step is still seen
  CHECK(!r.events.empty() && std::fabs(r.events.back().to_w - 1630) < 5);
}

static void an_oscillating_load_reports_once_not_every_timeout() {
  Run r;
  r.hold(10, 10);
  for (int i = 0; i < 1200; i++)           // 10 minutes of 500, 500, 700, 700, ...
    r.feed({(i / 2) % 2 ? 700.0f : 500.0f});
  CHECK(r.events.size() <= 2);
  std::printf("  %zu events in 10 minutes of oscillation\n", r.events.size());
  r.hold(10, 10);                          // and it switching off is still seen
  CHECK(!r.events.empty() && std::fabs(r.events.back().to_w - 10) < 5);
}

static void a_load_is_seen_switching_off_a_busy_circuit_as_it_was_switching_on() {
  Run r;
  r.hold(2880, 10);
  r.hold(3000, 10);                        // on: 120 W onto 2880 W
  r.hold(2880, 10);                        // off: 120 W off 3000 W
  CHECK(r.events.size() == 2);
}

int main() {
  struct {
    const char *name;
    void (*fn)();
  } tests[] = {
      {"a heater is a clean step up and down", a_heater_is_a_clean_step_up_and_down},
      {"a motor shows its inrush and how long it took to settle", a_motor_shows_its_inrush_and_how_long_it_took_to_settle},
      {"one odd frame is not a step", one_odd_frame_is_not_a_step},
      {"noise and slow drift are not steps", noise_and_slow_drift_are_not_steps},
      {"a brief load comes back as a pulse", a_brief_load_comes_back_as_a_pulse},
      {"a load that never settles is reported when it times out", a_load_that_never_settles_is_reported_when_it_times_out},
      {"steps follow on from each other", steps_follow_on_from_each_other},
      {"a big or wobbly level needs a bigger step", a_big_or_wobbly_level_needs_a_bigger_step},
      {"the event reads as json", the_event_reads_as_json},
      {"unknown frames are skipped", unknown_frames_are_skipped},
      {"a noisy load learns its wobble and stays quiet", a_noisy_load_learns_its_wobble_and_stays_quiet},
      {"an oscillating load reports once, not every timeout", an_oscillating_load_reports_once_not_every_timeout},
      {"a load is seen switching off a busy circuit as it was switching on", a_load_is_seen_switching_off_a_busy_circuit_as_it_was_switching_on},
  };
  for (auto &t : tests) {
    int before = failures;
    t.fn();
    std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", t.name);
  }
  std::printf("%d failure(s)\n", failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
