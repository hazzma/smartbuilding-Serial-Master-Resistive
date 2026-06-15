# UIUX.md
# Smart Building Master S3 - Dashboard UI/UX Specification

## Document Purpose

Dokumen ini mendefinisikan rancangan UI/UX untuk HMI Smart Building Master S3 dan aplikasi/dashboard pendamping untuk Smart Building Firmware V2.

Fokus utama UI baru:
- besar
- sederhana
- mudah dibaca oleh orang normal
- cocok untuk layar 480x320
- adaptive berdasarkan fitur/slave yang terdeteksi
- background/wallpaper first, dengan UI overlay transparan
- tetap punya logic yang jelas untuk implementasi firmware
- app/dashboard pendamping memakai navigasi sederhana: Home, Devices, Settings

Dokumen ini dipisahkan dari FSD agar FSD tetap menjadi spesifikasi sistem, sementara dokumen ini menjadi spesifikasi pengalaman pengguna dan aturan layout.

Firmware V2 change note:
- What changed: UI/app docs now treat per-sensor MQTT topics as the primary dashboard input model.
- Why changed: Firmware V2 publishes each sensor/control family separately instead of relying on one large master state topic.
- Implementation effect: app and dashboard implementations SHALL merge per-topic updates into their local view model before rendering.

---

# 1. Core UI Philosophy

Dashboard utama SHALL be simple and large.

User utama tidak perlu melihat:
- detail slave
- register
- address
- mapping
- debug counter
- semua sensor sekaligus

Dashboard hanya menampilkan informasi penting yang aktif dan tersedia.

Design goal:

```text
User cukup lihat:
- jam
- suhu rata-rata
- status koneksi
- kontrol yang tersedia
- warning ringan jika ada sensor penting offline
```

Engineer/admin tetap bisa masuk ke menu detail melalui Settings.

---

# 1.1 Companion App Navigation

The Flutter/app dashboard SHALL stay simple and use three primary areas:

- Home
- Devices
- Settings

Home:
- shows the selected class/room dashboard
- renders live sensor values and actuator states from the current class/room
- keeps the UI focused on normal operation, not engineering details

Devices:
- manages discovered MQTT devices/masters
- lets the user choose which master/class/room is displayed
- manages display-oriented settings such as visible device labels or dashboard ordering

Settings:
- manages MQTT broker configuration
- manages topic/class naming configuration
- manages app-level preferences that are not part of daily dashboard control
- provides Device Info on a second Settings page for firmware info and editable class/room name

Firmware V2 change note:
- What changed: the app navigation is reduced to Home, Devices, and Settings.
- Why changed: the app should be easy for classroom/building users and should not expose firmware internals as primary navigation.
- Implementation effect: older app flows that depend on one generic device-detail state page should be treated as secondary or legacy behavior.

---

# 1.2 Settings And Device Info Navigation

Settings SHALL be divided into two static pages. Navigating between Page 1 and Page 2 is performed using horizontal swipe gestures (left swipe to transition from Page 1 to Page 2, and right swipe to transition back). The screen SHALL show page dot indicators at the bottom center to display the active page. There are no NEXT/PREV buttons.

Recommended Settings Page 1 layout:

```text
+------------------------------------------------+
| Settings                               [ BACK ]|
+------------------------------------------------+
| Network Priority                       WiFi    |
+------------------------------------------------+
| [ WiFi Setup ]   [ LAN Setup ]   [ Slaves ]    |
| [ Connected  ]   [ Setup     ]   [ Fieldbus]   |
+------------------------------------------------+
| RS485: Fieldbus Online                         |
| Swipe left for MQTT & Device Info              |
+------------------------------------------------+
|                     o   .                      |
+------------------------------------------------+
```

Recommended Settings Page 2 layout:

```text
+------------------------------------------------+
| Settings 2                             [ BACK ]|
+------------------------------------------------+
| MQTT Setup                                     |
| Broker Connected / MQTT Setup                  |
+------------------------------------------------+
| Device Info                                    |
| Name / Class Room                              |
+------------------------------------------------+
|                     .   o                      |
+------------------------------------------------+
```

Clicking `MQTT Setup` SHALL open a dedicated summary screen first. It SHALL NOT immediately open the broker keyboard editor. Clicking `Device Info` SHALL open the dedicated Device Info screen.

MQTT Setup summary layout:

```text
+------------------------------------------------+
| MQTT Setup                              [BACK] |
+------------------------------------------------+
| Broker / Host (tap to edit)                    |
| broker.example.com                             |
+------------------------------------------------+
| Port: 8883                 | TLS: ON            |
+------------------------------------------------+
| Username (tap to edit)                         |
| classroom-master                               |
+------------------------------------------------+
| Password (tap to edit)                         |
| ********                                       |
+------------------------------------------------+
| MQTT OFFLINE             [SAVE & RECONNECT]    |
+------------------------------------------------+
```

