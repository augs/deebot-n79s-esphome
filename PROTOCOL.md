# Deebot N79S mainboard UART protocol

This is the complete reference for talking to the Ecovacs Deebot N79S mainboard
MCU from the Wi-Fi module's UART. It's written so an agent or developer can
implement a client from this file alone. Each fact is tagged
with how it was established:

- **[mcu]**: read from the mainboard firmware itself
  (a flash dump of firmware `V703_513_214_180706`, disassembled; the dump
  itself is not distributed). This is the
  strongest evidence, since it's the code that parses your frames.
  Addresses are given so you can re-check them.
- **[verified]**: CRC-checked against captured traffic, or exercised on the
  robot.
- **[app]**: read from the Android app's protocol code (`DN79SProtocol`). It
  shows what the cloud sends, but the app is shared with other models, so
  where it disagrees with **[mcu]**, **[mcu]** wins.
- **[inferred]**: a reasonable guess. Confirm it before relying on it.

## 1. Hardware and physical layer

| Item | Value |
|---|---|
| Robot | Ecovacs Deebot N79S, device class `155` (`ECO_DEVICE_DN79S`) |
| Mainboard | `DN622-A V01_01`, MCU STM32F071VBT6, RDP level 1 (a dump exists, see above) |
| MCU firmware | `V703_513_214_180706`. STM32Cube HAL, image at `0x08000000`, 0x1F818 bytes used |
| Wi-Fi module | ESP8266-WROOM-02, 4 MB flash, on a small carrier board |
| Link | Mainboard J5 (4-pin JST) = MCU **USART1** ↔ ESP8266 UART0 (TX GPIO1, RX GPIO3), 3.3 V logic |
| Serial format | **115200 baud, 8N1, no flow control, 16× oversampling** **[mcu]**: the `MX_USART1_UART_Init` code at `0x0800b9d0` sets `BaudRate = 0xE1<<9`, `WordLength 8`, `StopBits 1`, `Parity none`, `Mode TX/RX`, `HwFlowCtl none` |
| Old baud value | Earlier code for this robot used `119200`, which is 3.5% fast. It happened to work, but use 115200 |
| Debug header J14 | MCU USART3, also 115200 8N1 **[mcu]**. It carries a separate factory-test protocol (`@400=…*`, `@404=%4d,…*`, `@456%s*`). That's out of scope here |
| Other UART0 traffic | The ESP8266 boot ROM prints at ~74880 baud on UART0 TX at reset. It has caused no problems in practice. Never log application output on UART0: use UART1 (GPIO2, TX only) |

## 2. Frame format

Every message in both directions is one frame:

```
'`'  'F'  CH  escape( ADDR  PAYLOAD  0x00  CRC )  '\n'
 60   46   ..
```

| Field | Size | Meaning |
|---|---|---|
| `` ` `` `F` | 2 | Start marker, always 0x60 0x46 |
| `CH` | 1 | Channel, see §2.1 |
| `ADDR` | 0+ | Channels `A` and `B` (see §2.2; MCU→ESP frames carry at least `\x00\x00`). Absent on `C` |
| `PAYLOAD` | n | One XML element `<ctl .../>`. ASCII, except `Log` frames, which carry binary bytes after the XML |
| `0x00` | 1 | Null terminator. Optional on frames you send |
| `CRC` | 1 | CRC-8, see §3 |
| `\n` | 1 | 0x0A end of frame |

### 2.1 Channels

| CH | Direction | Use |
|---|---|---|
| `A` | ESP→MCU: cloud command relay. MCU→ESP: the reply to it | App commands (`Clean`, `GetBatteryInfo`, ...) and their `ret=` replies |
| `B` | MCU→ESP only | Unsolicited status reports (`BatteryInfo`, `CleanReport`, `error`, ...) |
| `C` | both | Link and housekeeping between the ESP and the MCU (`GetDeviceCap`, `WIFIStat`, `DCStatus`, ...) |

### 2.2 Address prefix on channel A (and B)

The stock firmware puts the requesting XMPP JID (the app user's
`<user>@ecouser.net/<resource>`) between the header and the XML, so the reply
can be routed back to that user:

| Direction | Bytes after `` `FA`` / `` `FB`` | Seen as |
|---|---|---|
| ESP→MCU (stock) | `\x00` JID `\x00` | `` `FA\x00<jid>\x00<ctl id='...' td='Clean'>...`` **[verified]** |
| ESP→MCU (minimal) | nothing | `` `FA<ctl td='GetBatteryInfo'/>`` is accepted **[verified]** |
| MCU→ESP | JID `\x00\x00` | `` `FA<jid>\x00\x00<ctl id='...' ret='ok'/>`` **[verified]** |
| MCU→ESP (no JID known) | `\x00\x00` | `` `FA\x00\x00<ctl id='' ret='ok'/>`` and `` `FB\x00\x00<ctl td='BatteryInfo'>...`` **[verified]** |

The MCU remembers the last JID it saw and echoes it on later replies, even
replies to frames that had no address **[verified]**. When parsing, find the
first `<` and treat everything between `` `FX`` and it as the address.

Channel `C` frames have no address: `` `FC<ctl ...>\x00<crc>\n`` **[verified]**.

### 2.3 Byte escaping **[mcu]**

Three byte values may not appear raw after the 3-byte header, because they
are the frame delimiters and the escape character. They're sent as two
bytes:

| Raw byte | Sent as |
|---|---|
| `0x5C` `\` | `5C 01` |
| `0x60` `` ` `` | `5C 02` |
| `0x0A` `\n` | `5C 03` |

- **MCU receive** (`0x08018870`): it un-escapes the whole frame body before
  checking the CRC. A `\` followed by anything other than `01`/`02`/`03`
  makes it **drop the frame** (the function returns -1). The CRC is computed
  over the *un-escaped* bytes.
- **MCU transmit** (`0x080132ac`): it only escapes the check byte, because
  its XML never contains these three characters. **[verified]** on
  hardware: a `SetTime` reply whose CRC is `0x0A` arrived as `... 00 5C 03 0A`
  and validated after un-escaping.
- **What a client must do:**
  - Escape at least the check byte on everything you send. Escaping every
    byte after the header is equally valid.
  - Un-escape received frames before checking the CRC.
- **Why it matters:** about 1.2% of frames have a check byte of `0x5C`,
  `0x60` or `0x0A`. Sent raw, those are silently dropped or cut short.
- **Retired workaround:** an older "drop the null if the CRC is `\n`" trick
  is superseded by this. It still sent raw `\` and `` ` `` check bytes.

## 3. Check byte (CRC-8/SMBUS) **[mcu]** **[verified]**

- **Algorithm:** CRC-8, poly `0x07`, init `0x00`, no input/output
  reflection, xorout `0x00`. This is the catalog's CRC-8/SMBUS
  (check("123456789") = `0xF4`).
- **Coverage:** every un-escaped byte after the 3-byte `` `FX`` header, up to
  and including the null terminator: `ADDR + PAYLOAD + 0x00`.
- **In the MCU:**
  - `0x08018290` builds a 256-entry table in RAM at `0x20000fc4`, from poly
    `0x07`, on first use.
  - `0x08017860` is `crc8_update(&crc, b)`: `crc = table[crc ^ b]`.
  - `0x08017496` is `crc8(buf, len)`, starting from `0` with no final XOR.
  - Receive (`0x08014750`): `crc8(rx + 3, len - 5) == rx[len - 2]` on the
    un-escaped buffer `rx` (`0x2000143b`), i.e. everything between the
    header and the check byte.
  - Transmit: `crc8(payload, len + 1)`, which covers the payload plus its
    null terminator.
- **Captures:** this matches all 19 real captured frames in both directions,
  including a 490-byte `DeviceCap` and binary `Log` frames, plus 27 check
  bytes found earlier by brute force.

```python
ESC = {0x5C: 1, 0x60: 2, 0x0A: 3}
UNESC = {v: k for k, v in ESC.items()}

def crc8(data, crc=0):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc

def escape(data):
    out = bytearray()
    for b in data:
        out += bytes((0x5C, ESC[b])) if b in ESC else bytes((b,))
    return bytes(out)

def unescape(data):                     # None on a bad escape, like the MCU
    out, esc = bytearray(), False
    for b in data:
        if esc:
            if b not in UNESC: return None
            out.append(UNESC[b]); esc = False
        elif b == 0x5C: esc = True
        else: out.append(b)
    return bytes(out)

def frame(payload: bytes, channel=b"A") -> bytes:
    body = payload + b"\x00"
    return b"`F" + channel + escape(body + bytes((crc8(body),))) + b"\n"

