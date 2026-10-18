# Deebot N79S on ESPHome

This is an ESPHome replacement for the stock firmware on the Wi-Fi board of an
**Ecovacs Deebot N79S** robot vacuum. With it the robot works locally with
Home Assistant through the ESPHome native API, with no Ecovacs cloud.

The robot's mainboard MCU (an STM32F071) does all the work. The Wi-Fi board
(an ESP8266-WROOM-02) just relays XML commands to it over a UART. This
project speaks that UART protocol, which is documented in full in
[PROTOCOL.md](PROTOCOL.md), including the framing, CRC-8, escaping, every
command and report, and the quirks.

| Path | What |
|---|---|
| `components/deebot_n79s/` | ESPHome external component (the protocol) |
| `deebot-n79s.yaml` | Example device config |
| `secrets.example.yaml` | The secrets it uses |
| `homeassistant/deebot_n79s.yaml` | Home Assistant package: a `vacuum` entity wrapping the ESPHome entities |
| `PROTOCOL.md` | Mainboard UART protocol reference |
| `tools/n79s_frame.py` | Python reference of the framing, with test vectors |

## Hardware

- **Wi-Fi board:** an ESP8266-WROOM-02 module with 4 MB flash, on a small
  carrier board. It plugs into the mainboard's 4-pin **J5** connector, which
  carries 3.3 V, GND, TX and RX.
- **The UART:** the ESP8266's UART0 (GPIO1 TX, GPIO3 RX) is wired to the MCU's
  USART1 at **115200 8N1**. Because UART0 belongs to the mainboard, ESPHome
  logs go to UART1 (GPIO2, TX only) and over Wi-Fi.
- **Board choice:** the config uses `board: esp12e`. ESPHome's
  `esp_wroom_02` board assumes 2 MB of flash, but this module has 4 MB, and
  the chip is the same ESP8266EX.

## Install

1. **Copy the config.** Copy `deebot-n79s.yaml` into your ESPHome config
   directory. Copy `secrets.example.yaml` there too and fill it in as
   `secrets.yaml`, or add the keys to your existing one. The config pulls
   the component from this repo (`github://augs/deebot-n79s-esphome@main`).
2. **Back up the stock firmware.** Do this before anything else; it's the
   only way back to the Ecovacs app:
   ```bash
   esptool --port /dev/ttyUSB0 --baud 115200 read-flash 0 0x400000 deebot-wifi-backup.img
   ```
3. **Flash over serial.** The first flash has to be over serial:
   1. Unplug the module from J5, wire it to a 3.3 V USB-serial adapter, and
      hold GPIO0 low at reset to enter download mode.
   2. Run `esphome run deebot-n79s.yaml --device /dev/ttyUSB0`, or write the
      factory image with esptool.
   3. If reads or writes come back corrupt, use 115200 baud, as with the
      backup.
   4. If your adapter's auto-reset drops download mode between commands, add
      `--before no-reset --after no-reset`.

   Some adapters can't supply enough current for the ESP8266's Wi-Fi. If
   the module never joins your network while on the adapter, plug it back
   into the robot to test.
4. **Reconnect.** Plug the module back into J5 and power on the robot. Later
   updates go over the air.

To go back to stock, write the backup with `esptool write-flash 0 deebot-wifi-backup.img`.

## Home Assistant

1. **Add the device.** Add it under Settings → Devices & services → ESPHome,
   using the `api_key` from your secrets. It's usually discovered
   automatically.
2. **Add the vacuum entity.** Copy `homeassistant/deebot_n79s.yaml` into your
   packages directory, with `homeassistant: packages: !include_dir_named packages`
   in `configuration.yaml`. Then reload template entities (Developer tools →
   YAML) or restart HA. This creates `vacuum.deebot_n79s` with start, pause,
   stop, return to base, spot clean, locate and fan speed.
3. **Time.** It comes from Home Assistant, timezone included. Every sync also
   sets the robot's clock, which on-robot schedules run on.

Home Assistant vacuum entities no longer carry a battery level, so the
battery stays its own sensor, and the package copies it into an attribute.

## Entities

