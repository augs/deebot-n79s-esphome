#include "deebot_n79s.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif

namespace esphome {
namespace deebot_n79s {

static const char *const TAG = "deebot_n79s";

static const size_t HEADER_LEN = 3;        // "`FX"
static const size_t MAX_FRAME_LEN = 1024;  // DeviceCap is ~490 bytes.
static const uint32_t STARTUP_DELAY_MS = 2000;
// Gap between queued frames so a burst of polls doesn't overrun the MCU.
static const uint32_t TX_SPACING_MS = 100;
static const size_t MAX_TX_QUEUE = 32;
static const size_t MAX_PENDING = 16;
// Clean state reported within this long after a pause doesn't unpause.
static const uint32_t PAUSE_SETTLE_MS = 10000;

uint8_t crc8(const uint8_t *data, size_t len, uint8_t crc) {
  // CRC-8/SMBUS (poly 0x07, init 0x00), used as the UART check byte.
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++)
      crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1;
  }
  return crc;
}

static const uint8_t ESC = 0x5C;

std::string escape(const std::string &data) {
  std::string out;
  out.reserve(data.size() + 4);
  for (char c : data) {
    switch (static_cast<uint8_t>(c)) {
      case 0x5C:
        out += "\x5c\x01";
        break;
      case 0x60:
        out += "\x5c\x02";
        break;
      case 0x0A:
        out += "\x5c\x03";
        break;
      default:
        out.push_back(c);
    }
  }
  return out;
}

bool unescape(const std::string &data, std::string *out) {
  out->clear();
  bool esc = false;
  for (char c : data) {
    if (esc) {
      if (c < 1 || c > 3)
        return false;  // The MCU drops the frame here too.
      out->push_back(static_cast<char>(c == 1 ? 0x5C : c == 2 ? 0x60 : 0x0A));
      esc = false;
    } else if (static_cast<uint8_t>(c) == ESC) {
      esc = true;
    } else {
      out->push_back(c);
    }
  }
  return !esc;
}

std::string frame(const std::string &payload, char channel) {
  std::string body = payload;
  body.push_back('\0');
  body.push_back(static_cast<char>(crc8(reinterpret_cast<const uint8_t *>(body.data()), body.size())));
  std::string out = "`F";
  out.push_back(channel);
  out += escape(body);
  out.push_back('\n');
  return out;
}

bool check_frame(const std::string &msg) {
  if (msg.size() <= HEADER_LEN + 1 || msg[0] != '`' || msg.back() != '\n')
    return false;
  std::string body;
  if (!unescape(msg.substr(HEADER_LEN, msg.size() - HEADER_LEN - 1), &body) || body.size() < 2)
    return false;
  return crc8(reinterpret_cast<const uint8_t *>(body.data()), body.size() - 1) ==
         static_cast<uint8_t>(body.back());
}

bool Element::get(const char *key, std::string *out) const {
  for (const auto &attr : this->attrs) {
    if (attr.first == key) {
      *out = attr.second;
      return true;
    }
  }
  return false;
}

bool Element::has(const char *key) const {
  for (const auto &attr : this->attrs) {
    if (attr.first == key)
      return true;
  }
  return false;
}

static bool is_space(char c) { return isspace(static_cast<unsigned char>(c)) != 0; }

static std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && is_space(s[a]))
    a++;
  while (b > a && is_space(s[b - 1]))
    b--;
  return s.substr(a, b - a);
}