def check_frame(f: bytes) -> bool:      # f = one frame including '`F?' and '\n'
    body = unescape(f[3:-1])
    return body is not None and len(body) > 1 and crc8(body[:-1]) == body[-1]
```

```c
uint8_t crc8(const uint8_t *p, size_t n, uint8_t crc) {
  while (n--) { crc ^= *p++; for (int i = 0; i < 8; i++) crc = crc & 0x80 ? (crc << 1) ^ 0x07 : crc << 1; }
  return crc;
}
```

Properties worth knowing:

- **The null is optional when sending.** The MCU checks whatever sits
  between the header and the check byte, so the frame is accepted with or
  without the trailing `0x00`, as long as the CRC matches what was sent
  **[mcu]** **[verified]**: `` `FC<ctl td='GetDeviceCap'/>`` with `\x00\x19`
  and with `\xB0` both work. The MCU always sends the null.
- **Splitting on `\n`.** Thanks to escaping, a raw `0x0A` only ever ends a
  frame. The exception is `Log` frames: they carry an unescaped binary tail
  (`\xa5\x01\x08Q\xe4`), which looks like a fixed record, so they could in
  principle contain one **[inferred]**. Splitting on `\n` and then
  un-escaping is fine.
- **Residue check.** Because init and xorout are both 0, the CRC over
  `body + CRC` is `0x00` (after un-escaping). That's a handy cross-check if
  you frame on CRC instead of `\n`.
- **Validate on receive** with `check_frame()` and drop frames that fail.
  They are usually partial reads.

Test vectors (complete frames as they appear on the wire):

```
b"`FC<ctl td='GetDeviceCap'/>\x00\x19\n"
b"`FA<ctl td='GetBatteryInfo'/>\xf8\n"                                  # no null
b"`FC<ctl td='DCStatus'><DC name='xmpp' on='1' conn='2'/>\xd8\n"         # no null, used on the robot
b"`FB\x00\x00<ctl td='BatteryInfo'><battery power='067'/></ctl>\x00\xbf\n"
b"`FB\x00\x00<ctl td='error' errno='104'/>\x00\xe1\n"
b"`FA<ctl id='1' td='Clean'><clean type='auto' speed='standard' act='s'/></ctl>\x00M\n"
b"`FA\x00\x00<ctl td='Log' t='946687920' evt='8'/>\xa5\x01\x08Q\xe4\x00\xbd\n"   # binary tail
b"`FA<ctl id='863'/>\x00\\\x03\n"                                        # CRC 0x0A, escaped
```

## 4. Payload conventions

- **The MCU matches literal substrings, not parsed XML** **[mcu]**. Its
  string table holds fragments like `act='p'`, `act='h'`, `conn='2'`,
  `ret='ok'`, `speed=`/`strong`, `GetLog` and `stopGo`. So:
  - **Use single quotes** for attributes in frames you send, exactly as
    shown in §6. `act="p"` will not match.
  - **Watch for overlapping names.** Command names are matched by prefix or
    substring: `GetLog` catches `GetLogs`, and `Brush` also matches inside
    `SideBrush`.
  - **This is why malformed XML works.** It explains why unbalanced XML such
    as `<ctl td='Clean'><clean type='auto'></ctl>` was accepted
    **[verified]**. Still, send well-formed XML.
- **Request ids.** Requests carry `id='<digits>'` **[app]**, and the reply
  echoes it: `<ctl id='51038053' ret='ok' errno=''/>`. Use the id to match
  replies to requests. Channel C uses the literal strings `id='radomnum'`
  (sic), `id='req_resp_id'` and `id='REECOO87'`.
- **Reply status** **[mcu]**: `ret='ok' errno=''` is success. Failures come
  as `ret='fail'` or with `errno='7'` / `errno='9'`, and which commands
  produce those codes hasn't been worked out yet.
- **Quoting in received frames.** The MCU uses `'` in most frames, but some
  report templates use `"`, sometimes mixed in one frame (`CleanReport`,
  `ChargeState`). Accept both on receive.
- **Numbers.** Numbers are decimal strings, often zero-padded (`power='067'`,
  `val='089'`). Times are Unix seconds.
- **Clock.** The MCU clock starts at about Unix 946684800 (2000-01-01) after
  power loss until `SetTime` is sent **[verified]**: logs show
  `t='946685880'`.

