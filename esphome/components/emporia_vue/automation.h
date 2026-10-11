#pragma once

#ifdef USE_ESP32

#include "emporia_vue.h"

#include "esphome/core/automation.h"

namespace esphome {
namespace emporia_vue {

// Trigger for after statistics sensors are updated
class EmporiaVueUpdateTrigger : public Trigger<> {
 public:
  explicit EmporiaVueUpdateTrigger(EmporiaVueComponent *parent) {
    parent->add_on_update_callback([this]() { this->trigger(); });
  }
};

// Trigger for when a clamp or circuit watching for load events sees a step finish
class EmporiaVueLoadEventTrigger : public Trigger<const LoadEvent &> {
 public:
  explicit EmporiaVueLoadEventTrigger(EmporiaVueComponent *parent) {
    parent->add_on_load_event_callback([this](const LoadEvent &event) { this->trigger(event); });
  }
};

}  // namespace emporia_vue
}  // namespace esphome

#endif // ifdef USE_ESP32