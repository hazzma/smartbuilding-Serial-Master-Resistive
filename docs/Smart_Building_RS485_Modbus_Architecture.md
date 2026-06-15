
# Smart Building RS485 Modbus Communication Architecture
## Capability-Driven Master-Orchestrated Design

## Project
Smart Building Master S3 + Distributed Slave Network

## Document Purpose

Dokumen ini mendefinisikan arsitektur komunikasi RS485 terbaru menggunakan:

- Modbus RTU via DFRobot_RTU Library
- Slave wire contract v2.1.0: `docs/From_SLave/RS485_Modbus_Slave_Firmware_Contract v2.md`

Dokumen ini mencakup:
- RS485 transport architecture
- Modbus register philosophy
- Firmware V2.1 slave model: master-owned Device Profile enforcement
- Master-orchestrated runtime behavior
- Discovery & pairing flow
- Slave identity system
- Runtime role assignment
- Saved-slave reconnect startup flow
- Dashboard integration model
- Control routing
- Reliability philosophy
- Slave implementation guideline

Tujuan utama:

Membuat sistem slave fleksibel, scalable, mudah diremix, dan tetap kompatibel dengan Firmware V2.1:
- saved slave reconnect on startup
- Device Profile enforcement in the master
- optional Lux through v2.1 registers
- `IR_COMBO_NODE` support for AC 1, AC 2, and Projector
- v2.1 register compatibility without reviving V1 bundled capability assumptions

---

# 1. Core System Philosophy

Slave SHALL NOT menentukan dirinya menjadi apa.

Slave hanya menyediakan:
- capability
- sensor
- channel
- control endpoint

Master menentukan:
- role
- mapping
- dashboard slot
- control purpose
- runtime behavior
- active feature

Firmware V2 alignment:

- What changed: the master owns Device Profile policy and the slave remains policy-blind.
- Why it changed: production hardware needs explicit profiles such as `IR_COMBO_NODE`, while slaves should stay simple.
- Implementation effect: Slave Detail selects a profile, master persists that profile, and master writes only the v2.1 registers allowed by that profile.

---

# 2. Architecture Philosophy

Slave = Dumb Capability Node

Master = System Brain / Orchestrator

Slave tidak memiliki knowledge terhadap:
- dashboard layout
- room logic
- temp slot order
- IR target purpose
- UI behavior

Master memiliki seluruh orchestration logic.

---

# 3. Supported Slave Model

Firmware V2.1 primary model:

```text
Master-owned Device Profile + policy-blind slave registers
```

Initial Device Profiles:

- `TEMP_NODE`: Temperature sensors + optional Lux.
- `CO2_NODE`: CO2 sensors + optional Lux.
- `PRESENCE_NODE`: Human Presence sensors + optional Lux.
- `RELAY_NODE`: Relay/LED control + optional Lux.
- `IR_COMBO_NODE`: IR AC 1 + IR AC 2 + IR Projector + optional Lux.

Device Profile rules:

- Device Profile is stored in the master registry.
- Slave does not need a profile register.
- Slave does not decide which profile combinations are valid.
- Master writes v2.1 assignment registers based on selected profile.
- `IR_COMBO_NODE` is intentionally allowed for production cost, PCB count, and installation simplicity.

Temperature note:

Temperature can still expose up to four DHT22 channels through `TEMP_SENSOR_ASSIGNMENT` bits. This is still one Temperature profile, not four different profiles.

Legacy / V1 Notes:

Previous architecture drafts allowed `1 slave = multiple capabilities`, for example `TEMP + CO2 + PIR + IR`. That assumption is deprecated for Firmware V2. It remains useful as historical context only and SHALL NOT be treated as the primary implementation target.

What changed: v2.1 replaces the generic one-main-type rule with explicit Device Profiles.
Why it changed: Firmware V2.1 needs clear master-owned policy while allowing production exceptions such as `IR_COMBO_NODE`.
Implementation effect: UI and master validation should use profile rows, not raw multi-checkbox main-type policy.

---

# 4. Communication Stack

The implementation has two related flows: runtime data flow and UI command flow.

Runtime data flow:

```text
RS485 BUS
    ->
UART + MAX3485
    ->
DFRobot_RTU Library
    ->
RS485 Manager
    ->
Slave Registry
    ->
Mapping Manager
    ->
Dashboard Model
    ->
Dashboard/UI
```