## 5. Link setup (channel C)

### 5.1 What the stock ESP does at boot **[verified]**

1. ESP → `` `FC<ctl td='GetDeviceCap'/>`` about 14 times, until the MCU
   answers.
2. MCU → `` `FC<ctl td='DeviceCap' c='s2'><MID>...</MID><class>155</class><acs .../><acms>...</acms></ctl>``
   (capability list, see §8).
3. ESP → `` `FC<ctl td='WIFIStat' st='s2'/>``, then later
   `st='a'` / `st='o' s='<ssid>' p='<psk>'` as Wi-Fi comes up.
4. ESP → `` `FC<ctl td='DCStatus'><DC name='xmpp' on='1' conn='1' xsi='<xmpp server ip>'/></ctl>``,
   then `conn='2'` once XMPP is logged in.

### 5.2 What a replacement client must do

- **Send `DCStatus conn='2'` once after boot:**
  `<ctl td='DCStatus'><DC name='xmpp' on='1' conn='2'/>` on channel C. The
  MCU looks for `DCStatus` and the literal `conn='2'` **[mcu]**. Until it
  sees them, its charge and clean status reports are wrong or missing
  **[verified]**. `xsi` can be omitted.
- **The rest is optional.** `GetDeviceCap` and `WIFIStat` aren't needed for
  control.

### 5.3 Channel-C messages the MCU sends

| Message | Meaning |
|---|---|
| `<ctl id='radomnum' td='GetWIFIStat'/>` | Periodic poll for Wi-Fi state. The stock ESP answers `<ctl td='WIFIStat' st='o' s='<ssid>' p='<psk>' id='radomnum'/>`, which puts the Wi-Fi password on the UART in clear text. Ignoring it has no visible effect **[verified]** |
| `<ctl td='FactoryRest' id='req_resp_id'/>` | The user requested a factory reset on the robot. The stock ESP replies `<ctl id='req_resp_id' ret='ok'/>` **[verified]** and presumably clears its Wi-Fi and cloud config **[inferred]** |
| `<ctl id='REECOO87' td='SetWIFICfg'><cfg s='<ssid>' p='<psk>'/></ctl>` | **[mcu]** It tells the ESP which network to join. It sits next to the string `WIFITEST000` and the factory-test output `@424=Wifi Test=`, so it's most likely the factory Wi-Fi test **[inferred]**. A client can ignore it |
| `<ctl td='DeviceCap' ...>` | The reply to `GetDeviceCap` (§8) |

The `WIFIStat` `st` values seen are `s2`, `a`, `o` (online) and `c`. Apart
from `o`, their meanings are **[inferred]** only (smart-config, associating,
online, connecting).

## 6. Commands (channel A, ESP→MCU)

Add `id='<n>'` to everything except `Move` (the app sends `Move` without one
and expects no reply). The table omits the id. "MCU accepts" lists the
values present in the MCU's string table **[mcu]**. Values the app has but
the MCU lacks have no effect on this robot.

