# Flutter App MQTT Requirements - Firmware V2

## Purpose

Dokumen ini adalah requirement MQTT untuk aplikasi Flutter yang memonitor dan mengontrol Smart Building Master pada Firmware V2.

Changed:
- Firmware V2 memakai topic MQTT per data type, bukan satu JSON besar sebagai state utama.
- Command actuator dikirim ke topic `control`, lalu master meneruskan command ke slave target.
- State actuator dipublish ulang hanya setelah target slave mengonfirmasi state terbaru.
- Server dapat menjadi consumer utama MQTT, melakukan phrasing/normalisasi data,
  lalu menyediakan data untuk Flutter. Master tetap mengirim payload numerik ringan.

Why:
- Per-topic MQTT membuat dashboard lebih sederhana, payload lebih kecil, dan setiap sensor/actuator bisa disinkronkan secara terpisah.

Implementation effect:
- Flutter app harus subscribe ke topic sensor/actuator yang relevan untuk class/room terpilih.
- Firmware master harus publish state per sensor type dan subscribe command actuator per actuator type.
- Single master state JSON V1 tidak lagi dipublish periodik oleh Firmware V2.5.

---

## 1. Firmware V2 MQTT Topic Model

Changed:
- Topic MQTT utama sekarang mengikuti format class/room + data type.
- Format runtime Firmware V2.5 adalah `<class_name>/data/<data_type>` untuk publish
  dan `<class_name>/control/<command_type>` untuk command.

Why:
- User dan dashboard perlu melihat data berdasarkan ruang/class dan jenis data secara langsung.

Implementation effect:
- App harus mengizinkan konfigurasi class/room display name.
- Jika class/room name berubah, app dan firmware harus regenerate default topic labels/templates dari nama tersebut.
- App tidak boleh menganggap semua data datang dari satu topic state master.
- App harus membentuk topic dari class name yang sama dengan master.

Exact publish topics for class `HD01`:
- `HD01/data/temp`
- `HD01/data/co2`
- `HD01/data/lux`
- `HD01/data/human`
- `HD01/data/led`
- `HD01/data/projector`
- `HD01/data/ac`
- `HD01/data/alert`
- `HD01/data/active`

Exact subscribe/control topics for class `HD01`:
- `HD01/control/led`
- `HD01/control/ac`
- `HD01/control/projector`
- `HD01/control/schedule`

Class name examples:
- If class/room name is `HD01`, default topic labels are derived as `HD01/data/<data_type>` and `HD01/control/<command_type>`.
- If class/room name is `LA2`, default topic labels are derived as `LA2/data/<data_type>` and `LA2/control/<command_type>`.
- Example: `LA2/data/co2`, `LA2/data/temp`, `LA2/control/led`.

Topic naming rule:
- Data topics SHALL use `<class_name>/data/<data_type>`.
- Command topics SHALL use `<class_name>/control/<command_type>`.
- Class name comes from the editable and persisted master class name.
- Supported data type suffixes are `temp`, `co2`, `lux`, `human`, `led`, `ac`,
  `projector`, `alert`, and `active`.
- Supported control suffixes are `led`, `ac`, `projector`, and `schedule`.

Direction rule:
- Firmware V2.5 separates state and command topics.
- Firmware publishes confirmed state on `data` topics and listens on `control`
  topics.

---

## 2. Publish Requirements

Changed:
- Each sensor data type SHALL have its own MQTT publish topic.
- General simple sensors SHALL publish integer payloads, except temperature.
- LED SHALL publish an integer scalar, `1` for ON and `0` for OFF.
- Temperature SHALL publish one float average in Celsius with one decimal place, calculated from valid temperature slots.
- AC SHALL use `PPTTFFSS` because it carries power, target temperature, fan speed, and swing.

Why:
- Most simple sensors are easier to process as integer payloads; temperature keeps one decimal place for useful display precision.
- Multi-position devices need structured payloads so the app can synchronize each position correctly.

Implementation effect:
- Flutter app must parse payload type by topic/data type.
- MQTT consumers must not expect one shared master state JSON as the primary data source.

Payload rules:
- Temperature: float average Celsius with one decimal place, example value `27.4`. Firmware skips publishing this topic while temperature is invalid; Alert Bit 0 indicates invalid temperature.
- CO2: integer payload, example value `720`.
- Lux: integer non-projector room-Lux payload, example value `350`. Projector-verification Lux is local-only and never published to the app.
- Presence: integer payload, example values `0` or `1`.
- LED: integer payload, `1` means ON and `0` means OFF.
- Projector: integer payload, `1` means ON and `0` means OFF.
- Alert: decimal integer bitmask.
- Active: retained integer `1`; MQTT LWT sets retained `0` on unexpected disconnect.
- Simple scalar sensors: integer payload unless their specific contract says otherwise; temperature is float.

