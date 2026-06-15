# Smart Building Master Documentation Index

Use these documents as the current source of truth for Smart Building Firmware V2:

- `FSD_Smart_Building_Master_UPDATED.md` - system requirements, Firmware V2 behavior, and implementation impact notes.
- `UIUX.md` - dashboard, empty state, widget, Slave Manager, and touch behavior.
- `Calibration_TC.md` - XPT2046 touch alignment measurements, deviation history, active correction anchors, and retest procedure.
- `Smart_Building_RS485_Modbus_Architecture.md` - active RS485 Modbus architecture.
- `Smart_Building_Connectivity_Dashboard_Mapping_Design_UPDATED.md` - Slave Manager, feature assignment, and dashboard mapping behavior.
- `Flutter_App_MQTT_Requirements.md` - Flutter app MQTT requirements. Firmware V2 now uses per-sensor publish topics and actuator command topics; this file must stay aligned with the FSD.
- `MQTT_V2.5_Changelog.md` - exact V2.5 runtime topic format, EMQX defaults, and compatibility notes.
- `From_SLave/RS485_Modbus_Slave_Firmware_Contract v2.md` - current agreed slave wire contract v2.1.0.

Important Firmware V2 rules:

- Startup flow is `START -> Check Saved Slave -> Try reconnect if saved slave exists -> Else do nothing`.
- Discovery/pairing is for new or recovered slaves, not a mandatory assignment step on every boot.
- MQTT publishes each data type using the exact V2.5 runtime template
  `<class_name>/data/<data_type>`, for example `HD01/data/temp` and `HD01/data/co2`.
- Simple sensor payloads use integers except temperature. Temperature uses one float average Celsius value with one decimal place and is skipped while invalid so retained last-known data is preserved. LED and projector use integer `1` or `0`. AC uses `PPTTFFSS`. Alert uses a decimal integer bitmask.
- MQTT subscribes to actuator control topics under `<class_name>/control/...`. After the target slave confirms the new state, the master republishes the related data topic for app/server synchronization.
- MQTT QoS/retain defaults: scalar data QoS 0 retain true; active/LWT QoS 1 retain true; actuator commands QoS 1 retain false.
- Slave Detail selection follows the master-owned Device Profile model.
- Device profiles include `TEMP_NODE`, `PRESENCE_NODE`, `CO2_NODE`, `RELAY_NODE`, and `IR_COMBO_NODE`; slave firmware remains policy-blind.
- Slave capability/config follows the agreed contract: capability registers live at `0x0010..0x0017`, address assignment uses `0x0000`, recovery uses `0x00F4..0x00F7`, and AC control uses `0x0200..0x020D`.

Documentation change intent:

- What changed: V2 replaces single-state MQTT and legacy bundled slave assumptions with per-topic MQTT and master-owned Device Profile selection.
- Why it changed: this keeps the firmware, dashboard, and Flutter app easier to reason about.
- Implementation effect: firmware agents should update behavior around saved-slave reconnect, MQTT publish/subscribe routing, actuator state confirmation, and Device Profile based slave configuration before changing UI or source code.

Legacy/reference documents:

- `RS485_agent_documentation_report.md` is an implementation report; use the architecture and FSD files above for normative behavior.
- Any old examples that show one slave providing multiple main sensor types at once are Legacy / V1 Notes unless explicitly updated for Firmware V2.
- Any old examples that show one combined MQTT state JSON as the primary app contract are Legacy / V1 Notes unless explicitly updated for Firmware V2.