std::vector<Element> parse_elements(const std::string &s) {
  std::vector<Element> out;
  const size_t n = s.size();
  size_t i = 0;
  while ((i = s.find('<', i)) != std::string::npos) {
    i++;
    if (i >= n)
      break;
    if (s[i] == '/' || s[i] == '?' || s[i] == '!') {
      // End tag, declaration or comment.
      i = s.find('>', i);
      if (i == std::string::npos)
        break;
      continue;
    }
    Element el;
    size_t start = i;
    while (i < n && !is_space(s[i]) && s[i] != '/' && s[i] != '>')
      i++;
    el.name = s.substr(start, i - start);
    bool tag_closed = false, self_closing = false;
    while (i < n) {
      char c = s[i];
      if (is_space(c)) {
        i++;
      } else if (c == '/') {
        self_closing = true;
        i++;
      } else if (c == '>') {
        tag_closed = true;
        i++;
        break;
      } else {
        size_t a = i;
        while (i < n && s[i] != '=' && s[i] != '>' && s[i] != '/' && !is_space(s[i]))
          i++;
        std::string key = s.substr(a, i - a);
        while (i < n && is_space(s[i]))
          i++;
        if (i >= n || s[i] != '=')
          continue;  // Valueless attribute.
        i++;
        while (i < n && is_space(s[i]))
          i++;
        if (i >= n || (s[i] != '\'' && s[i] != '"'))
          break;
        char quote = s[i++];
        size_t end = s.find(quote, i);
        if (end == std::string::npos) {
          i = n;
          break;
        }
        el.attrs.emplace_back(key, s.substr(i, end - i));
        i = end + 1;
      }
    }
    if (tag_closed && !self_closing) {
      size_t end = s.find('<', i);
      el.text = trim(s.substr(i, (end == std::string::npos ? n : end) - i));
    }
    out.push_back(std::move(el));
  }
  return out;
}

// Printable copy of a frame for logging.
static std::string printable(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c >= 0x20 && c < 0x7f) {
      out.push_back(c);
    } else {
      out += str_sprintf("\\x%02x", static_cast<uint8_t>(c));
    }
  }
  return out;
}

// printable() with the address (the cloud JID the MCU echoes on channel A/B
// replies) blanked out, so it doesn't end up in logs.
// Same for the Wi-Fi SSID/PSK in s='' / p='' (WIFIStat, SetWIFICfg).
static std::string redacted(const std::string &raw) {
  std::string msg = raw;
  for (const char *key : {" s=", " p="}) {
    size_t pos = 0;
    while ((pos = msg.find(key, pos)) != std::string::npos) {
      size_t q = pos + 3;
      size_t end = q < msg.size() && (msg[q] == '\'' || msg[q] == '"') ? msg.find(msg[q], q + 1) : std::string::npos;
      if (end == std::string::npos) {
        pos = q;
        continue;
      }
      msg.replace(q + 1, end - q - 1, "***");
      pos = q + 5;
    }
  }
  size_t lt = msg.find('<');
  if (lt == std::string::npos || lt <= HEADER_LEN)
    return printable(msg);
  for (size_t i = HEADER_LEN; i < lt; i++) {
    if (msg[i] != '\0')
      return printable(msg.substr(0, HEADER_LEN)) + "<jid>" + printable(msg.substr(lt));
  }
  return printable(msg);
}