MQTT Setup behavior:
- Broker, port, username, and password fields open the keyboard only after their field card is tapped.
- TLS is changed using its card toggle.
- Password remains masked on the summary screen.
- `SAVE & RECONNECT` persists the active values and requests MQTT reconnect using the saved configuration.
- Broker, port, TLS mode, username, and password SHALL survive reboot through ESP32-S3 NVS/Preferences.
- The current implementation stores these MQTT connection fields in NVS namespace `device_cfg`.

Device Name and Class Name remain editable from Device Info. These editable properties are stored dynamically in NVS Preferences and persisted across device reboots.

Class/room name behavior:

```text
If class name = HD01:
    generated topic labels include:
    HD01/data/co2
    HD01/data/temp
    HD01/data/led
    HD01/control/led

If class name = LA2:
    generated topic labels include:
    LA2/data/co2
    LA2/data/temp
    LA2/data/led
    LA2/control/led
```

What changed: Settings Page 2 now acts as an entry page for dedicated MQTT Setup and Device Info screens. MQTT Setup displays the full active connection summary before any edit action.

Why changed: Users must be able to inspect the complete MQTT configuration before editing a single field, and saved connection values must remain visible after reboot.

Implementation effect: UI persists MQTT server, port, TLS mode, username, password, device name, and class name to NVS Preferences. MQTT reconnect uses the saved connection fields. Settings page transitions remain horizontal swipe gestures.

WiFi Setup persistence behavior:

```text
When WiFi Setup opens:
    load the last saved SSID and password from NVS namespace wifi_cfg
    show the saved SSID
    show the saved password masked unless SHOW is active

When CONNECT is pressed:
    save the requested SSID and password to wifi_cfg
    update the WiFi Setup form values
    start the connection attempt
```

The WiFi Setup form SHALL NOT reset to hardcoded presentation credentials when reopened.

---

# 2. Screen Resolution

Target display:

```text
480 x 320 landscape
```

Coordinate assumption:

```text
x: 0 - 479
y: 0 - 319
```

---

# 3. Wallpaper / Background Support

UI SHALL support wallpaper/background image.

Firmware SHALL provide a way to include a wallpaper asset. Current implementation uses the generated asset in:

```text
/Aset/Wallpaper.cpp
/src/wallpaper_asset.cpp
/src/wallpaper_asset.h
```

Alternative future storage may use:

```text
/assets/wallpaper_bg.h
/assets/wallpaper_bg.cpp
```

or:

```text
/data/wallpaper.raw
/data/wallpaper.rgb565
```

Recommended wallpaper format:

```text
RGB565
480x320
```

Wallpaper rules:

```text
1. Wallpaper SHALL be rendered first.
2. UI cards/widgets SHALL be drawn above wallpaper only when the screen needs a control surface.
3. Text readability SHALL remain priority.
4. If wallpaper is too bright/dark, widget cards SHALL use translucent or solid background.
5. Wallpaper SHALL NOT block UI performance.
6. Dashboard top/status area SHALL remain visually transparent over wallpaper.
7. Dashboard empty state SHALL NOT draw a large opaque card/panel.
8. Admin/setup screens MAY use translucent or solid panels for form/list readability.
```

---

# 4. Top Bar Layout

Top bar touch/status affordances SHALL always be available, but the bar itself SHALL NOT use a full-width opaque background on the dashboard.

The dashboard top area is an overlay, not a panel. Icons/text may render directly over the wallpaper. Small local chip backgrounds are allowed only when needed for readability, but the old full-width black/status strip SHALL NOT be used on the main dashboard.

Top left:

```text
Settings icon/menu affordance
HH:MM when no large centered clock is visible
```

Top right small status icons:

```text
WiFi
LAN
Slave Port / Fieldbus
```

Example:

```text
+------------------------------------------------+
| =   14:32                       WiFi LAN BUS   |
+------------------------------------------------+
```

If the dashboard is in empty/status mode and a large centered clock is shown, the small top-left clock SHALL be hidden to avoid duplicate time display.

The top-left Settings icon remains the only dashboard menu affordance. A separate bottom `MENU` button SHALL NOT be shown when the top-left Settings icon is present.

Status icon behavior:

```text
WiFi icon:
    visible if WiFi module enabled
    active if connected

LAN icon:
    visible if LAN module enabled
    active if connected

Slave/Bus icon:
    visible if fieldbus feature enabled
    active if at least one slave online
```

