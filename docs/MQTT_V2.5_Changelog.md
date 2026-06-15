# MQTT V2.5 Changelog

## Runtime Topic Format

Firmware V2.5 uses the saved master class name as the MQTT topic prefix.
The current lightweight runtime format is:

```text
<class_name>/data/<data_type>
<class_name>/control/<command_type>
```

For a master named `HD01`, the exact V2 topics are:

```text
HD01/data/temp
HD01/data/co2
HD01/data/lux
HD01/data/human
HD01/data/led
HD01/data/projector
HD01/data/ac
HD01/data/alert
HD01/data/active

HD01/control/led
HD01/control/projector
HD01/control/ac
HD01/control/schedule
```

Changing the class name in the master changes the prefix automatically. For
example, class `LA2` publishes to `LA2/data/temp`, `LA2/data/co2`, and the
other `LA2/data/<data_type>` topics.

Actuator state and commands no longer share the same literal topic. Master
publishes confirmed state to `<class_name>/data/...` and listens for commands on
`<class_name>/control/...`.

Firmware V2.5 runtime publishes only the V2 per-topic model. The old combined
JSON compatibility topic `binus/ayam` is cleared as a retained message on MQTT
connect and is not published periodically.

The deployment flow now allows a server to subscribe to MQTT, normalize/phrase
the numeric payloads, and then serve Flutter. Because of that, the master keeps
payloads numeric and small.

## Payload Format

Runtime payloads are intentionally small:

| Topic example | Payload | Meaning |
|---|---|---|
| `HD01/data/temp` | Float | Average temperature in Celsius with one decimal place, for example `27.4`. Publish is skipped while no temperature is valid so the retained last-known value is preserved; Alert Bit 0 indicates invalid temperature. |
| `HD01/data/co2` | Integer | CO2 ppm. |
| `HD01/data/lux` | Integer | Valid non-projector room Lux. Projector-verification Lux is local-only and never published. |
| `HD01/data/human` | Integer | `1` means presence detected, `0` means no presence. |
| `HD01/data/led` | Integer | `1` means any mapped LED/relay is ON, `0` means all mapped LED/relay outputs are OFF. |
| `HD01/data/projector` | Integer | `1` means projector ON command/state, `0` means OFF command/state. |
| `HD01/data/ac` | 8-digit integer | AC command/state encoded as `PPTTFFSS`. Target temperature is clamped to `16..30` degrees Celsius. |
| `HD01/data/alert` | Decimal integer bitmask | Error/alert flags agreed by master, server, and Flutter. |
| `HD01/data/active` | Integer | Retained `1` when master is connected. MQTT LWT publishes retained `0` if it disconnects unexpectedly. |

The firmware accepts scalar command payloads on `HD01/control/led` and
`HD01/control/projector`: `1`, `0`, `on`, `off`, `true`, or `false`. New clients
should send numeric payloads.

`HD01/control/schedule` accepts both event commands and daily schedule payloads:

```text
PRE_CLASS_ON
CLASS_ENDED
YYYYMMDD;HHMM-HHMM;HHMM-HHMM;...
```

A valid daily schedule payload overwrites the previous stored schedule and
resets slot trigger flags. Example: `20260609;0800-0930;1015-1200`. This lets
the server push one daily schedule around midnight while the master executes the
day locally if server/MQTT availability becomes unstable later.

The overwrite is immediate: a newly accepted payload replaces all previous
slots even when an old slot has not started or ended yet.

LED state publication uses relay-register confirmation. After
`<class>/control/led`, the master writes the relay command, reads Relay 1/2 back,
then publishes the confirmed aggregate `0` or `1` to `<class>/data/led`. Lux is
optional and is not part of LED ON/OFF confirmation. A failed write/readback
raises Alert Bit 4 and does not publish the requested state as if it succeeded.

Current V2.8 implementation: `PRE_CLASS_ON`, `CLASS_ENDED`, daily schedule
validation, NVS persistence, overwrite, reboot catch-up, and local execution are
active. See `docs/V2.8_Planning.md`.

### Alert Decimal Bitmask

`HD01/data/alert` publishes one decimal integer. Each bit means:

| Bit | Decimal | Meaning |
|---:|---:|---|
| 0 | 1 | Temperature error / no valid temperature. |
| 1 | 2 | CO2 error / no valid CO2. |
| 2 | 4 | Lux error / no valid Lux. |
| 3 | 8 | Human/presence sensor error / no valid presence. |
| 4 | 16 | LED/relay error. |
| 5 | 32 | Check projector / IR path. Raised when Projector ON was commanded but no Lux channel verified ON after retry. |
| 6 | 64 | AC control/bus error or cooling-performance warning. |
| 7 | 128 | After-hours empty-room active-load anomaly. Recommended V2.7.1 trigger: valid time, valid empty occupancy, enough baseline days, and `active_load_minutes > max(avg_7d * 1.5, avg_7d + 60)` during 22:00-06:00. |

Example:

```text
131
```

Means `1 + 2 + 128`: temperature error, CO2 error, and after-hours
empty-room active-load anomaly.

### AC 8-Digit Payload Draft

The next AC MQTT contract uses one compact 8-digit decimal payload:

```text
PPTTFFSS
```

Field meaning:

| Field | Digits | Values |
|---|---:|---|
| `PP` | 2 | AC power: `00` off, `01` on. |
| `TT` | 2 | Target temperature in Celsius: `16..30`. |
| `FF` | 2 | Fan speed enum. |
| `SS` | 2 | Swing/vertical vane enum. |

Examples:

```text
01240201
```

Means AC on, target `24` C, fan speed `02`, swing mode `01`.

```text
00240000
```

Means AC off, remembered/desired target `24` C, fan auto/default, swing off/fixed.

Fan speed enum:

| Value | Meaning |
|---|---|
| `00` | Auto |
| `01` | Low |
| `02` | Medium |
| `03` | High |
| `04` | Quiet/Silent |
| `05` | Turbo/Powerful |
| `06..98` | Reserved |
| `99` | No change / unsupported |

Swing enum:

| Value | Meaning |
|---|---|
| `00` | Off / fixed |
| `01` | Auto swing |
| `02` | Up |
| `03` | Mid-up |
| `04` | Middle |
| `05` | Mid-down |
| `06` | Down |
| `07` | Step next |
| `08` | Step previous |
| `09` | Auto comfort |
| `10` | Auto powerful |
| `11..98` | Reserved |
| `99` | No change / unsupported |

Implementation note: master firmware now publishes and accepts `HD01/data/ac`
and `HD01/control/ac` in
this `PPTTFFSS` format. Legacy AC JSON parsing remains as a transition fallback,
but new clients should use the 8-digit payload.

## Default EMQX Deployment

```text
Deployment: SmartClass_serverless
Address: wd5de919.ala.asia-southeast1.emqxsl.com
MQTT TLS port: 8883
WebSocket TLS port: 8084
Username: Hansganteng
Password: 12345678
Publish interval: 5 seconds
```

The ESP32 firmware connects with MQTT over TLS on port `8883`. Port `8084` is
for WebSocket TLS clients such as web applications and is not used by the
firmware's PubSubClient connection.

## Compatibility Changes

- Replaced the previous flat topic labels with data/control topics such as
  `HD01/data/temp` and `HD01/control/ac`.
- Added tracked EMQX defaults in `src/mqtt_defaults.h`.
- Kept `src/mqtt_secrets.h` as an optional ignored local override.
- Kept retained V2 state publishing.
- Stopped periodic legacy `binus/ayam` combined-state publishing.