| Purpose | Payload | MCU accepts | Reply |
|---|---|---|---|
| Clean | `<ctl td='Clean'><clean type=T speed=S act=A/></ctl>` | `type` `auto` `spot` `border` `singleRoom` (and `stop` **[verified]**). `speed` `standard` `strong`. `act` only `'p'` (pause) and `'h'` (stop) | `ret` |
| Resume after pause | same as start: `act='s'` or no `act` | the MCU has **no `act='r'`**, so resuming is just starting again **[mcu]** | `ret` |
| Return to dock | `<ctl td='Charge'><charge type='go'/></ctl>` **[verified]** | `go`, `stopGo` | `ret` |
| Cancel return | `<ctl td='Charge'><charge type='stopGo'/></ctl>` | | `ret` |
| Locate (beep) | `<ctl td='PlaySound' sid='0'/>` (no `sid` also works **[verified]**) | `PlaySound` | `ret` |
| Clean state | `<ctl td='GetCleanState'/>` | | `<ctl id ret='ok' errno=''><clean type=T speed=S st='s'/'h' t='' a=''/></ctl>` **[mcu]**: `st` is only ever `s` or `h`, and `t`/`a` are always empty |
| Get speed | `<ctl td='GetCleanSpeed'/>` | | `<ctl id ret='ok' errno='' speed='standard'/'strong'/>` **[mcu]** |
| Set speed | `<ctl td='SetCleanSpeed' speed=S/>` | `standard`, `strong` | `<ctl id ret='ok' errno=''/>` |
| Battery | `<ctl td='GetBatteryInfo'/>` **[verified]** | | `<ctl id ret='ok'><battery power='067'/></ctl>` |
| Dock state | `<ctl td='GetChargeState'/>` **[verified]** | | `<charge type='Going'/'SlotCharging'/'WireCharging'/'Idle'/>` **[mcu]** (capital `G` in this reply) |
| Consumable | `<ctl td='GetLifeSpan' type=C/>` **[verified]** | `SideBrush`, `DustCaseHeap`, `Brush` | `<ctl id ret='ok' type=C val='089' total='365'/>`. `val` is % remaining, and `total` is always `365` **[mcu]** |
| Reset consumable | `<ctl td='SetLifeSpan' type=C val='100'/>` | same three types | `ret` |
| Firmware version | `<ctl td='GetVersion' name='FW'/>` | | `<ctl id ... td='version'><ver name='FW'>VERSION</ver></ctl>` **[mcu]** |
| Set clock | `<ctl td='SetTime'><time t='<unix>' tz='<±hours>' tzm='<minutes>'/></ctl>` | `t=`, `tz=`, `tzm=` | `ret` |
| Read logs | `<ctl td='GetLogs' count='N'/>` (see §10 hazard) | matched as `GetLog` | one `` `FA`` frame per entry: `<ctl td='Log' t='<unix>' evt=E/>` plus a binary tail **[verified]** |
| Clear logs | `<ctl td='EmptyLog'/>` **[verified]** | | `ret` |
| List schedules | `<ctl td='GetSched'/>` | | `<s>` children, see below |
| Add schedule | `<ctl td='AddSched'><sched name=N on='1' time='HH:MM' repeat=DAYS><ctl td='Clean'><clean type='auto'/></ctl></sched></ctl>` **[app]** | `AddSched`, `name` | `ret` |
| Edit schedule | `<ctl td='ModSched'><ModSched name=OLD><sched name=N on='0/1' time='HH:MM' repeat=DAYS><ctl td='Clean'><clean type='auto'/></ctl></sched></ModSched></ctl>` **[app]** | `ModSched` | `ret` |
| Delete schedule | `<ctl td='DelSched'><DelSched name=N/></ctl>` **[app]** | `DelSched` | `ret` |
| Manual drive | `<ctl td='Move'><move action=M/></ctl>` (no `id`) **[verified]** | `forward` `SpinLeft` `SpinRight` `TurnAround` `stop`. **No `backward` or `brake`** **[mcu]** | none |

Schedules:

- **Element the MCU returns** (in both the `GetSched` reply and `Sched2`)
  **[mcu]**:
  `<s n='<name>' o='0|1' t='HH:MM' r='<days>' f='<src>'> <ctl td='clean' type='auto'/> </s>`.
  The `f` attribute is optional. Note that the element is `<s>`, not
  `<sched>`, and that it uses the short attribute names.
- **Time.** `time`/`t` is zero-padded 24 h `HH:MM` in robot local time,
  using the offset from `SetTime`.
- **Days.** `repeat`/`r` is 7 characters of `0`/`1`, **Sunday first**
  **[app]**. For example `0111110` is weekdays. `0000000` means no repeat.
- **Name.** The app names a new schedule `<currentTimeMillis><random int>`.
  Any unique string works, and `ModSched`/`DelSched` use it as the key.
- **Clean mode.** Scheduled cleans are always `type='auto'`, at the current
  speed.
- **Untested.** Schedule commands haven't been sent to this robot yet.
  Confirm with a `GetSched` round trip first.

`DeviceCap` (§8) also advertises user-management commands (`GetUserInfo`,
`AddUser`, ...). Those are cloud ACLs and aren't needed locally.

## 7. Reports (channel B, MCU→ESP) **[mcu]**, examples **[verified]**

These are the exact templates in the MCU firmware:

| `td` | Shape |
|---|---|
| `BatteryInfo` | `<ctl td='BatteryInfo'><battery power='067'/></ctl>` (percent) |
| `CleanReport` | `<ctl td="CleanReport"><clean type="T" speed="standard\|strong" st=' ' rsn=' '/></ctl>`, or `<ctl td='CleanReport'> <clean type='T' speed='...'/> </ctl>` |
| `ChargeState` | `<ctl td="ChargeState"><charge type="going\|SlotCharging\|WireCharging\|Idle"/></ctl>`, or `<ctl td='ChargeState'><charge type='Going\|Idle'/></ctl>` |
| `LifeSpan` | `<ctl td='LifeSpan' type='SideBrush\|DustCaseHeap\|Brush' val='079' total='365'/>` |
| `Sched2` | `<ctl td='Sched2'>` + `<s .../>` children (§6) + `</ctl>`. Sent when schedules change, empty when there are none |
| `error` | `<ctl td='error' errno='N'/>` |

How to turn these into robot state:

- **`CleanReport` never carries a real `st` or `rsn`.** This MCU always
  sends a single space. Take the state from `type`: `auto`, `spot`,
  `border` or `singleRoom` means cleaning, and `stop` means stopped.
- **`GetCleanState` gives `st`.** Its reply has `st` set to `s` (cleaning)
  or `h` (halted) and nothing else. Paused and stopped can't be told apart
  from reports **[inferred]**, so track pause yourself from the commands you
  sent.
- **Match replies by `id`.** `GetCleanState` replies contain the same
  `<clean>` child as `CleanReport`, so a parser that only looks at the child
  tag will mis-handle them.
- **Dock state comes from `ChargeState`.** `going`/`Going` means returning,
  and `SlotCharging`/`WireCharging` mean docked. Compare
  case-insensitively.
- **What a return to dock looks like on the wire** **[verified]**
  (2026-10-18, on the ESPHome port):
  1. `Charge type='go'` gets `ret='ok'`.
  2. Straight away, `FB` `CleanReport type="stop"` and
     `FB` `ChargeState type="going"` arrive.
  3. `GetChargeState` answers `Going`.
  4. When the robot reaches the dock, `FB` `ChargeState type="SlotCharging"`
     arrives (here about 15 s later).

  While returning, `GetCleanState` answers `type='stop' st='s'`. **`st='s'`
  does not mean cleaning when `type='stop'`**, so never derive "cleaning"
  from `st` alone.
- **`errno=100` means "no error".** In practice it arrives when the robot
  times out sitting idle off the dock, so treat it as stopped **[verified]**.
- **Ignore what the app has but this MCU never sends:** `PowerOff`, going-home
  reasons (`r=` on `ChargeState`), clean start/stop reasons, and
  comma-separated or action errors. They're **[app]** features of other
  models. Parsing them is harmless.

## 8. Device capability reply **[verified]** **[mcu]**

```
<ctl td='DeviceCap'  c='s2'><MID><device-id>@155.ecorobot.net/atom</MID><class>155</class>
<acs v='setting,userman,clean'/><acms>
<acm ac='setting' td='SetTime,SetLifeSpan,EmptyLog,GetSched,ModSched,DelSched,Sched2'/>
<acm ac='clean' td='GetBatteryInfo,BatteryInfo,GetLogs,Move,error,Clean,CleanReport,GetCleanState,Charge,ChargeState,GetChargeState,GetLifeSpan,LifeSpan'/>
<acm ac='userman' td='GetUserInfo,AddUser,DelUser,SetAC,GetAccessControl,SetAccessControl'/></acms></ctl>
```

(Shown wrapped. On the wire it's one line. `MID` is this unit's cloud id.)

## 9. Enumerations

These are the values this MCU uses **[mcu]**. Values in parentheses exist
only in the app **[app]** and are irrelevant here.

- **T** clean type: `auto`, `spot`, `border`, `singleRoom`, `stop`
  (`SpotArea`, `CustomArea`, `IR`).
- **S** speed: `standard`, `strong` (`silent`, `intelligent`).
- **A** action: `p` pause, `h` stop. Anything else, including `s` or no
  `act`, starts. (`r` resume)
- **ST** status (GetCleanState only): `s` cleaning, `h` halted. (`p`
  paused)
- **CS** charge state: `going`/`Going`, `SlotCharging`, `WireCharging`,
  `Idle`.
- **C** consumable: `SideBrush`, `DustCaseHeap`, `Brush` (`AirFilter`,
  `IonSterilize`).
- **M** move: `forward`, `SpinLeft`, `SpinRight`, `TurnAround`, `stop`
  (`backward`, `brake`).
- **SRC** schedule source `f`: `p` app, `q` robot, `i` IR remote **[app]**.
- **E** log event, values present in the MCU:
  - `1` hang
  - `2` wheel error
  - `6` down-sensor error
  - `7` clean started
  - `8` clean stopped

  The app also defines `3` stuck, `4` complex environment and `5` LDS
  error.
- **errno** device error **[app]** (the MCU builds this number at runtime):

  | Code | Meaning |
  |---|---|
  | 100 | ok / stopped |
  | 101 | low battery |
  | 102 | host hang |
  | 103 | wheel abnormal |
  | 104 | down (cliff) sensor |
  | 105 | stuck |
  | 106 | side brush worn |
  | 107 | dust case/filter worn |
  | 108 | side brush abnormal |
  | 109 | roller abnormal |
  | 110 | no dust box |
  | 111 | bumper abnormal |
  | 112 | LDS |
  | 113 | main brush worn |
  | 114 | dust case full |
  | 115 | battery error |
  | 116 | forward-looking sensor |
  | 117 | gyroscope |
  | 118 | strainer blocked |
  | 119 | fan error |
  | 120 | water box |
  | 121 | plate jammed |
  | 201 | air filter missing |
  | 202 | ultrasonic |
  | 203 | small wheel |
  | 204 | wheel hang |
  | 205–207 | ion sterilizer |

  **[verified]** on this robot: 100, 104, 111.

## 10. Hazards and quirks

- **Escape the check byte** (§2.3). An unescaped `\` or `` ` `` check byte
  makes the MCU drop the frame silently, and an unescaped `\n` truncates it.