---

# 4.1 Top Icon Asset Requirements

Future bitmap icon assets for the dashboard top overlay SHOULD be placed under:

```text
/Aset/icons/
```

Preferred source format:

```text
PNG
transparent background
64 x 64 px source size
simple high-contrast shape
white or near-white foreground for default/topbar use
```

Required icons:

```text
settings_64.png
wifi_on_64.png
wifi_off_64.png
```

Optional future icons:

```text
lan_on_64.png
lan_off_64.png
bus_on_64.png
bus_off_64.png
co2_64.png
```

Runtime render target on 480x320 screen:

```text
Settings icon: 24 x 24 px visible icon inside ~48 x 48 px touch area
WiFi icon:     20-24 x 20-24 px visible icon inside top status area
```

If assets are converted into firmware C arrays, conversion SHOULD preserve transparency as either an explicit alpha/mask bitmap or a precomposited RGB565 variant matching the top overlay background style.

---

# 5. Small CO2 Indicator

CO2 SHALL appear as a small compact indicator on the top overlay row if valid, aligned with the Settings icon/time and WiFi/LAN/BUS indicators.

Example:

```text
CO2 720 ppm
```

Rules:

```text
IF co2_valid == true
    show small CO2 chip
ELSE
    hide CO2 chip
ENDIF
```

---

# 6. Dashboard Capability Flags

Dashboard layout is adaptive based on available capabilities.

Detected capability flags:

```cpp
bool has_temp;
bool has_ac;
bool has_projector;
bool has_led;
bool has_co2;
bool has_lux;
bool has_human_presence;
```

---

# 7. Temperature Widget

If temperature exists, dashboard SHALL show average temperature.

```text
AVG Temperature = average(valid temperature slots)
```

If user taps AVG temperature, UI SHALL open Temperature Detail screen.

Temperature point identity SHALL be stable:

```text
Point 1 = Temperature 1 = Modbus 0x0100
Point 2 = Temperature 2 = Modbus 0x0101
Point 3 = Temperature 3 = Modbus 0x0102
Point 4 = Temperature 4 = Modbus 0x0103
```

The UI SHALL NOT compact temperature values upward. If only Temperature 2 is assigned, it appears as Point 2, while Point 1 remains `-- / Not assigned`.

---

# 8. Temperature Large-Only Mode

If only temperature is available:

```text
has_temp == true
has_ac == false
has_projector == false
has_led == false
```

Layout:

```text
+------------------------------------------------+
| 14:32                              WiFi LAN BUS|
|                                                |
|                                                |
|                 27.8°C                         |
|              Average Room Temp                 |
|                                                |
|          Tap for 4-point detail                |
|                                                |
+------------------------------------------------+
```

Temperature SHALL be large and centered.

---

# 9. Temperature + Other Controls Mode

If temperature exists and at least one control exists:

```text
has_temp == true
AND
(has_ac || has_projector || has_led)
```

Then temperature widget SHALL shrink slightly and move into the adaptive widget grid.

When AVG temperature, AC, Projector, and LED are all visible, the implemented
V2.8 layout prioritizes the complete AC control surface:

```text
          AVG Temperature (transparent, tap for detail)
AC control, tall   Projector
AC Swing / Fan     LED 1 | LED 2
```

Rules:
- Average temperature remains clickable but SHALL not consume a full opaque
  card in this dense layout.
- AC remains tall on the left and keeps power, target, UP/DOWN, Swing, and Fan.
- Projector is placed on the upper-right.
- LED 1 and LED 2 are placed side-by-side on the lower-right when two relay
  channels are available.

Special layout for Temperature + AC + Projector without LED:

```text
+------------------------------------------------+
| Settings / Time / Status                       |
| [ AC control, tall ]   [ Projector square ]    |
| [ UP ] [ DOWN ]        [                  ]    |
| [Swing] [ Fan ]        [ Temperature      ]    |
+------------------------------------------------+
```

Rules:
- AC remains on the left.
- Projector is a square control on the upper-right.
- Average temperature is placed below the projector.
- AC `UP` and `DOWN` touch targets are enlarged for easier operation.

Current target example:

```text
Settings 14:32  CO2 720ppm     WiFi LAN BUS

[ AVG Temperature ]  [ AC Target       ON ]
[ 27.8 C / Avg    ]  [ 24 C              ]

[ Projector ON/OFF ] [ Lamps 1-4        ]
```

Example:

```text
+------------------------------------------------+
| 14:32                              WiFi LAN BUS|
|                                      CO2 720ppm |
|                                                |
|  27.8°C        AC Target                       |
|  Avg Temp      24°C                            |
|  Detail        [ UP ] [ DOWN ]                 |
|                                                |
|             [ Projector ON/OFF ]               |
|                                                |
+------------------------------------------------+
```

---

# 10. AC Widget Behavior

AC widget SHALL appear only if AC capability/control endpoint is detected and mapped.

```text
IF has_ac == true
    show AC target temperature widget
ELSE
    hide AC widget
ENDIF
```

AC widget content:

Current AC card target:

```text
AC Target        [ ON / OFF ]
24 C             [         ]
[UP] [DOWN]
[Swing] [Fan]
```

Legacy simplified shape:

```text
AC Target
24°C
[UP] [DOWN]
```

Initial simplified UI:

```text
Target temperature + UP/DOWN
```

Current target layout SHOULD stack AC label above the target value, with a large colored power badge on the right spanning the label/value area:

```text
AC Target        [ ON / OFF ]
24 C             [         ]
[UP] [DOWN]
```

Power badge color:
- ON uses green fill
- OFF uses red fill

Expanded AC control behavior:
- The power badge SHALL remain compact; the entire available AC card height SHALL NOT become one oversized ON/OFF button.
- `UP` and `DOWN` SHALL use larger touch targets in expanded layouts.
- A bottom control row SHALL show Swing and Fan mode.
- Current Swing sequence is `AUTO -> UP -> MID -> DOWN -> AUTO`.
- Current Fan sequence is `AUTO -> LOW -> MID -> HIGH -> AUTO`.
- Swing and Fan are temporary local UI states only until the RS485 slave contract defines their command/status registers.

Temporary AC 1+2 behavior:

```text
If the mapped device profile is IR_COMBO_NODE:
    AC 1 capability may be enabled.
    AC 2 capability may be enabled.
    The current dashboard still shows one AC control panel.
    Power, target temperature, and mode commands from that panel are mirrored to AC 1 and AC 2 when both are enabled.
```

This is temporary behavior for the current dashboard. Future UI MAY split AC 1 and AC 2 into separate panels, but the active dashboard requirement is one AC panel with mirrored AC 1+2 commands.

---

# 11. Projector Widget Behavior

Projector control SHALL appear only if projector capability is detected and mapped.

```text
IF has_projector == true
    show Projector ON/OFF
ELSE
    hide Projector widget
ENDIF
```

Default placement:

```text
center bottom
```

Special AC + Projector layout without temperature or LED:

```text
AC control: large/tall card on the left
Projector: large square control centered vertically on the right
```

The projector SHALL NOT be rendered as a thin horizontal button in this combination.

Projector control SHALL use a large touch target and SHALL visually match the scale of other active dashboard controls.

V2.7.1 projector verification state display:

```text
OFF
POWERING_ON
VERIFIED_ON
RETRYING
NO_LUX
CHECK_LUX
CHECK_PROJECTOR
```

UI behavior:
- `POWERING_ON` / `RETRYING` SHOULD show a pending visual state while keeping
  the command responsive.
- `VERIFIED_ON` SHOULD render as normal ON.
- `NO_LUX` SHALL keep the normal green ON card because the command was sent and
  Lux verification is optional. Only the `NO LUX` status text SHALL be red so
  users can distinguish missing feedback without mistaking the projector as
  OFF.
- `CHECK_LUX` SHOULD render as ON with a warning cue, because at least one Lux
  channel verified ON while another expected channel was invalid or unchanged.
- `CHECK_PROJECTOR` SHOULD render as ON with an error cue and expose Alert Bit 5,
  because the master sent the IR command but no Lux channel verified projector ON
  after retry.

If LED also exists:

```text
Projector moves left
LED moves right
```

---

# 12. LED/Lamp Widget Behavior

LED/Lamp control SHALL appear only if LED/Lamp capability is detected and mapped.

```text
IF has_led == true
    show LED/Lamp control
ELSE
    hide LED widget
ENDIF
```

LED/Lamp control SHALL support up to 4 lamp channels:

```text
Lamp 1
Lamp 2
Lamp 3
Lamp 4
```

If only one lamp channel is available, the widget MAY behave as a direct ON/OFF toggle.

If 2-4 lamp channels are available, the main dashboard widget SHALL make channel selection explicit before toggling a lamp. Acceptable UI patterns:
- segmented channel selector `1 2 3 4` inside the lamp widget
- tap aggregate Lamp widget to open a lamp detail sheet/page with four large toggles
- compact 2x2 mini-toggle grid when there is enough space

The UI SHALL NOT silently toggle all lamps when the user expected one selected channel.