UI command/request flow:

```text
Dashboard/UI
    ->
UI callbacks / state request flags
    ->
RS485 Manager
    ->
DFRobot_RTU Library
    ->
UART + MAX3485
    ->
RS485 BUS
```

Dashboard/UI SHALL NOT access UART, MAX3485 direction control, Modbus parser, or raw register transport directly.

Implementation note:

```text
RS485State.slaves[]      = Slave Registry
RS485State.mappings[]    = Logical Mapping table
RS485State.dashboard     = Dashboard Model
mapping_manager_update_locked()
    = recompute logical mappings and compose Dashboard Model
```

The code still exposes compatibility/debug types such as `RS485Frame`, `RS485Command`, sequence counters, and transaction result wrappers. These are firmware API/debug compatibility layers only. They SHALL NOT be interpreted as the active wire protocol. The active RS485 wire transport is Modbus RTU through DFRobot_RTU.

Physical transport stack:

```text
RS485 Manager
    ->
DFRobot_RTU Library
    ->
UART + MAX3485
    ->
RS485 BUS
```

---

# 5. Modbus Philosophy

System menggunakan Modbus RTU untuk:
- framing
- CRC
- ACK behavior
- timeout handling
- request/response

ACK dianggap valid ketika slave memberikan Modbus response valid.

---

# 6. Bus Ownership Rule

Master owns the bus.

Slave SHALL NOT transmit without request.

Slave hanya boleh respond ketika:
- polling
- read request
- write request
- pairing mode request

---

# 7. Addressing Scheme

Recommended:

1 = Reserved
2-246 = Normal assigned slave
247 = Pairing/default address
248-254 = Reserved/testing

Per the agreed slave contract v2.1.0, all slaves boot at address 247 because slave config is RAM-only. The master owns persistent MAC/address/profile/name/room registry data and restores known slaves after reboot.

For a known slave, recovery restores both transport identity and master-owned role state. After the slave accepts the recovered address, the master reapplies the saved Device Profile and assignment registers so the slave returns to its previous role, such as `TEMP_NODE`, `CO2_NODE`, `PRESENCE_NODE`, `RELAY_NODE`, or `IR_COMBO_NODE`.

---

# 8. Pairing Flow

For the easier operational explanation of all three field scenarios and their
actual firmware timing, use:

`RS485_Master_Discovery_Recovery_Flow_ID.md`

Startup reconnect flow:

1. START.
2. Master checks saved slave registry.
3. If saved slave exists, master tries reconnect/recovery.
4. If no saved slave exists, master does nothing automatically.
5. User starts DISCOVER only when a new/unknown slave must be assigned.

What changed: boot does not automatically imply fresh assignment.
Why it changed: saved MAC/address mappings are authoritative.
Implementation effect: pairing is a user-driven operation for unknown devices, while normal startup restores known slaves first.

1. User taps DISCOVER
2. Master pauses polling
3. Pairing countdown starts
4. Master scans pairing/default address 247
5. Slave is already listening on address 247 after boot
6. Master reads identity registers
7. Master reads MAC and firmware metadata
8. UI shows MAC/identity and marks unknown MAC as `UNPAIRED_DEVICE_DETECTED`
9. User assigns address/name/room and selects Device Profile
10. Master writes capability/profile registers to `0x0010..0x0017`
11. Master writes new address to `NODE_ADDRESS 0x0000`
12. Slave applies address immediately and leaves address 247
13. Master stores MAC, assigned address, Device Profile, device name, and room
14. Polling resumes on the assigned address

The slave-side save-config signal is removed from the v2.1 slave contract. Slave address and capability persistence belong to the master; slave remains RAM-only.

Recovery flow after slave reboot:

1. Slave reboots and returns to address 247
2. Master has saved MAC -> assigned address from registry
3. Known assigned address does not respond
4. Master writes recovery MAC/address to `247:0x00F4 length 4`
5. Matching slave applies the recovered address
6. Non-matching slaves ignore the recovery write and remain at 247
7. Master ignores Modbus response collision/error for this recovery write only
8. Master confirms recovery by polling the recovered assigned address
9. Master writes saved Device Profile / assignment registers `0x0010..0x0017` to the recovered slave

