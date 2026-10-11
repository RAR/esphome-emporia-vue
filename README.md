Unofficial ESPHome-based local control for your Emporia Vue energy monitor.

- **Rapid response**: Data updates every five seconds. This allows for quick-responding automations and rapid feedback when your consumption changes.
- **Totally offline**: Everything is completely local, and nothing leaves your network. No internet? No problem, everything still works fine.
- **You're in control**: All the source code and configuration is available for you to use and modify to fit your particular situation. Data feeds into Home Assistant, where it can be visualized and acted upon.

See [the new website for installation instructions](https://emporia-vue-local.github.io/docs/tutorial/intro). Please leave any feedback or questions in the discussion section.

The older README that was previously here [is still available](https://github.com/emporia-vue-local/esphome/blob/dev/README.old.md).

## This fork: better sensing

A drop-in `emporia_vue` with more out of each frame the Vue's meter chip sends
(a Vue 3 delivers one about every 500 ms; the component polls every 200 ms so
none are missed), all optional; configurations written for upstream work unchanged.

```yaml
external_components:
  - source: github://RAR/esphome-emporia-vue@better-sensing
    components: [emporia_vue]
```

- **Power factor and apparent power per clamp** — `power_factor:` and
  `apparent_power:` beside `power:` and `current:`, from each input's current
  and its phase's voltage. Power factor is unknown below 25 VA, where the
  current reading is mostly the clamp's noise floor (an idle clamp reads ~0.12 A).
- **Circuits on more than one clamp** — a 240 V breaker with a clamp on each
  leg is one entry under `circuits:` (`inputs: ["3", "4"]`): its power is the
  sum, its current the larger leg's. No template sensors to keep in step.
- **`publish_interval:`** — publish each sensor's mean over that span (for
  example `5s`) instead of every frame. Energy from `total_daily_energy` on
  the averaged sensor is the same, and nothing else (events, `on_update`)
  slows down. Leave it out to publish every frame, as before.
- **Load events** — every branch clamp and circuit is watched frame by frame
  for steps: a load switching on or off. Each finished step fires
  `on_load_event` with `event`; `event.json()` gives
  `{"key","age_ms","from_w","to_w","peak_w","va_from","va_to","pf_from","pf_to","noise_w","settle_ms","settled","pulse","drift","held_w","held_span_w","held_ms","stages","dt_ms","samples"}`
  — where it was, where it went, its inrush peak, the apparent power and
  power factor either side (the load's own reactive part is
  √(va²−w²) to minus from), the level's usual wobble, how long it took to
  settle, and the frames around the step. `pulse` is a step that went and
  came back (an inrush, a brief load); `settled: false` one still moving at
  the timeout. A step must clear the larger of `min_step`, `min_step_percent`
  of the smaller level, and `noise_factor` times the level's learned wobble
  (that part held to `max_noise` if set), so a noisy or oscillating load stays quiet
  instead of spamming events. A change too gradual or too jittery to make a
  step (a load fading off, or one switching off a circuit whose wobble hides
  it) is still reported once a slow average has held two steps away for
  `drift_frames`: `drift: true`, from and to are averages, and it is dated
  from when the change began. Change slower than about 5 minutes (a heater
  sagging as it warms) is followed, not reported.
  Every event also gives the stretch it ended (`held_w` its mean, `held_ms`,
  `held_span_w` its max minus min), so a load that varies while on is
  measured by what it drew, and `stages`: levels the step passed through for
  two frames or more on the way (`[[270,3]]` — a PC waking, then its screen).
  A load switching on and off over and over (a 3D printer's bed, a bang-bang
  element) is reported once it has made `cycle_steps` alternating steps alike
  in size between the same two levels within `cycle_window`: one event with
  `"cycling":"start"` (from its low level to its mean so far; `spans_ms` back
  to its first switch, which were already reported) and one with
  `"cycling":"end"` (from its mean, `held_w`, to the level after), each with
  `amp_w`, `period_ms` and `duty`. The switches between are not reported. A
  step that does not fit (another load switching) ends it and is reported.
  Mains clamps and clamps inside a circuit are not watched unless they say
  `load_events: true`; any can say `load_events: false`. The key is the power
  sensor's name without "Power" (`Hot Tub Power` → `hot_tub`), or `key:`.
- **Diagnostics** — `frame_rate:` (frames per second actually read) and
  `missed_frames:` (frames the ESP32 fell behind on).
- `reverse: true` on a clamp fitted the wrong way round.

```yaml
sensor:
  - platform: emporia_vue
    variant: vue3
    i2c_id: i2c_a
    publish_interval: 5s
    frame_rate: { name: Frame Rate }
    missed_frames: { name: Missed Frames }
    phases:
      - { id: phase_a, input: BLACK, calibration: 0.01925, voltage: { name: Phase A Voltage } }
      - { id: phase_b, input: RED, calibration: 0.01925, voltage: { name: Phase B Voltage } }
    ct_clamps:
      - { phase_id: phase_a, input: "A", power: { name: Phase A Power } }
      - { phase_id: phase_b, input: "B", power: { name: Phase B Power } }
      - { phase_id: phase_a, input: "1" }
      - { phase_id: phase_b, input: "2" }
      - phase_id: phase_a
        input: "3"
        power: { name: Kitchen Power, id: kitchen_power }
        power_factor: { name: Kitchen Power Factor }
    circuits:
      - inputs: ["1", "2"]
        power: { name: Dryer Power, id: dryer_power }
        current: { name: Dryer Current }
    step_detection:       # all optional; these are the defaults
      min_step: 30W       # a step must move at least this far
      min_step_percent: 2%   # ...and this fraction of the smaller level
      noise_factor: 4     # ...and this many times the level's usual wobble
      max_noise: 0W       # ...that part held to this if set (never under 1.5x the wobble); 0W no cap
      settle_within: 10W  # settled once settle_frames span no more than this
      settle_within_percent: 3%
      confirm_frames: 2   # 1 s at 500 ms frames
      settle_frames: 4    # 2 s
      timeout: 10s
      drift_frames: 20    # a gradual change held this long (10 s) is reported; 0 never
      cycle_steps: 4      # alternating steps alike in size that make a cycling load; 0 never
      cycle_tolerance: 35%
      cycle_window: 60s
    on_load_event:
      - mqtt.publish:
          topic: my-vue/load_event
          payload: !lambda return event.json();
```

The step detector is plain C++ (`step_detector.h`) with host tests:

```sh
c++ -std=c++17 -Wall -Wextra -I esphome/components/emporia_vue \
  tests/emporia_vue/test_step_detector.cpp -o /tmp/test_step && /tmp/test_step
```