Current V2.8 implementation:
- A relay slave with two channels renders separate `LED 1` and `LED 2` buttons.
- `LED 1` writes Relay 1 register `0x010D`.
- `LED 2` writes Relay 2 register `0x010E`.
- Each button displays the state read back from its own relay register.
- Individual channel controls exist only on the local master touchscreen.
- The mobile app SHALL expose one aggregate Lamp control, not separate LED 1
  and LED 2 controls.
- Every mobile app/server `control/led` MQTT command and schedule action SHALL
  control Relay 1 and Relay 2 together.

Default placement logic:

```text
IF has_projector == false
    LED/Lamp widget centered
ELSE
    LED/Lamp widget right side
ENDIF
```

LED/Lamp control SHALL use a large touch target and SHALL visually match the scale of other active dashboard controls. Per-channel toggles SHALL still respect the minimum 48 x 48 px touch target where possible.

---

# 13. Projector + LED Layout Rule

Only Projector:

```text
Projector widget centered bottom
```

Only LED:

```text
LED/Lamp widget centered bottom
```

Projector + LED:

```text
Projector widget bottom-left
LED/Lamp widget bottom-right
```

Example:

```text
+------------------------------------------------+
|                                                |
|                                                |
|                                                |
|        [ Projector ON/OFF ] [ Lamps 1-4 ]      |
+------------------------------------------------+
```

---

# 14. No Temperature Mode

If no temperature exists:

```text
has_temp == false
```

Dashboard SHALL not reserve a large empty temperature space.

No Temp + AC + LED:

```text
AC left
LED right
```

No Temp + Only One Control:

```text
single control centered
```

Example:

```text
+------------------------------------------------+
| 14:32                              WiFi LAN BUS|
|                                                |
|                                                |
|                 [ Lamps 1-4 ]                  |
|                                                |
+------------------------------------------------+
```

---

# 15. Main Dashboard Layout Decision Tree

```text
START
  |
  v
Check has_temp
  |
  +-- YES --------------------------------+
  |                                       |
  |  Check has_ac/projector/led            |
  |       |                               |
  |       +-- NONE ---------------------> Temp Large Center Mode
  |       |
  |       +-- ANY ----------------------> Temp Compact + Dynamic Controls
  |
  +-- NO ---------------------------------+
          |
          v
       Check controls
          |
          +-- none ---------------------> Empty/Status Mode
          |
          +-- one control --------------> Single Control Center
          |
          +-- multiple controls --------> Split Control Layout
```

---

# 16. Widget Placement Algorithm

Pseudo-code:

```cpp
DashboardLayout compute_layout(DashboardModel d) {
    bool has_temp = d.avg_temp_valid;
    bool has_ac = d.ac_available;
    bool has_projector = d.projector_available;
    bool has_led = d.led_available;

    if (has_temp) {
        if (!has_ac && !has_projector && !has_led) {
            return LAYOUT_TEMP_CENTER_LARGE;
        }

        return LAYOUT_TEMP_COMPACT_WITH_CONTROLS;
    }

    int control_count = count_true(has_ac, has_projector, has_led);

    if (control_count == 0) {
        return LAYOUT_STATUS_EMPTY;
    }

    if (control_count == 1) {
        return LAYOUT_SINGLE_CONTROL_CENTER;
    }

    return LAYOUT_MULTI_CONTROL_SPLIT;
}
```

---

# 17. Dynamic Control Positioning

Pseudo-code:

```cpp
void place_controls() {
    if (has_temp && has_ac && has_projector && !has_led) {
        ac_rect = LEFT_TALL_CARD;
        projector_rect = RIGHT_TOP_SQUARE;
        temperature_rect = RIGHT_BOTTOM_CARD;
    }

    else if (!has_temp && has_ac && has_projector && !has_led) {
        ac_rect = LEFT_TALL_CARD;
        projector_rect = RIGHT_CENTER_SQUARE;
    }

    if (has_projector && has_led) {
        projector_rect = LEFT_BOTTOM_CARD;
        led_rect = RIGHT_BOTTOM_CARD;
    }

    else if (has_projector) {
        projector_rect = CENTER_BOTTOM_CARD;
    }

    else if (has_led) {
        led_rect = CENTER_BOTTOM_CARD;
    }

    if (!has_temp && has_ac && has_led) {
        ac_rect = LEFT_CENTER_CARD;
        led_rect = RIGHT_CENTER_CARD;
    }

    else if (!has_temp && has_ac && !has_led && !has_projector) {
        ac_rect = CENTER_CARD;
    }
}
```

---

# 18. Swipe Gesture Detail Panel