| ESPHome entity | Source | Notes |
|---|---|---|
| `sensor.deebot_n79s_battery` | `BatteryInfo` / `GetBatteryInfo` | % |
| `sensor.deebot_n79s_main_brush_life` | `LifeSpan type='Brush'` | % |
| `sensor.deebot_n79s_side_brush_life` | `LifeSpan type='SideBrush'` | % |
| `sensor.deebot_n79s_filter_life` | `LifeSpan type='DustCaseHeap'` | % |
| `sensor.deebot_n79s_status` | clean state, charge state, error, tracked pause | cleaning, paused, returning, docked, idle, error |
| `sensor.deebot_n79s_error` | `td='error'` `errno` | error text, `None` when clear |
| `sensor.deebot_n79s_charge_state` | `ChargeState` / `GetChargeState` | going, SlotCharging, WireCharging, Idle |
| `sensor.deebot_n79s_firmware` | `GetVersion name='FW'` | read at boot |
| `sensor.deebot_n79s_schedules` | `GetSched` / `Sched2` | JSON list, see below |
| `select.deebot_n79s_suction_speed` | `speed` | standard / strong |
| `select.deebot_n79s_clean_mode` | clean `type` | auto / border / spot / singleRoom, used by Clean |
| `button.deebot_n79s_clean` | `Clean type=<mode> act='s'` | also resumes a pause (the MCU has no resume) |
| `button.deebot_n79s_pause` | `Clean act='p'` | |
| `button.deebot_n79s_spot_clean` | `Clean type='spot'` | |
| `button.deebot_n79s_stop` | `Clean type='stop'` | |
| `button.deebot_n79s_return_to_dock` | `Charge type='go'` | |
| `button.deebot_n79s_cancel_return` | `Charge type='stopGo'` | |
| `button.deebot_n79s_locate` | `PlaySound sid='0'` | beeps |
| `button.deebot_n79s_move_forward`, `_spin_left`, `_spin_right`, `_turn_around`, `_move_stop` | `Move action=` | no reply. The MCU has no backward |
| `button.deebot_n79s_reset_*` | `SetLifeSpan val='100'` | after replacing a part |
| `button.deebot_n79s_sync_time` | `SetTime` | also sent on every time sync |
| `button.deebot_n79s_refresh` | all polls, version, schedules | |

## Schedules

The robot stores its own cleaning schedules. They're published as compact
JSON:

```json
[{"name":"weekdays","on":true,"time":"09:30","days":"0111110","src":"p"}]
```

- **`days`** is 7 characters of 0/1, Sunday first.
- **`src`** is `p` (app), `q` (robot) or `i` (IR remote).
- **Long lists:** Home Assistant truncates states at 255 characters, which is
  about four schedules. The full list is always in the device log.

Home Assistant actions:

| Action | Data |
|---|---|
| `esphome.deebot_n79s_add_schedule` | `name`, `hour`, `minute`, `days`, `enabled` (an empty name generates one) |
| `esphome.deebot_n79s_modify_schedule` | same, where `name` picks the schedule |
| `esphome.deebot_n79s_delete_schedule` | `name` |
| `esphome.deebot_n79s_refresh_schedules` | |

Names are limited to `[A-Za-z0-9_.-]`, 1–32 characters. Scheduled cleans
are always `auto` at the current suction speed, as in the app.

## How it works

- **Request ids.** Every command except `Move` carries `id='<n>'`, and its
  reply is matched by that id. Failures are logged with the command name,
  and changes are read back from the robot.
- **Sending.** Frames are queued and sent 100 ms apart. Attributes are always
  single-quoted, because the MCU matches literal substrings such as
  `act='p'`.
- **Cleaning vs. stopped** comes from two places: the `type` in `CleanReport`
  (whose `st` and `rsn` are always blank on this MCU), and the `GetCleanState`
  reply, whose `st='s'` only means cleaning when `type` isn't `stop`.
- **Paused** is tracked by the component, because the MCU never reports it.
- **Status priority:** error > returning > docked > paused > cleaning > idle.
- **Boot.** At boot the component sends `DCStatus conn='2'`. Without it, the
  MCU's status reports are unreliable. Status, consumables, version and
  schedules follow, and are re-polled every 5 minutes.
- **Ignored messages.** `GetWIFIStat` and `SetWIFICfg` from the mainboard
  are ignored. `FactoryRest` is acknowledged.
- **`GetLogs` is never sent:** a second call makes the mainboard reset the
  ESP8266.
- **Log redaction.** The RX log blanks out cloud JIDs and any SSID or
  password values.

## Status

Tested on a real robot:

- the link handshake;
- battery, consumables, firmware version and the schedule list;
- Locate (`PlaySound`);
- return to dock, with status going returning → docked and `SlotCharging`
  arriving.

Taken from the app and the MCU firmware but not yet run on hardware:

- pause (`act='p'`), cancel return (`stopGo`);
- adding, changing and deleting schedules;
- an escaped check byte on the wire.

Reports and fixes are welcome.

## License

MIT. See [LICENSE](LICENSE). This project isn't affiliated with Ecovacs.
"Deebot" and "Ecovacs" are their trademarks.