MQTT delivery policy:

| MQTT message type | QoS | Retain | App behavior |
|---|---:|---|---|
| Simple sensor publish | 0 | true | App may receive the last-known numeric value immediately after subscribe. |
| LED/projector state publish | 0 | true | App should treat retained `1`/`0` as last confirmed actuator state. |
| Temperature average publish | 0 | true | App may receive last-known average temperature immediately after subscribe. |
| Alert publish | 0 | true | Server/app decodes decimal bitmask. |
| Active publish / LWT | 1 | true | Retained `1` while online; retained `0` on MQTT LWT. |
| Actuator command | 1 | false | App commands must not be retained; old commands must not replay after reconnect. |

What changed: QoS/retain policy is explicitly defined for Firmware V2.

Why: reconnecting apps should see latest retained state, but actuator commands must not repeat accidentally.

Implementation effect: Flutter app should accept retained sensor/status messages as last-known state and should publish commands with retain disabled.

### 2.1 Temperature Publish Payload

Changed:
- Temperature is published as one float average in Celsius with one decimal place.

Why:
- The dashboard/app only needs the room-level temperature summary for MQTT, while detailed per-slot temperature stays local to the master UI.

Implementation effect:
- Flutter app or server parses `HD01/data/temp` as a float/double.
- Firmware does not overwrite the retained temperature topic while no valid temperature slot is available. The app SHALL use Alert Bit 0 to indicate the invalid condition.

Example payload:

```text
27.4
```

Parsing rules:
- Parse the whole payload as a signed float/double.
- Values `0.0..80.0` are normal Celsius display values.
- The temperature topic is not updated while unavailable, stale, not installed, or no valid slots exist. Alert Bit 0 communicates the invalid condition.

### 2.2 LED Publish Payload

Changed:
- LED state is published as one integer scalar.

Why:
- The Flutter dashboard only needs room-level LED ON/OFF status for the current control surface.

Implementation effect:
- Flutter app must treat LED publish payload as source of truth after command confirmation.
- App should update local UI state from the latest LED publish message, not from optimistic command state alone.

Example payload:

```text
1
```

Parsing rules:
- `1` means ON.
- `0` means OFF.
- Firmware command input accepts `1`, `0`, `on`, `off`, `true`, or `false`.

### 2.3 Alert Publish Payload

Changed:
- Alert is published as one decimal integer bitmask on `HD01/data/alert`.

Why:
- Decimal bitmask keeps the ESP32 payload numeric and small while still allowing
  server/Flutter to decode multiple simultaneous alert states.

Implementation effect:
- Server should decode alert flags before phrasing them for Flutter.
- Flutter may also decode the bitmask directly if it subscribes to MQTT.

Bit assignments:

| Bit | Decimal | Meaning |
|---:|---:|---|
| 0 | 1 | Temperature error / no valid temperature. |
| 1 | 2 | CO2 error / no valid CO2. |
| 2 | 4 | Lux error / no valid Lux. |
| 3 | 8 | Human/presence sensor error / no valid presence. |
| 4 | 16 | LED/relay error. |
| 5 | 32 | Check projector / IR path. Projector command was sent, but Lux verification did not prove ON after retry. |
| 6 | 64 | AC control/bus error or cooling-performance warning. |
| 7 | 128 | After-hours empty-room active-load anomaly. Server/Flutter should phrase this as an empty-room energy anomaly, not as a sensor fault. |

Example:

```text
131
```

Meaning: temperature error + CO2 error + after-hours empty-room active-load anomaly.

---

## 3. Subscribe And Command Requirements

Changed:
- Firmware V2 subscribes to actuator control topics such as LED, AC, and Projector.
- Command flow is command received, forward to target slave, wait for command result, then publish updated command/status state.
- AC command payload SHALL use the 8-digit decimal format `PPTTFFSS`.
- LED and Projector command payloads SHOULD be scalar `1` or `0`; JSON remains accepted for compatibility.

Why:
- The app/dashboard must synchronize with the master's confirmed state or command result, not only requested state.
- IR-based AC and projector controls are one-way, so command status means command execution result only, not verified physical AC/projector state.