static std::string json_escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (static_cast<uint8_t>(c) < 0x20) {
      out += str_sprintf("\\u%04x", static_cast<uint8_t>(c));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// DeviceErr from the app.  100 means no error.
static const char *error_text(int code) {
  switch (code) {
    case 101:
      return "low battery";
    case 102:
      return "host hang";
    case 103:
      return "wheel abnormal";
    case 104:
      return "down sensor";
    case 105:
      return "stuck";
    case 106:
      return "side brush worn";
    case 107:
      return "dust case/filter worn";
    case 108:
      return "side brush abnormal";
    case 109:
      return "roller abnormal";
    case 110:
      return "no dust box";
    case 111:
      return "bumper abnormal";
    case 112:
      return "LDS";
    case 113:
      return "main brush worn";
    case 114:
      return "dust case full";
    case 115:
      return "battery error";
    case 116:
      return "forward-looking sensor";
    case 117:
      return "gyroscope";
    case 118:
      return "strainer blocked";
    case 119:
      return "fan error";
    case 120:
      return "water box";
    case 121:
      return "plate jammed";
    case 201:
      return "air filter missing";
    case 202:
      return "ultrasonic";
    case 203:
      return "small wheel";
    case 204:
      return "wheel hang";
    case 205:
    case 206:
    case 207:
      return "ion sterilizer";
    default:
      return nullptr;
  }
}

static bool one_of(const std::string &v, std::initializer_list<const char *> options) {
  for (const char *opt : options) {
    if (v == opt)
      return true;
  }
  return false;
}

static void publish(text_sensor::TextSensor *sens, const std::string &value) {
  if (sens != nullptr)
    sens->publish_state(value);
}

static void publish(sensor::Sensor *sens, const std::string &value) {
  if (sens != nullptr && !value.empty())
    sens->publish_state(atoi(value.c_str()));
}

static bool wifi_online() {
#ifdef USE_WIFI
  return wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected();
#else
  return true;
#endif
}

void DeebotN79S::setup() {
  this->next_id_ = 10000000 + random_uint32() % 80000000;
  this->publish_status_();
  this->set_error_("None");
  publish(this->clean_mode_text_sensor_, this->clean_mode_);
  this->set_timeout("startup", STARTUP_DELAY_MS, [this]() {
    // This only needs to be sent once to make the mainboard state machine
    // return the correct results for charge and clean status.
    this->send_payload("<ctl td='DCStatus'><DC name='xmpp' on='1' conn='2'/>", 'C');
    this->send_wifi_stat_();
    this->xmpp_announced_ = true;
    this->request_status();
    this->request_supplies();
    this->request_version();
    this->request_schedules();
  });
}

void DeebotN79S::loop() {
  uint8_t c;
  while (this->available() && this->read_byte(&c))
    this->handle_byte_(c);

  if (!this->tx_queue_.empty() && millis() - this->last_tx_ms_ >= TX_SPACING_MS) {
    const std::string &out = this->tx_queue_.front();
    ESP_LOGD(TAG, "TX: %s", printable(out).c_str());
    this->write_array(reinterpret_cast<const uint8_t *>(out.data()), out.size());
    this->tx_queue_.pop_front();
    this->last_tx_ms_ = millis();
  }

  // Tell the MCU as soon as Wi-Fi comes up or drops, not just when it polls.
  if (this->xmpp_announced_ && wifi_online() != this->wifi_reported_)
    this->send_wifi_stat_();
}

void DeebotN79S::update() {
  if (!this->xmpp_announced_)
    return;
  this->request_status();
  this->request_supplies();
}

void DeebotN79S::dump_config() {
  ESP_LOGCONFIG(TAG, "Deebot N79S:");
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "Battery", this->battery_level_sensor_);
  LOG_SENSOR("  ", "Brush", this->brush_sensor_);
  LOG_SENSOR("  ", "Side Brush", this->side_brush_sensor_);
  LOG_SENSOR("  ", "Dust Case", this->dust_case_sensor_);
  LOG_TEXT_SENSOR("  ", "Status", this->status_text_sensor_);
  LOG_TEXT_SENSOR("  ", "Error", this->error_text_sensor_);
  LOG_TEXT_SENSOR("  ", "Firmware", this->firmware_version_text_sensor_);
  LOG_TEXT_SENSOR("  ", "Schedules", this->schedules_text_sensor_);
}

// --- Receive -------------------------------------------------------------

// rx_buf_ holds the header plus the un-escaped body.  The CRC (computed over
// un-escaped bytes) is kept running so a raw '\n' can be checked against the
// residue: 0 over body + check byte means the frame is complete.  With
// escaping a raw '\n' only ever ends a frame, except in the binary tail of
// Log frames, which the MCU doesn't escape.
void DeebotN79S::handle_byte_(uint8_t c) {
  if (this->rx_after_bad_newline_) {
    this->rx_after_bad_newline_ = false;
    if (c == '`') {
      ESP_LOGW(TAG, "Bad check byte on UART message: %s", redacted(this->rx_buf_).c_str());
      this->reset_rx_();
    }
  }

  if (this->rx_buf_.empty()) {
    // Skip noise (e.g. bootloader output) until a frame starts.  '`' is
    // always escaped inside a frame, so a raw one is a frame start.
    if (c == '`') {
      this->rx_buf_.push_back(c);
      this->rx_crc_ = 0;
    }
    return;
  }
  if (this->rx_buf_.size() < HEADER_LEN) {
    this->rx_buf_.push_back(c);
    return;
  }

  if (this->rx_escape_) {
    this->rx_escape_ = false;
    if (c < 1 || c > 3) {
      ESP_LOGW(TAG, "Bad escape 0x5c 0x%02x, dropping: %s", c, redacted(this->rx_buf_).c_str());
      this->reset_rx_();
      return;
    }
    this->rx_append_(c == 1 ? 0x5C : c == 2 ? 0x60 : 0x0A);
    return;
  }
  if (c == ESC) {
    this->rx_escape_ = true;
    return;
  }

  if (c == '\n' && this->rx_buf_.size() > HEADER_LEN + 1 && this->rx_crc_ == 0) {
    this->rx_buf_.push_back(c);
    this->parse_message_(this->rx_buf_);
    this->reset_rx_();
    return;
  }

  this->rx_append_(c);
  if (c == '\n')
    this->rx_after_bad_newline_ = true;
}