What changed: recovery is the normal path for known RAM-only slaves after reboot.
Why it changed: slaves boot at 247 but master-owned registry remembers their assigned identity.
Implementation effect: master should not wipe or reassign known slaves unless recovery fails, the user deletes/forgets the slave, or the user intentionally re-pairs them. A deleted/forgotten slave is removed from the master registry and will not auto-recover until paired again.

---

# 9. Slave Identity Registers

0x0000 NODE_ADDRESS
0x0001 FW_VERSION
0x0002 MAC_0_1
0x0003 MAC_2_3
0x0004 MAC_4_5

MAC registers are the stable identity source used by pairing and recovery. Master firmware may derive an internal UID from MAC for registry persistence.

---

# 10. Capability Registers

0x0010 TEMP_SENSOR_ASSIGNMENT
0x0011 LUX_SENSOR_ASSIGNMENT
0x0012 CO2_SENSOR_COUNT
0x0013 PRESENCE_SENSOR_ASSIGNMENT
0x0014 RELAY_ASSIGNMENT
0x0015 IR_PROJECTOR_ENABLE
0x0016 IR_AC_1_ENABLE
0x0017 IR_AC_2_ENABLE

Capability/profile registers are written by master according to the selected Device Profile. Reading these registers from a slave is useful for sync/diagnostics, but it SHALL NOT override the master's profile assignment after the user has configured it.

When the user presses SAVE in Slave Detail, the master writes the current profile/config to `0x0010..0x0017`. For temperature:

```text
TEMP_SENSOR_ASSIGNMENT bit 3 -> Temp 1 active
TEMP_SENSOR_ASSIGNMENT bit 2 -> Temp 2 active
TEMP_SENSOR_ASSIGNMENT bit 1 -> Temp 3 active
TEMP_SENSOR_ASSIGNMENT bit 0 -> Temp 4 active
```

The master SHALL NOT write a slave-side save-config signal in v2.1.

Firmware V2.1 assignment validation:

- What changed: capability assignment registers are the v2.1 wire format, while Device Profile is master-owned policy.
- Why it changed: v2.1 needs policy-blind slaves and production-oriented profiles.
- Implementation effect: master validates profile choices and writes matching v2.1 registers. `IR_COMBO_NODE` may enable AC 1, AC 2, and Projector together.

---

# 11. Capability Bitmask

This bitmask is an internal master/UI convenience model, not the v2.1 wire contract.

enum CapabilityBit {
    CAP_TEMP
    CAP_CO2
    CAP_PRESENCE
    CAP_AC_IR
    CAP_PROJECTOR_IR
    CAP_LIGHT_RELAY
    CAP_LUX
    CAP_LCD_CTRL
}

---

# 12. Device Profile Example

TEMP_NODE assigned by master:

```text
0x0010 TEMP_SENSOR_ASSIGNMENT = 0b1000
0x0011 LUX_SENSOR_ASSIGNMENT  = 0b0000
```

Runtime register:

```text
0x0100 TEMP_1_X10
```

---

# 13. Legacy / V1 Multi Sensor Slave Example

Slave B assigned by master:

```text
Legacy V1-style bundle:
Temperature + CO2 + Presence
```

Runtime registers:

```text
0x0100 TEMP_1_X10
0x0101 TEMP_2_X10
legacy grouped CO2 register
legacy grouped presence register
```

Firmware V2 status:

- What changed: this example is no longer a valid primary V2.1 assignment.
- Why it changed: V2.1 uses Device Profiles and the v2.1 register map.
- Implementation effect: keep this only for understanding old documents or test fixtures. New V2.1 configs should use Device Profiles, including `IR_COMBO_NODE` where production hardware needs a combined IR node.

---

# 14. Runtime Role Assignment

Slave SHALL NOT permanently know:
- dashboard slot
- room role
- AC purpose
- projector purpose

Master assigns runtime behavior dynamically.

---

# 15. Dashboard Philosophy

Dashboard membaca logical slot.

Dashboard tidak membaca slave secara langsung.

---

# 16. Temperature Dashboard Behavior

Home dashboard hanya menampilkan:

AVG TEMPERATURE

AVG dihitung dari seluruh temp slot valid.

Jika user tap AVG:
show detailed 4-point temperature page.

---

# 17. Dynamic Control Visibility

IF AC capability detected:
show AC control

IF projector capability detected:
show projector control

