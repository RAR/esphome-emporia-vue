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
  void feed(std::vector<float> ws, float pf = NAN) {
    for (float w : ws) {
      if (det.feed(w, pf, ms))
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
  NEAR(on.pf_from, 0.98, 0.01);
  NEAR(on.pf_to, 1.0, 0.01);
  CHECK(on.start_ms == 10 * FRAME_MS);
  CHECK(on.count >= 3 + 4);                // the lead-in frames, then the step
  NEAR(on.samples[0], 46, 2);
  NEAR(on.samples[3], 1528, 0.5);
  CHECK(!on.is_pulse(r.det.config()));
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
  NEAR(e.pf_to, 0.8, 0.01);
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
  CHECK(r.events[0].is_pulse(r.det.config()));
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
  CHECK(e.last_ms - e.start_ms >= 10000);
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

static void a_big_level_needs_a_proportionally_big_step() {
  Run r;
  r.hold(3000, 10, NAN, 20);
  r.hold(3120, 10, NAN, 20);               // 4 %, under the 5 % threshold
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
  CHECK(json.find("\"pf_from\":null,\"pf_to\":null") != std::string::npos);
  CHECK(json.find("\"settled\":true") != std::string::npos);
  CHECK(json.find("\"dt_ms\":200,\"samples\":[46,46,46,1530,1530,1530,1530]}") != std::string::npos);
}

static void unknown_frames_are_skipped() {
  Run r;
  r.hold(10, 5);
  r.feed({NAN, NAN});
  r.hold(10, 5);
  CHECK(r.events.empty());
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
      {"a big level needs a proportionally big step", a_big_level_needs_a_proportionally_big_step},
      {"the event reads as json", the_event_reads_as_json},
      {"unknown frames are skipped", unknown_frames_are_skipped},
  };
  for (auto &t : tests) {
    int before = failures;
    t.fn();
    std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", t.name);
  }
  std::printf("%d failure(s)\n", failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