void DeebotN79S::rx_append_(uint8_t c) {
  this->rx_buf_.push_back(static_cast<char>(c));
  this->rx_crc_ = crc8(&c, 1, this->rx_crc_);
  if (this->rx_buf_.size() > MAX_FRAME_LEN) {
    ESP_LOGW(TAG, "UART message too long, dropping");
    this->reset_rx_();
  }
}

void DeebotN79S::reset_rx_() {
  this->rx_buf_.clear();
  this->rx_escape_ = false;
  this->rx_after_bad_newline_ = false;
}

void DeebotN79S::parse_message_(const std::string &msg) {
  ESP_LOGD(TAG, "RX: %s", redacted(msg).c_str());
  if (msg.size() < HEADER_LEN + 2 || msg[1] != 'F')
    return;
  // Replies may carry "<jid>\0\0" before the <ctl>; the parser skips it.
  const std::vector<Element> els = parse_elements(msg.substr(HEADER_LEN, msg.size() - HEADER_LEN - 2));
  if (els.empty())
    return;

  // ret first: some replies also carry a td (GetVersion's td='version').
  std::string td;
  if (els[0].has("ret")) {
    this->handle_reply_(els);
  } else if (els[0].get("td", &td)) {
    this->handle_report_(td, els);
  } else {
    ESP_LOGD(TAG, "Unhandled message");
  }
}

void DeebotN79S::handle_report_(const std::string &td, const std::vector<Element> &els) {
  const Element &root = els[0];
  if (td == "GetWIFIStat") {
    // The mainboard polling our Wi-Fi state, which drives its Wi-Fi light.
    std::string id;
    root.get("id", &id);
    this->send_wifi_stat_(id);
    return;
  }
  if (td == "SetWIFICfg") {
    // Most likely the factory Wi-Fi test.  Our Wi-Fi config is compiled in.
    ESP_LOGD(TAG, "Ignoring SetWIFICfg");
    return;
  }
  if (td == "BatteryInfo") {
    for (size_t i = 1; i < els.size(); i++) {
      std::string power;
      if (els[i].name == "battery" && els[i].get("power", &power))
        publish(this->battery_level_sensor_, power);
    }
  } else if (td == "ChargeState") {
    for (size_t i = 1; i < els.size(); i++) {
      if (els[i].name == "charge")
        this->apply_charge_(els[i]);
    }
  } else if (td == "CleanReport") {
    for (size_t i = 1; i < els.size(); i++) {
      if (els[i].name == "clean")
        this->apply_clean_(els[i], true);
    }
  } else if (td == "LifeSpan") {
    this->apply_lifespan_(root);
  } else if (td == "Sched2") {
    this->apply_schedules_(els);
  } else if (td == "error") {
    this->apply_error_(root);
  } else if (td == "version") {
    this->apply_version_(els);
  } else if (td == "FactoryRest") {
    // Factory reset requested on the robot.  Ack it like the stock firmware;
    // our Wi-Fi config is compiled in, so there is nothing to clear.
    std::string id;
    root.get("id", &id);
    ESP_LOGW(TAG, "Robot requested a factory reset, acknowledging");
    this->send_payload("<ctl id='" + id + "' ret='ok'/>", 'C');
  } else if (td == "PowerOff") {
    // In the app's protocol, but this MCU never sends it.
    ESP_LOGW(TAG, "Robot powering off");
    this->powered_off_ = true;
  } else {
    ESP_LOGD(TAG, "Unhandled report td='%s'", td.c_str());
  }
  this->publish_status_();
}