Dashboard SHALL support swipe gesture to reveal secondary environmental data.

Secondary data:

```text
Lux
Human Presence
```

Recommended gesture:

```text
Swipe up reveals Environmental Detail Panel.
Swipe down hides it.
```

Environmental Detail Panel layout:

```text
+------------------------------------------------+
| Environment Detail                    [CLOSE]  |
+------------------------------------------------+
| Lux              420 lx                        |
| Human Presence   Detected                      |
+------------------------------------------------+
```

Rules:

```text
IF lux_valid == false
    display "--"
ENDIF

IF human_presence_valid == false
    display "Unknown"
ENDIF
```

---

# 19. Temperature Detail Screen

Opened by tapping AVG temperature.

```text
+------------------------------------------------+
| Temperature Detail                    [BACK]   |
+------------------------------------------------+
| Average: 27.8°C                                |
+------------------------------------------------+
| Point 1     27.2°C     Room Node A / Temp 1    |
| Point 2     28.0°C     Room Node A / Temp 2    |
| Point 3     --         Not assigned            |
| Point 4     --         Not assigned            |
+------------------------------------------------+
```

Rules:

```text
IF temp slot valid
    show value
ELSE IF mapped but stale
    show "STALE / Offline"
ELSE
    show "-- / Not assigned"
ENDIF
```

Point labels SHALL stay fixed:

```text
Point 1 shows Temperature 1 only.
Point 2 shows Temperature 2 only.
Point 3 shows Temperature 3 only.
Point 4 shows Temperature 4 only.
```

This is true even when only one or two temperature channels are assigned.

---

# 19.1 Slave Feature Assignment UI

Slave Detail SHALL treat feature rows as master-side assignment controls.

Firmware V2 capability rule:

Slave configuration SHALL use master-owned Device Profile selection and enforcement.

Device Profile choices:
- `TEMP_NODE`
- `PRESENCE_NODE`
- `CO2_NODE`
- `RELAY_NODE`
- `IR_COMBO_NODE`

Profile behavior:
- `TEMP_NODE` enables Temperature rows and MAY enable optional Lux.
- `PRESENCE_NODE` enables Human Presence rows and MAY enable optional Lux.
- `CO2_NODE` enables CO2 rows and MAY enable optional Lux.
- `RELAY_NODE` enables Lamp/Relay rows and MAY enable optional Lux.
- `IR_COMBO_NODE` enables AC 1, AC 2, and Projector rows together and MAY enable optional Lux.

Lux is defined by the v2.1 slave contract:
- assignment register: `LUX_SENSOR_ASSIGNMENT 0x0011`
- runtime registers: `LUX_1_LX..LUX_4_LX 0x0104..0x0107`

Rules:

```text
Device Profile is required before SAVE is enabled.
Supported Device Profile choices are Available by default.
Checked means assigned/enabled by the master.
Unchecked means not assigned by the master.

When a Device Profile is selected:
    Rows allowed by that profile become visible/editable.
    Non-profile feature rows outside that profile are hidden from the normal row list.
    Locked profile rows may still show as unavailable when another online slave already owns the exclusive role/slot.

When the currently selected Device Profile is tapped again:
    Clear the profile selection.
    Clear master-owned enabled/checked rows for that slave.
    Return the detail screen to UNASSIGNED state.

If IR_COMBO_NODE is selected:
    AC 1, AC 2, and Projector rows are profile-valid.
    The dashboard still renders one AC panel for now.
    AC 1+2 commands are mirrored by the current dashboard behavior.

If the Device Profile changes:
    Clear assignments that are no longer valid for the new profile.
    Keep name, room, MAC, and assigned address unless the user changes them.
```

SAVE behavior:

```text
Tap SAVE:
    validate selected Device Profile against enabled rows
    write v2.1 capability assignment/count registers 0x0010..0x0017
    assign/update slave address by writing NODE_ADDRESS 0x0000 when pairing or re-pairing
    persist MAC, address, Device Profile, device name, and room in the master saved registry
```

No slave-side save action:

```text
The UI SHALL NOT expose any slave-side save/persist action.
The slave is RAM-only.
The master owns persistence through its saved device registry.
```

Temperature assignment mapping:

```text
TEMP_SENSOR_ASSIGNMENT bit 3 -> Temperature 1 / 0x0100
TEMP_SENSOR_ASSIGNMENT bit 2 -> Temperature 2 / 0x0101
TEMP_SENSOR_ASSIGNMENT bit 1 -> Temperature 3 / 0x0102
TEMP_SENSOR_ASSIGNMENT bit 0 -> Temperature 4 / 0x0103
```