- **Use single-quoted attributes** in frames you send (§4).
- **`GetLogs` resets the Wi-Fi module.** Calling it a second time without
  `EmptyLog` makes the MCU reset the ESP8266 **[verified]**. Send `EmptyLog`
  right after every `GetLogs`, or don't use it.
- **Pace your sends.** Space TX frames about 60–100 ms apart. 60 ms
  between frames worked for the brute-force loop **[verified]**. Whether
  back-to-back frames get dropped hasn't been tested, and the ESPHome port
  uses 100 ms to be safe.
- **Text-mode captures are lossy.** A serial terminal in text mode strips
  the nulls and turns bytes ≥ 0x80 into U+FFFD (`EF BF BD`). Don't take
  check bytes from such captures. Use raw binary captures.
- **The UART carries secrets.** It carries the Wi-Fi SSID and PSK
  (`WIFIStat`, `SetWIFICfg`) and the user's cloud JID. Redact them from logs
  and docs.

## 11. How to verify a change without the robot

1. Build the frame with `frame()` from §3, and check it with
   `check_frame()`.
2. Run the test vectors in §3. Every one must pass `check_frame()`.
3. Check an attribute value against the MCU's string table. It's quick, and
   it tells you whether the MCU will recognise it:
   `python3 -c "import sys;print(open(sys.argv[1],'rb').read().count(sys.argv[2].encode()))" mcu-dump.bin "act='p'"`
   (this needs your own dump of the mainboard flash).