void DeebotN79S::handle_reply_(const std::vector<Element> &els) {
  const Element &root = els[0];
  std::string id_str, ret;
  root.get("id", &id_str);
  root.get("ret", &ret);

  Pending req{0, "?", Request::OTHER, ""};
  bool matched = false;
  if (!id_str.empty()) {
    uint32_t id = strtoul(id_str.c_str(), nullptr, 10);
    for (auto it = this->pending_.begin(); it != this->pending_.end(); ++it) {
      if (it->id == id) {
        req = *it;
        this->pending_.erase(it);
        matched = true;
        break;
      }
    }
  }

  if (ret != "ok") {
    std::string err;
    root.get("errno", &err);
    ESP_LOGW(TAG, "%s (id %s) failed: ret='%s' errno='%s'", req.td, id_str.c_str(), ret.c_str(), err.c_str());
    return;
  }

  // Follow-ups to read back what a command changed.
  switch (req.kind) {
    case Request::PAUSE:
      // The MCU never reports paused (GetCleanState st is only s/h), so
      // remember it until a clean starts or the robot docks.
      this->paused_ = true;
      this->paused_since_ms_ = millis();
      this->clean_st_ = 'h';
      this->send_ctl_("GetCleanState");
      break;
    case Request::CLEAN:
      this->send_ctl_("GetCleanState");
      this->send_ctl_("GetChargeState");
      break;
    case Request::SET_SPEED:
      this->send_ctl_("GetCleanSpeed");
      break;
    case Request::SET_LIFESPAN:
      this->send_ctl_("GetLifeSpan", " type='" + req.arg + "'");
      break;
    case Request::SCHED_CHANGE:
      this->request_schedules();
      break;
    default:
      break;
  }

  // The reply shapes are distinct, so parse by structure.  Children of a
  // reply are state, never reports: GetCleanState's <clean type='stop'
  // st='h'/> looks like a CleanReport to a naive parser.
  bool has_schedules = req.kind == Request::GET_SCHED;
  for (size_t i = 1; i < els.size(); i++) {
    const Element &el = els[i];
    std::string value;
    if (el.name == "battery" && el.get("power", &value)) {
      publish(this->battery_level_sensor_, value);
    } else if (el.name == "charge") {
      this->apply_charge_(el);
    } else if (el.name == "clean") {
      this->apply_clean_(el, false);
    } else if (el.name == "s" && el.has("n")) {
      has_schedules = true;
    }
  }
  if (has_schedules)
    this->apply_schedules_(els);
  const bool found_fw = this->apply_version_(els);
  if (matched && !found_fw && strcmp(req.td, "GetVersion") == 0)
    ESP_LOGW(TAG, "GetVersion reply had no <ver name='FW'> child");
  if (root.has("type") && root.has("val"))
    this->apply_lifespan_(root);
  std::string speed;
  if (root.get("speed", &speed))
    publish(this->fan_speed_text_sensor_, speed);

  this->publish_status_();
}

bool DeebotN79S::apply_version_(const std::vector<Element> &els) {
  // <ctl ... td='version'><ver name='FW'>VERSION</ver></ctl>
  for (size_t i = 1; i < els.size(); i++) {
    std::string name;
    if (els[i].get("name", &name) && name == "FW") {
      publish(this->firmware_version_text_sensor_, els[i].text);
      return true;
    }
  }
  return false;
}

void DeebotN79S::apply_clean_(const Element &clean, bool /*report*/) {
  std::string type, st, speed;
  clean.get("type", &type);
  clean.get("st", &st);
  st = trim(st);
  // CleanReport always has st=' ' on this MCU, so the type decides there;
  // the GetCleanState reply has st='s' or 'h'.
  char status = (st == "s" || st == "h") ? st[0] : (type == "stop" ? 'h' : 's');

  if (!type.empty() && type != "stop" && type != this->clean_mode_) {
    this->clean_mode_ = type;
    publish(this->clean_mode_text_sensor_, type);
  }
  if (clean.get("speed", &speed))
    publish(this->fan_speed_text_sensor_, speed);

  this->clean_st_ = status;
  this->powered_off_ = false;
  if (status == 's') {
    // Cleaning: unpaused (once the pause has had time to settle), the error
    // is gone and we're off the dock.
    if (this->paused_ && millis() - this->paused_since_ms_ > PAUSE_SETTLE_MS)
      this->paused_ = false;
    if (this->error_active_)
      this->set_error_("None");
    if (this->charge_state_ != "going")
      this->charge_state_ = "Idle";
  }
}

void DeebotN79S::apply_charge_(const Element &charge) {
  std::string type;
  if (!charge.get("type", &type))
    return;
  // The GetChargeState reply says 'Going', reports say "going".
  if (str_equals_case_insensitive(type, "going"))
    type = "going";
  this->charge_state_ = type;
  this->powered_off_ = false;
  publish(this->charge_state_text_sensor_, type);
  if (type == "going" || type == "SlotCharging" || type == "WireCharging")
    this->paused_ = false;
  if (type == "SlotCharging" || type == "WireCharging") {
    this->clean_st_ = 'h';
    if (this->error_active_)
      this->set_error_("None");
  }
}

