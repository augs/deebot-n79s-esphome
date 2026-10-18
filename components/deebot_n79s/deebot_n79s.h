#pragma once

// Deebot N79S mainboard UART protocol, with the command set from the Ecovacs
// Android app's DN79SProtocol.  Protocol reference: ../../PROTOCOL.md.
//
// Framing (both directions):
//   "`F" + channel + escape([address] + payload + "\0" + crc8) + "\n"
// channel is A (XMPP relay), B (unsolicited report) or C (local).  crc8 is
// CRC-8/SMBUS (poly 0x07, init 0x00, no reflection, no xorout) over the
// un-escaped bytes after the 3 byte "`FX" header.  escape() maps
// 0x5C/0x60/0x0A to 5C 01/02/03.

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

namespace esphome {
namespace deebot_n79s {

uint8_t crc8(const uint8_t *data, size_t len, uint8_t crc = 0);
// Escape 0x5C/0x60/0x0A as 5C 01/02/03 (everything after the header).
std::string escape(const std::string &data);
// Undo escape(); false on an invalid escape, where the MCU drops the frame.
bool unescape(const std::string &data, std::string *out);
// Build a complete, escaped UART frame for an XML payload.
std::string frame(const std::string &payload, char channel = 'A');
// True if a complete raw "`FX...<crc>\n" frame from the wire is intact.
bool check_frame(const std::string &msg);

// One start tag of a payload, with its attributes and the text after it.
struct Element {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attrs;
  std::string text;

  bool get(const char *key, std::string *out) const;
  bool has(const char *key) const;
};
// Flat list of the start tags in a payload, in document order.  Lenient
// enough for the mainboard's not-quite-XML (both quote styles, stray tags).
std::vector<Element> parse_elements(const std::string &body);

struct Schedule {
  std::string name;
  bool enabled;
  std::string time;    // HH:MM, robot local time.
  std::string days;    // 7 x 0/1, Sunday first.
  std::string source;  // p app, q device, i IR remote.
};

class DeebotN79S : public PollingComponent, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_battery_level_sensor(sensor::Sensor *s) { this->battery_level_sensor_ = s; }
  void set_brush_sensor(sensor::Sensor *s) { this->brush_sensor_ = s; }
  void set_side_brush_sensor(sensor::Sensor *s) { this->side_brush_sensor_ = s; }
  void set_dust_case_sensor(sensor::Sensor *s) { this->dust_case_sensor_ = s; }
  void set_status_text_sensor(text_sensor::TextSensor *s) { this->status_text_sensor_ = s; }
  void set_error_text_sensor(text_sensor::TextSensor *s) { this->error_text_sensor_ = s; }
  void set_fan_speed_text_sensor(text_sensor::TextSensor *s) { this->fan_speed_text_sensor_ = s; }
  void set_clean_mode_text_sensor(text_sensor::TextSensor *s) { this->clean_mode_text_sensor_ = s; }
  void set_charge_state_text_sensor(text_sensor::TextSensor *s) { this->charge_state_text_sensor_ = s; }
  void set_firmware_version_text_sensor(text_sensor::TextSensor *s) { this->firmware_version_text_sensor_ = s; }
  void set_schedules_text_sensor(text_sensor::TextSensor *s) { this->schedules_text_sensor_ = s; }
#ifdef USE_TIME
  void set_time_source(time::RealTimeClock *clock) { this->time_source_ = clock; }
#endif

  // Cleaning.  start() cleans in clean_mode; the MCU has no resume, so this
  // also continues a paused clean (resume() is the same).
  void start();
  void clean();
  // type is auto, border, spot or singleRoom.
  void clean(const std::string &type);
  void spot_clean() { this->clean("spot"); }
  void pause();
  void resume();
  void stop();
  void charge();
  void cancel_charge();
  void locate();
  // action is forward, SpinLeft, SpinRight, TurnAround or stop.
  void move(const std::string &action);
  // Mode used by clean()/start(): auto, border, spot or singleRoom.
  void set_clean_mode(const std::string &mode);
  const std::string &get_clean_mode() const { return this->clean_mode_; }
  // speed is standard or strong.
  void set_fan_speed(const std::string &speed);
  // component is Brush, SideBrush or DustCaseHeap.
  void reset_lifespan(const std::string &component);