Firmware V2 change note:
- What changed: the assignment UI now starts from Device Profile selection instead of the legacy unavailable-row rule.
- Why changed: Firmware V2.1 makes the master responsible for profile policy while the slave remains policy-blind.
- Implementation effect: UI should enable only rows allowed by the selected Device Profile, write v2.1 registers `0x0010..0x0017`, and persist profile/name/room/address in the master registry.

---

# 19.2 Slave Pairing And Saved Registry UI

Unknown slave state:

```text
UNPAIRED_DEVICE_DETECTED
```

Discovery behavior:

```text
If a device appears at address 247 and its MAC is not in the saved registry:
    show UNPAIRED_DEVICE_DETECTED
    show Pair Device as the primary action
    do not auto-assign address
    do not auto-select Device Profile
    do not auto-add it to the saved registry
```

Pair Device flow:

```text
Tap Pair Device:
    read identity from address 247
    user selects Device Profile
    user enters device name and room
    user confirms enabled profile-valid rows
    master writes capability registers 0x0010..0x0017
    master writes assigned address to NODE_ADDRESS 0x0000
    master saves registry entry
```

Saved device registry fields:
- Device name
- Room
- MAC
- Address
- Device Profile
- Status
- Last seen

Saved registry actions:
- Rename
- Delete
- Recover
- Re-Pair

Firmware V2.5 normal Slave Manager controls:
- `DISCOVER`
- `POLL ON` / `POLL OFF`
- device list and device detail
- `DELETE`
- `SAVE`

Manual `PING`, `READ`, `INFO`, and `MAP` controls are diagnostic/advanced
functions and SHALL NOT appear in the normal UI. Dashboard mapping remains
automatic after profile or feature changes.

Action behavior:

```text
Rename:
    edit saved device name and/or room in the master registry

Delete:
    remove the saved registry entry after confirmation
    clear mappings that reference the deleted slave UID/address
    stop automatic recovery for that slave until it is paired again

Recover:
    run known-device recovery from the saved MAC/address/profile
    after recovery, write the saved profile assignment registers back to the slave

Re-Pair:
    start a user-confirmed pairing flow for the saved device and rewrite v2.1 configuration
```

---

# 20. Dashboard States

Normal:

```text
At least one useful widget exists.
```

Empty state:

```text
+------------------------------------------------+
| =                              WiFi LAN BUS    |
|                                                |
|                    14:32                       |
|                 No Device Active               |
|          Open Menu > Slave Manager             |
|                                                |
+------------------------------------------------+
```

Empty state SHALL be transparent over wallpaper:
- no opaque empty-state card
- large centered clock
- short centered status text
- centered clock and `No Device Active` SHOULD use high-contrast light text when wallpaper is dark
- helper text `Open Menu > Slave Manager` MAY use dark text if it reads better over the current wallpaper
- small top-left clock hidden while large centered clock is visible
- no bottom `MENU` button when the top-left Settings icon is visible

Current empty-state visual target:

```text
Wallpaper visible full-screen
Top-left Settings icon visible
Top-right connection indicators visible
Centered large HH:MM
Centered "No Device Active"
Centered helper text "Open Menu > Slave Manager"
No empty panel/card behind the message
```

Connection warning:

```text
Slave Offline
Check fieldbus connection
```

Warning SHALL be visible but not cover the entire screen unless critical.

---

# 21. User Interaction Summary

```text
Tap AVG temperature:
    open Temperature Detail Screen

Swipe up:
    open Environment Detail Panel

Tap AC UP:
    increase AC target temperature

Tap AC DOWN:
    decrease AC target temperature

Tap Projector:
    toggle projector desired state safely

Tap LED/Lamp aggregate:
    if one lamp channel exists:
        toggle that lamp safely
    else:
        open/select lamp channel controls

Tap Lamp 1-4:
    toggle selected lamp channel safely

Tap top-left Settings icon:
    open Settings
```

Backend command safety:

```text
User-facing toggle SHALL be translated into explicit final state command.
```

Example:

```text
User taps Lamp channel
Current desired state = OFF
Send SET_LIGHT_CHANNEL(channel_id, ON)
```

Do not send ambiguous TOGGLE commands to slave if reliability retry can duplicate commands.

---

# 22. Dashboard Data Inputs

Firmware V2 dashboard input model:
- MQTT data is received per sensor/control topic.
- The app/dashboard SHALL merge per-topic updates into a local dashboard model.
- The app/dashboard SHALL NOT assume the old single master state JSON as the primary data source.
- The old single master state JSON model MAY remain as a Legacy / V1 compatibility note only.