void DeebotN79S::apply_lifespan_(const Element &el) {
  // total= is always 365 on this MCU, so only val (% left) is used.
  std::string type, val;
  el.get("type", &type);
  el.get("val", &val);
  if (type == "Brush") {
    publish(this->brush_sensor_, val);
  } else if (type == "SideBrush") {
    publish(this->side_brush_sensor_, val);
  } else if (type == "DustCaseHeap") {
    publish(this->dust_case_sensor_, val);
  }
}

void DeebotN79S::apply_error_(const Element &root) {
  std::string codes, action;
  root.get("errno", &codes);
  // An action error carries action= and error= and maps errno 5 to 103.
  const bool action_error = root.get("action", &action) && root.has("error");

  std::string text;
  bool stopped = false;
  size_t pos = 0;
  while (pos <= codes.size()) {
    size_t comma = codes.find(',', pos);
    std::string item = trim(codes.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
    pos = comma == std::string::npos ? codes.size() + 1 : comma + 1;
    if (item.empty())
      continue;
    int code = atoi(item.c_str());
    if (action_error && code == 5)
      code = 103;
    if (code == 100) {
      // No error; sent when the robot times out idle off the dock.
      stopped = true;
      continue;
    }
    const char *name = error_text(code);
    if (!text.empty())
      text += ", ";
    text += name != nullptr ? name : str_sprintf("unknown error code: %d", code);
  }

  if (stopped)
    this->clean_st_ = 'h';
  if (text.empty()) {
    if (this->error_active_)
      ESP_LOGI(TAG, "Error cleared");
    this->set_error_("None");
    return;
  }
  if (action_error)
    ESP_LOGW(TAG, "Action '%s' failed: %s (errno '%s')", action.c_str(), text.c_str(), codes.c_str());
  else
    ESP_LOGW(TAG, "Vacuum error: %s (errno '%s')", text.c_str(), codes.c_str());
  this->error_active_ = true;
  publish(this->error_text_sensor_, text);
}

void DeebotN79S::apply_schedules_(const std::vector<Element> &els) {
  this->schedules_.clear();
  for (size_t i = 1; i < els.size(); i++) {
    const Element &el = els[i];
    Schedule sched;
    std::string on;
    if (!el.get("n", &sched.name))
      continue;
    el.get("o", &on);
    el.get("t", &sched.time);
    el.get("r", &sched.days);
    el.get("f", &sched.source);
    sched.enabled = on == "1";
    this->schedules_.push_back(sched);
  }

  std::string json = "[";
  for (const auto &sched : this->schedules_) {
    if (json.size() > 1)
      json += ",";
    json += "{\"name\":\"" + json_escape(sched.name) + "\",\"on\":" + (sched.enabled ? "true" : "false") +
            ",\"time\":\"" + json_escape(sched.time) + "\",\"days\":\"" + json_escape(sched.days) +
            "\",\"src\":\"" + json_escape(sched.source) + "\"}";
  }
  json += "]";
  ESP_LOGI(TAG, "Schedules: %s", json.c_str());
  if (json.size() > 255)
    ESP_LOGW(TAG, "Schedule list is %u chars; Home Assistant truncates states at 255",
             static_cast<unsigned>(json.size()));
  publish(this->schedules_text_sensor_, json);
}

void DeebotN79S::set_error_(const std::string &error) {
  this->error_active_ = error != "None";
  publish(this->error_text_sensor_, error);
}

void DeebotN79S::publish_status_() {
  const char *status;
  if (this->powered_off_) {
    status = "off";
  } else if (this->error_active_) {
    status = "error";
  } else if (this->charge_state_ == "going") {
    status = "returning";
  } else if (this->charge_state_ == "SlotCharging" || this->charge_state_ == "WireCharging") {
    status = "docked";
  } else if (this->paused_) {
    status = "paused";
  } else if (this->clean_st_ == 's') {
    status = "cleaning";
  } else {
    status = "idle";
  }
  if (this->last_status_ == status)
    return;
  this->last_status_ = status;
  publish(this->status_text_sensor_, status);
}

// --- Transmit ------------------------------------------------------------

void DeebotN79S::send_payload(const std::string &payload, char channel) {
  if (this->tx_queue_.size() >= MAX_TX_QUEUE) {
    ESP_LOGW(TAG, "TX queue full, dropping: %s", printable(payload).c_str());
    return;
  }
  this->tx_queue_.push_back(frame(payload, channel));
}

void DeebotN79S::send_wifi_stat_(const std::string &id) {
  // st='o' online, 'c' connecting.  The stock firmware also sends the SSID
  // and PSK in s='' / p=''; the MCU doesn't need them.
  const bool online = wifi_online();
  this->wifi_reported_ = online;
  std::string payload = std::string("<ctl td='WIFIStat' st='") + (online ? "o" : "c") + "'";
  if (!id.empty())
    payload += " id='" + id + "'";
  this->send_payload(payload + "/>", 'C');
}

void DeebotN79S::send_ctl_(const char *td, const std::string &attrs, const std::string &children, Request kind,
                           const std::string &arg) {
  const uint32_t id = this->next_id_++;
  if (this->next_id_ > 99999999)
    this->next_id_ = 10000000;
  std::string payload = str_sprintf("<ctl id='%u' td='%s'", static_cast<unsigned>(id), td) + attrs;
  payload += children.empty() ? "/>" : ">" + children + "</ctl>";
  this->send_payload(payload);

  if (this->pending_.size() >= MAX_PENDING)
    this->pending_.pop_front();
  this->pending_.push_back(Pending{id, td, kind, arg});
}

void DeebotN79S::send_clean_(const std::string &type, const char *act) {
  const bool pause = strcmp(act, "p") == 0;
  if (!pause)
    this->paused_ = false;
  this->send_ctl_("Clean", "", "<clean type='" + type + "' act='" + act + "'/>",
                  pause ? Request::PAUSE : Request::CLEAN);
}

// The MCU has no act='r': resuming a paused clean is starting it again.
void DeebotN79S::start() { this->clean(); }

void DeebotN79S::clean() { this->clean(this->clean_mode_); }

void DeebotN79S::clean(const std::string &type) {
  if (!one_of(type, {"auto", "border", "spot", "singleRoom"})) {
    ESP_LOGW(TAG, "Unknown clean type '%s'", type.c_str());
    return;
  }
  this->send_clean_(type, "s");
}

void DeebotN79S::pause() { this->send_clean_(this->clean_mode_, "p"); }
void DeebotN79S::resume() { this->clean(); }
// Known to work without act='h'.
void DeebotN79S::stop() {
  this->paused_ = false;
  this->send_ctl_("Clean", "", "<clean type='stop'/>", Request::CLEAN);
}
void DeebotN79S::charge() {
  this->paused_ = false;
  this->send_ctl_("Charge", "", "<charge type='go'/>", Request::CLEAN);
}
void DeebotN79S::cancel_charge() { this->send_ctl_("Charge", "", "<charge type='stopGo'/>", Request::CLEAN); }
void DeebotN79S::locate() { this->send_ctl_("PlaySound", " sid='0'"); }

void DeebotN79S::move(const std::string &action) {
  // The MCU has no backward or brake.
  if (!one_of(action, {"forward", "SpinLeft", "SpinRight", "TurnAround", "stop"})) {
    ESP_LOGW(TAG, "Unknown move action '%s'", action.c_str());
    return;
  }
  // Fire and forget: the app sends Move without an id and gets no reply.
  this->send_payload("<ctl td='Move'><move action='" + action + "'/></ctl>");
}

void DeebotN79S::set_clean_mode(const std::string &mode) {
  if (!one_of(mode, {"auto", "border", "spot", "singleRoom"})) {
    ESP_LOGW(TAG, "Unknown clean mode '%s'", mode.c_str());
    return;
  }
  this->clean_mode_ = mode;
  publish(this->clean_mode_text_sensor_, mode);
}

void DeebotN79S::set_fan_speed(const std::string &speed) {
  if (!one_of(speed, {"standard", "strong"})) {
    ESP_LOGW(TAG, "Unknown fan speed '%s'", speed.c_str());
    return;
  }
  this->send_ctl_("SetCleanSpeed", " speed='" + speed + "'", "", Request::SET_SPEED);
}

void DeebotN79S::reset_lifespan(const std::string &component) {
  if (!one_of(component, {"Brush", "SideBrush", "DustCaseHeap"})) {
    ESP_LOGW(TAG, "Unknown consumable '%s'", component.c_str());
    return;
  }
  this->send_ctl_("SetLifeSpan", " type='" + component + "' val='100'", "", Request::SET_LIFESPAN, component);
}

void DeebotN79S::set_time(int64_t t, int tz_minutes) {
  // td before id, as in the stock firmware's SetTime.
  const uint32_t id = this->next_id_++;
  this->send_payload(str_sprintf("<ctl td='SetTime' id='%u'><time t='%lu' tz='%d' tzm='%d'/></ctl>",
                                 static_cast<unsigned>(id), static_cast<unsigned long>(t), tz_minutes / 60,
                                 abs(tz_minutes % 60)));
}

#ifdef USE_TIME
void DeebotN79S::set_time() {
  if (this->time_source_ == nullptr) {
    ESP_LOGW(TAG, "set_time: no time_id configured");
    return;
  }
  ESPTime now = this->time_source_->utcnow();
  if (!now.is_valid()) {
    ESP_LOGW(TAG, "set_time: clock not synced yet");
    return;
  }
  this->set_time(now.timestamp, ESPTime::timezone_offset() / 60);
}
#endif

static bool valid_sched_name(const std::string &name) {
  if (name.empty() || name.size() > 32)
    return false;
  for (char c : name) {
    if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.')
      return false;
  }
  return true;
}

bool DeebotN79S::sched_xml_(const std::string &name, int hour, int minute, const std::string &days, bool enabled,
                            std::string *out) {
  if (!valid_sched_name(name)) {
    ESP_LOGW(TAG, "Schedule name '%s' must be 1-32 chars of [A-Za-z0-9_.-]", name.c_str());
    return false;
  }
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
    ESP_LOGW(TAG, "Schedule time %d:%d out of range", hour, minute);
    return false;
  }
  if (days.size() != 7 || days.find_first_not_of("01") != std::string::npos) {
    ESP_LOGW(TAG, "Schedule days '%s' must be 7 x 0/1, Sunday first", days.c_str());
    return false;
  }
  *out = str_sprintf("<sched name='%s' on='%d' time='%02d:%02d' repeat='%s'>", name.c_str(), enabled ? 1 : 0, hour,
                     minute, days.c_str()) +
         "<ctl td='Clean'><clean type='auto'/></ctl></sched>";
  return true;
}