  // Set the vacuum clock, t is a unix timestamp, tz_minutes the UTC offset.
  void set_time(int64_t t, int tz_minutes);
#ifdef USE_TIME
  // Set the vacuum clock from the configured time source.
  void set_time();
#endif

  // Schedules always run an auto clean at the current fan speed.  days is 7
  // characters of 0/1, Sunday first.  An empty name on add generates one.
  void add_schedule(const std::string &name, int hour, int minute, const std::string &days, bool enabled);
  void modify_schedule(const std::string &name, int hour, int minute, const std::string &days, bool enabled);
  void delete_schedule(const std::string &name);
  void request_schedules();
  const std::vector<Schedule> &get_schedules() const { return this->schedules_; }

  void request_status();
  void request_supplies();
  void request_version();
  // Queue a raw payload, without an id.
  void send_payload(const std::string &payload, char channel = 'A');

 protected:
  enum class Request : uint8_t { OTHER, GET_SCHED, SCHED_CHANGE, CLEAN, PAUSE, SET_SPEED, SET_LIFESPAN };
  struct Pending {
    uint32_t id;
    const char *td;
    Request kind;
    std::string arg;
  };

  // Queue <ctl id='N' td='TD' ATTRS/> (or with CHILDREN) and remember the id.
  void send_ctl_(const char *td, const std::string &attrs = "", const std::string &children = "",
                 Request kind = Request::OTHER, const std::string &arg = "");
  void send_clean_(const std::string &type, const char *act);
  bool sched_xml_(const std::string &name, int hour, int minute, const std::string &days, bool enabled,
                  std::string *out);
  void handle_byte_(uint8_t c);
  void rx_append_(uint8_t c);
  void reset_rx_();
  void parse_message_(const std::string &msg);
  void handle_report_(const std::string &td, const std::vector<Element> &els);
  void handle_reply_(const std::vector<Element> &els);
  void apply_clean_(const Element &clean, bool report);
  void apply_charge_(const Element &charge);
  bool apply_version_(const std::vector<Element> &els);
  void apply_lifespan_(const Element &el);
  void apply_error_(const Element &root);
  void apply_schedules_(const std::vector<Element> &els);
  void set_error_(const std::string &error);
  void publish_status_();

  std::string rx_buf_;
  // Header + un-escaped body.  Running CRC over the body, 0 once the check
  // byte is in.
  uint8_t rx_crc_{0};
  // The last '\n' did not end a valid frame, so it was kept as data (a check
  // byte or binary payload).  A '`' next means the frame was corrupt.
  bool rx_after_bad_newline_{false};
  // The last raw byte was 0x5C, the next is an escape code.
  bool rx_escape_{false};

  std::deque<std::string> tx_queue_;
  uint32_t last_tx_ms_{0};
  uint32_t next_id_{0};
  std::deque<Pending> pending_;
  bool xmpp_announced_{false};

  // Robot state, combined into the status text sensor by publish_status_().
  char clean_st_{'h'};  // s cleaning, h halted.
  // Paused by us; the MCU can't report it.
  bool paused_{false};
  uint32_t paused_since_ms_{0};
  std::string charge_state_;
  bool error_active_{false};
  bool powered_off_{false};
  std::string clean_mode_{"auto"};
  std::string last_status_;
  std::vector<Schedule> schedules_;

  sensor::Sensor *battery_level_sensor_{nullptr};
  sensor::Sensor *brush_sensor_{nullptr};
  sensor::Sensor *side_brush_sensor_{nullptr};
  sensor::Sensor *dust_case_sensor_{nullptr};
  text_sensor::TextSensor *status_text_sensor_{nullptr};
  text_sensor::TextSensor *error_text_sensor_{nullptr};
  text_sensor::TextSensor *fan_speed_text_sensor_{nullptr};
  text_sensor::TextSensor *clean_mode_text_sensor_{nullptr};
  text_sensor::TextSensor *charge_state_text_sensor_{nullptr};
  text_sensor::TextSensor *firmware_version_text_sensor_{nullptr};
  text_sensor::TextSensor *schedules_text_sensor_{nullptr};
#ifdef USE_TIME
  time::RealTimeClock *time_source_{nullptr};
#endif
};

}  // namespace deebot_n79s
}  // namespace esphome