Implementation effect:
- Flutter app may show pending state after sending a command.
- Flutter app should consider the command resolved only after the relevant topic publishes updated state or command-result status.
- Firmware must route actuator commands to the slave that owns the actuator.

Exact subscribe/control topics for class `HD01`:
- `HD01/control/led`
- `HD01/control/ac`
- `HD01/control/projector`
- `HD01/control/schedule`

Direction rule:
- Actuator state and command topics are separated.
- Master publishes state to `HD01/data/...`.
- Master listens to commands on `HD01/control/...`.

Schedule command payloads:

```text
PRE_CLASS_ON
CLASS_ENDED
YYYYMMDD;HHMM-HHMM;HHMM-HHMM;...
```

The server owns schedule generation. The master has no schedule-edit UI; it
only listens to `HD01/control/schedule`. A valid daily payload replaces the
previous stored daily schedule and resets trigger flags. Recommended server
behavior is to publish the full daily schedule around midnight so the master can
execute pre-class and class-ended actions locally even if server availability is
unstable later in the day.

Implementation checkpoint:
- `PRE_CLASS_ON` and `CLASS_ENDED` are implemented.
- Daily payload parsing, persistence, overwrite, catch-up, and automatic local
  slot execution are implemented in V2.8.
- Recommended sensor polling and MQTT publish timing is documented in
  `docs/V2.8_Planning.md`.

Command flow:
1. Flutter app or server publishes actuator command to the selected class/room `control` topic.
2. Master receives the MQTT command.
3. Master forwards the command to the target slave device.
4. Target slave applies command and reports command status/result.
5. Master republishes the updated actuator state to the matching `data` topic.
6. Server/Flutter updates UI from the republished state/result.

Lamp control rule:
- Flutter SHALL show one aggregate Lamp control only.
- Flutter SHALL NOT expose separate LED 1 and LED 2 controls.
- A scalar command on `HD01/control/led` controls Relay 1 and Relay 2 together.
- Per-relay LED 1/LED 2 control is reserved for the local master touchscreen.

IR command status rule:
- AC and Projector command status SHALL be displayed as command-result status only, such as pending, success, busy, or failed.
- The app SHALL NOT present IR command status as proof that the physical AC/projector actually changed state, because IR communication is one-way.
- If the app shows requested AC/projector power, temperature, mode, or input, it SHALL label/treat it as the last command accepted by the master, not sensor-verified device state.

AC control panel rule:
- The current Flutter app requirement is one AC control panel.
- When the master exposes both AC 1 and AC 2 under an IR-capable device profile, the master SHALL mirror the same AC command to AC 1 and AC 2.
- Flutter SHALL NOT require separate AC 1 and AC 2 panels for the current implementation.

AC command example:

```text
01240201
```

Meaning:
- `01`: AC ON.
- `24`: target temperature 24 C.
- `02`: fan speed medium.
- `01`: swing auto.

AC payload format:
- `PP`: `00` off, `01` on.
- `TT`: target temperature, `16..30`.
- `FF`: fan speed enum.
- `SS`: swing/vertical vane enum.

Fan speed enum:
- `00`: auto.
- `01`: low.
- `02`: medium.
- `03`: high.
- `04`: quiet/silent.
- `05`: turbo/powerful.
- `06..98`: reserved.
- `99`: no change / unsupported.

Swing enum:
- `00`: off/fixed.
- `01`: auto swing.
- `02`: up.
- `03`: mid-up.
- `04`: middle.
- `05`: mid-down.
- `06`: down.
- `07`: step next.
- `08`: step previous.
- `09`: auto comfort.
- `10`: auto powerful.
- `11..98`: reserved.
- `99`: no change / unsupported.

Projector command example:

```text
0
```

AC target rule:
- Firmware clamps `TT` to `16..30` degrees Celsius before forwarding the command to the RS485 slave.

---

## 4. Flutter App Behavior

Changed:
- Flutter app navigation for Firmware V2 should stay simple: Home, Devices, Settings.
- Home shows selected class/room dashboard.
- Devices manages discovered MQTT masters/devices and display settings.
- Settings manages broker/topic configuration and editable class/room naming.

Why:
- Firmware V2 MQTT model is per-topic and class/room oriented, so the app should prioritize room dashboard clarity over raw master JSON inspection.

Implementation effect:
- App should maintain a selected class/room context.
- App should subscribe to the configured topics for the selected class/room.
- App should not require the legacy single state JSON to render the main dashboard.