IF LCD capability detected:
show LCD control

Jika tidak tersedia:
hide widget completely

Current firmware alignment:

```text
Implemented through DashboardModel:
- Temperature slots
- CO2
- Lux from v2.1 registers when the selected profile enables it
- Human Presence
- AC availability
- Projector availability

Implemented as direct registry-derived dashboard visibility:
- Light Relay / LED

Capability visible in Slave Detail / feature checklist, but not yet a main-dashboard logical control:
- LCD Control
```

Until `LOGICAL_LIGHT_RELAY_CONTROL` and/or `LOGICAL_LCD_CONTROL` are added to `DashboardLogicalId`, Light Relay and LCD SHALL be documented as partially integrated capabilities.

---

# 18. Logical Mapping Example

TEMP_SLOT_1 -> Slave A TEMP_1
TEMP_SLOT_2 -> Slave A TEMP_2
TEMP_SLOT_3 -> Slave B TEMP_1
TEMP_SLOT_4 -> Slave B TEMP_2

Dashboard hanya membaca slot.

Temperature slot numbering is fixed:

```text
TEMP_SLOT_1 / Point 1 -> Temp 1 -> 0x0100
TEMP_SLOT_2 / Point 2 -> Temp 2 -> 0x0101
TEMP_SLOT_3 / Point 3 -> Temp 3 -> 0x0102
TEMP_SLOT_4 / Point 4 -> Temp 4 -> 0x0103
```

The same temperature channel number SHALL NOT be enabled on multiple slaves at the same time from the Slave Detail UI. If Temp 1 is checked on one slave, Temp 1 appears Unavailable on other slaves until it is unchecked.

Current firmware logical slots:

```text
TEMP_SLOT_1
TEMP_SLOT_2
TEMP_SLOT_3
TEMP_SLOT_4
CO2_MAIN
LUX_MAIN
HUMAN_PRESENCE_MAIN
AC_CONTROL
PROJECTOR_CONTROL
```

Current firmware does not yet define dedicated logical dashboard slots for:

```text
LIGHT_RELAY_CONTROL
LCD_CONTROL
```

Therefore, LED/Light Relay dashboard visibility currently comes from enabled online slave capability, not from a persisted logical mapping slot.

Firmware V2 mapping effect:

- What changed: each slave source is interpreted through master-owned Device Profile policy.
- Why it changed: MQTT V2 publishes per sensor type and needs clean source ownership.
- Implementation effect: auto-map should use the selected Device Profile. `IR_COMBO_NODE` may fill AC and Projector control roles; temperature profile may expose up to four temperature channels.

---

# 19. NULL / Offline Behavior

Jika slot belum memiliki source:
display "--"

Jika slave offline:
mapping tetap ada
tetapi value menjadi invalid/stale

Jika slave online kembali:
mapping restored automatically

---

# 20. Sensor Register Layout

0x0100 TEMP_1_X10
0x0101 TEMP_2_X10
0x0102 TEMP_3_X10
0x0103 TEMP_4_X10

Lux:
0x0104 LUX_1_LX
0x0105 LUX_2_LX
0x0106 LUX_3_LX
0x0107 LUX_4_LX

Air Quality:
0x0108 CO2_PPM

Presence:
0x0109 PRESENCE_1_STATE
0x010A PRESENCE_2_STATE
0x010B PRESENCE_3_STATE
0x010C PRESENCE_4_STATE

Relay:
0x010D RELAY_1_STATE
0x010E RELAY_2_STATE

Master polling SHOULD read the contiguous v2.1 runtime block and parse values using Device Profile and assignment registers:

```text
Sensor/state block: read 0x0100 length 15
```

Invalid/unavailable values SHALL follow the sentinel values from the slave contract.

---

# 21. Control Register Layout

AC:
0x0200 AC_1_POWER
0x0201 AC_1_SET_TEMP
0x0202 AC_1_MODE
0x0203 AC_2_POWER
0x0204 AC_2_SET_TEMP
0x0205 AC_2_MODE
0x0206 AC_1_COMMAND_STATUS
0x0207 AC_2_COMMAND_STATUS

Projector:
0x0210 PROJECTOR_POWER
0x0211 PROJECTOR_INPUT
0x0212 PROJECTOR_COMMAND_STATUS

Current firmware control implementation status:

```text
Implemented/target active write paths SHALL route through RS485 Manager and the v2.1 register map:
- Relay writes use 0x010D..0x010E.
- AC writes use 0x0200..0x020D when IR_COMBO_NODE is assigned. The extension
  includes power, set temperature, mode, fan speed, vertical swing, and optional
  horizontal swing for AC 1 and AC 2.
- Projector writes use 0x0210..0x0211 when IR_COMBO_NODE is assigned.

Command status read paths:
- 0x0206 AC_1_COMMAND_STATUS
- 0x0207 AC_2_COMMAND_STATUS
- 0x0212 PROJECTOR_COMMAND_STATUS
```

Temporary AC dashboard behavior:

- The dashboard/control panel shows one AC control for now.
- If AC 1 and AC 2 are both exposed by `IR_COMBO_NODE`, master mirrors the same power/set-temperature/mode command to AC 1 and AC 2.
- Separate AC 1 / AC 2 controls are a future UI update.

Command status note:

- AC/projector command status means command execution result only.
- It does not prove the physical AC/projector state because IR is one-way.

---

# 22. Reliability Philosophy

Master tetap wajib maintain:
- online state
- degraded state
- timeout counter
- exception counter
- retry policy

---

# 23. Polling Philosophy

Recommended:
- Sensor polling: 500-1000 ms
- Identity polling: 5-10 s
- Config sync: on demand

Current firmware values:

```text
Sensor polling interval:      1000 ms
Identity sync interval:       10000 ms
Capability sync interval:      5000 ms
Pairing scan interval:          700 ms
Modbus response timeout:        100 ms
Retry count:                      1
Offline timeout:              5000 ms
Degraded fail threshold:          3 consecutive failures
Offline fail threshold:           5 consecutive failures
Auto-recovery interval:          10000 ms per saved slave
RS485 task loop interval:           10 ms
```

Timing interpretation:

- One `1000 ms` polling interval advances one saved registry entry; it does not
  poll every slave simultaneously.
- A slave is normally revisited after approximately
  `saved registry entry count x 1000 ms`.
- Failed thresholds count each Modbus attempt. With one configured retry, one
  fully failed transaction can contribute two failed attempts.
- A powered slave that only loses its RS485 cable remains on its assigned
  address and reconnects through normal polling. Address-247 recovery is mainly
  needed after a known RAM-only slave reboots.

---

# 24. Slave Design Guideline

Slave SHOULD:
- expose raw sensor data
- expose v2.1 capability/profile registers required by `docs/From_SLave/RS485_Modbus_Slave_Firmware_Contract v2.md`
- respond to Modbus request
- avoid orchestration logic
- remain RAM-only and policy-blind

Slave SHOULD NOT:
- decide dashboard slot
- decide room assignment
- decide AC purpose
- decide projector purpose
- decide Device Profile

---

# 25. Master Design Guideline

Master SHALL own:
- orchestration
- mapping
- dashboard logic
- role assignment
- visibility logic
- AVG calculation
- control routing

---

# 26. Final Engineering Principles

1. Dashboard SHALL NOT hardcode slave address.
2. Dashboard SHALL use logical slot only.
3. Slave SHALL expose capability, not final role.
4. Master SHALL assign runtime purpose.
5. Firmware V2.1 Device Profile assignment SHALL be supported.
6. Legacy bundled multi-capability assumptions SHALL NOT be the primary design target.
7. Dynamic dashboard widget visibility SHALL be supported.
8. AVG temperature SHALL be primary dashboard temperature.
9. Detailed temperature SHALL exist on detail page.
10. Modbus RTU SHALL be primary RS485 transport.
11. UI command requests SHALL go through RS485 Manager callbacks/state flags.
12. Runtime sensor data SHALL flow from RS485 Manager into Slave Registry, then Mapping Manager, then Dashboard Model.
13. Any capability shown on the main dashboard SHOULD eventually be represented by a logical mapping slot. Current exception: Light Relay / LED is still registry-derived.
14. Compatibility structs/commands in firmware SHALL be treated as API/debug wrappers around Modbus operations, not a separate active wire protocol.
15. Lux SHALL use v2.1 assignment/runtime registers when enabled by the selected Device Profile.
16. `IR_COMBO_NODE` SHALL support AC 1, AC 2, and Projector through one IR-capable slave.

---

# END DOCUMENT