void DeebotN79S::add_schedule(const std::string &name, int hour, int minute, const std::string &days,
                              bool enabled) {
  // The app names schedules after the current time plus a random number.
  const std::string sched_name = name.empty() ? str_sprintf("%u", static_cast<unsigned>(random_uint32() % 1000000000))
                                              : name;
  std::string sched;
  if (this->sched_xml_(sched_name, hour, minute, days, enabled, &sched))
    this->send_ctl_("AddSched", "", sched, Request::SCHED_CHANGE);
}

void DeebotN79S::modify_schedule(const std::string &name, int hour, int minute, const std::string &days,
                                 bool enabled) {
  std::string sched;
  if (this->sched_xml_(name, hour, minute, days, enabled, &sched))
    this->send_ctl_("ModSched", "", "<ModSched name='" + name + "'>" + sched + "</ModSched>", Request::SCHED_CHANGE);
}

void DeebotN79S::delete_schedule(const std::string &name) {
  if (!valid_sched_name(name)) {
    ESP_LOGW(TAG, "Schedule name '%s' must be 1-32 chars of [A-Za-z0-9_.-]", name.c_str());
    return;
  }
  this->send_ctl_("DelSched", "", "<DelSched name='" + name + "'/>", Request::SCHED_CHANGE);
}

void DeebotN79S::request_schedules() { this->send_ctl_("GetSched", "", "", Request::GET_SCHED); }

void DeebotN79S::request_status() {
  this->send_ctl_("GetBatteryInfo");
  this->send_ctl_("GetChargeState");
  // Safe now that replies are matched by id and decided on st.
  this->send_ctl_("GetCleanState");
}

void DeebotN79S::request_supplies() {
  this->send_ctl_("GetLifeSpan", " type='SideBrush'");
  this->send_ctl_("GetLifeSpan", " type='Brush'");
  this->send_ctl_("GetLifeSpan", " type='DustCaseHeap'");
  this->send_ctl_("GetCleanSpeed");
}

void DeebotN79S::request_version() { this->send_ctl_("GetVersion", " name='FW'"); }

}  // namespace deebot_n79s
}  // namespace esphome