4. On hardware, a frame the MCU accepts usually gets a reply within 60 ms
   (the brute-force loop's read delay). A bad CRC or bad escape is ignored
   silently.

## 12. Sources

| Source | What it gave |
|---|---|
| Mainboard flash dump, firmware `V703_513_214_180706` (STM32F071, Thumb-2, base `0x08000000`; not distributed) | Baud (`0x0800b9d0`), CRC table/update/compute (`0x08018290`, `0x08017860`, `0x08017496`), RX check (`0x08014750`), RX un-escape (`0x08018870`), TX framing and escape (`0x080132ac`), and string tables for the accepted commands (`0x08014900`–`0x08015b00`) and the report templates (`0x08019300`–`0x0801a500`, `0x08002000`) |
| Sniffed UART traffic of the stock Wi-Fi firmware, plus an earlier MicroPython replacement whose check bytes were brute-forced | Working frames, quirks, boot handshake |
| Ecovacs Android app (dex2jar): `com/ecovacs/lib_iot_client/robot/protocol/DN79SProtocol`, `robot/DN79S`, `robot/*` enums, `IOTDeviceType`, `robot/EcoRobotFactory` | Command builders, enums, and the 155 → DN79S mapping |
| `components/deebot_n79s/` and `tools/n79s_frame.py` in this repo | The reference implementations (C++ and Python) |