Required screens:
- Home: selected class/room dashboard with latest sensor and actuator states.
- Devices: discovered masters/devices, class/room mapping, display names, Device Profile, status, and last seen.
- Settings: broker address, credentials if needed, and topic/class configuration.

Devices / metadata requirements:
- Device Profile SHOULD appear in the device list or device detail when the master publishes saved device registry metadata.
- Device Profile values SHOULD use the master-owned v2.1 profile names or app labels derived from them: `TEMP_NODE`, `PRESENCE_NODE`, `CO2_NODE`, `RELAY_NODE`, and `IR_COMBO_NODE`.
- Each app-facing device row SHOULD include device name, room, Device Profile, online/stale/offline status, and last seen.
- Device name and room come from the master saved registry and are user-facing metadata.
- Status and last seen are runtime metadata from the master and SHOULD NOT be treated as continuously persisted slave fields.
- The app MAY show MAC/address in device detail or developer mode when published by the master, but normal app behavior should not depend on raw Modbus register addresses.
- Unknown devices reported by the master SHOULD appear as unpaired or pending user action, not as automatically usable dashboard devices.

Settings / Device Info requirements:
- Settings SHOULD provide a second page or next-page flow when needed.
- Device Info SHALL show firmware version, for example `Firmware V2`.
- Device Info SHALL show `Firmware By Hansel Kay CE LAB`.
- Device Info SHALL allow editing class/room name.
- Editing class/room name SHALL update default topic labels/templates, for example `HD01` -> `HD01/data/co2` and `LA2` -> `LA2/data/co2`.

Home SHOULD show:
- Average temperature from the float `data/temp` topic.
- CO2 integer value.
- Lux integer value if available.
- Presence integer/binary value if available.
- LED ON/OFF from the integer `data/led` topic.
- One AC control panel if configured; master mirrors to AC 1 and AC 2 when both are exposed.
- Projector ON/OFF from the integer `data/projector` topic if configured.

Control behavior:
- App SHALL send commands only for available actuator topics.
- App SHOULD show pending state after command publish.
- App SHALL use the next confirmed state publish or command-result publish as the synchronized UI state.
- For IR controls, synchronized UI state means the master's latest accepted command/result, not verified physical AC/projector state.

---

## 5. Error Handling And Stale Data

Changed:
- Stale state is evaluated per topic, not only per master.

Why:
- In Firmware V2 one sensor topic may keep updating while another sensor or actuator topic becomes stale.

Implementation effect:
- Flutter app should show stale status per sensor/actuator card.
- MQTT debugging should identify which topic failed or became stale.

Rules:
- If MQTT disconnects, mark all displayed values as disconnected/stale.
- If one topic misses expected updates, mark only that topic stale.
- Keep last known value visible but visually marked stale.
- Malformed payloads should be logged in developer/debug mode.
- Integer payload parse failures should not crash the app.
- JSON payload parse failures should not overwrite the last known valid state.

---

## 6. Legacy / V1 Notes

Changed:
- The old single master state JSON model is retained only as Legacy / V1 reference.

Why:
- Older firmware and earlier app experiments may still use the V1 state topic, but Firmware V2 must not depend on it as the primary model.

Implementation effect:
- Flutter app MAY support the V1 payload as a compatibility mode.
- New Firmware V2 implementation SHOULD prioritize per-topic publish/subscribe behavior.

Legacy V1 model:
- Master published one large JSON state payload to one configured state topic.
- App subscribed to a wildcard topic such as `smart-building/master/+/state`.
- App sent JSON commands to one configured command topic.
- Payload contained combined network, slave, sensor, and control state.

Legacy compatibility guidance:
- If supporting V1, keep it behind an explicit compatibility mode.
- Do not mix V1 combined state assumptions into the Firmware V2 dashboard requirements.
- Do not require `type: smart_building_master_state` for Firmware V2 per-topic sensor payloads.

---

## 7. Versioning

Changed:
- Firmware V2 versioning is documented at the topic/payload contract level.

Why:
- Per-topic payloads may evolve independently.

Implementation effect:
- JSON payloads SHOULD include version metadata when practical.
- Integer payload topics may rely on topic contract version documented in project docs.
- App should ignore unknown JSON fields.

Rules:
- JSON payloads SHOULD ignore unknown fields.
- JSON payloads MAY include `schema_version`.
- Integer topics SHALL be parsed according to the configured data type for that topic.
- Breaking payload changes must be documented before firmware/app implementation changes.