Payload handling by reference:
- General simple sensors use integer payloads.
- LED and projector use integer payloads: `1` for ON and `0` for OFF.
- Temperature uses one integer average Celsius payload. Per-slot temperature detail remains a local dashboard/detail-screen model.
- Full MQTT topic and payload rules live in the MQTT specification; this UI doc only defines how the app/dashboard consumes them.

Example, not hardcoded values:
- class/room temperature topic updates the temperature card and Temperature Detail screen.
- class/room LED topic updates all Lamp 1-4 sync states.
- class/room AC topic updates the AC widget state after the slave confirms the actuator state.

State synchronization rule:
- User control actions SHALL update the UI as pending until the target device confirms the state.
- After confirmation, the app/dashboard SHALL refresh from the corresponding published topic.
- LED state SHALL be synchronized per LED position, not as one ambiguous aggregate state.

Firmware V2 change note:
- What changed: per-topic MQTT updates are now the primary source for the companion app/dashboard.
- Why changed: Firmware V2 separates sensor and actuator families into independent topics for simpler app subscriptions and clearer synchronization.
- Implementation effect: app code should subscribe to the configured class/room topics, normalize incoming payloads, and then render one combined DashboardModel.

DashboardModel SHOULD include normalized fields such as:

```cpp
struct DashboardModel {
    bool avg_temp_valid;
    float avg_temp;

    bool temp_valid[4];
    float temp[4];

    bool co2_valid;
    int co2_ppm;

    bool lux_valid;
    float lux;

    bool human_presence_valid;
    bool human_presence;

    bool ac_available;
    int ac_target_temp;

    bool projector_available;
    bool projector_on;

    bool led_available;
    bool led_on[4];
    uint8_t led_count;

    bool wifi_connected;
    bool lan_connected;
    bool fieldbus_ok;

    char time_text[8];
};
```

Legacy / V1 Notes:
- A single large master state JSON may exist in older firmware/app flows.
- It SHALL NOT be documented or implemented as the primary Firmware V2 app/dashboard model.
- If kept for compatibility, it should be parsed only as a fallback source and clearly labeled as legacy behavior.

---

# 23. Main Render Flow

```text
Render wallpaper/background
    |
    v
Render transparent top overlay
    |
    v
Render CO2 small chip if valid
    |
    v
Compute dashboard layout mode
    |
    v
Render temperature widget if available
    |
    v
Render AC widget if available
    |
    v
Render Projector widget if available
    |
    v
Render LED/Lamp widget if available
    |
    v
Render top-left Settings/menu touch area
```

---

# 24. Dashboard UI Block Diagram

```text
Per-Sensor MQTT Topics / Local HMI Slave Data
        |
        v
Topic Normalizer / Mapping Manager
        |
        v
DashboardModel
        |
        v
Layout Decision Engine
        |
        +----------------------------+
        |                            |
        v                            v
Widget Visibility              Widget Placement
        |                            |
        +-------------+--------------+
                      |
                      v
                Dashboard Render
```

Firmware V2 change note:
- What changed: the UI block diagram now starts from per-sensor MQTT topics for the app and local HMI slave data for the device UI.
- Why changed: Firmware V2 splits data by sensor/control type while still rendering one simple dashboard.
- Implementation effect: the render layer should not know whether data came from MQTT or local Modbus; it should consume the normalized DashboardModel.

---

# 25. UI Implementation Rules

```text
1. Dashboard SHALL be data-driven.
2. Dashboard SHALL not hardcode fixed widget presence.
3. Widget visibility SHALL follow capability/mapping availability.
4. Main UI SHALL remain readable from distance.
5. Debug information SHALL NOT appear on main dashboard.
6. Secondary details SHALL require tap/swipe.
7. Wallpaper SHALL not reduce readability; adjust text color per element before adding heavy panels.
8. Control widgets SHALL use large touch targets.
9. Text SHALL remain short and high contrast.
10. If a widget is unavailable, hide it rather than show disabled clutter.
11. Dashboard empty/status mode SHALL keep wallpaper visible and avoid decorative opaque containers.
12. Full-width opaque top bars SHALL be avoided on the dashboard.
```

---

# 26. Recommended Touch Areas

Minimum touch target:

```text
48 x 48 px
```

Important controls:
- AC UP/DOWN
- Projector ON/OFF
- Lamp 1-4 ON/OFF
- Menu
- AVG temperature card

These SHALL use larger target area where possible.

---

# 27. Future UI Notes

Future enhancements may include:
- theme color template
- wallpaper picker
- brightness control
- day/night mode
- multi-room page
- animated environmental panel
- icon pack for WiFi/LAN/fieldbus

---

# END DOCUMENT
