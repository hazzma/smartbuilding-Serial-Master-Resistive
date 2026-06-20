# Automation & Security Update Specification

This document details the system design, logical changes, and documentation impact for the new automation, feedback-loop verification, and occupancy-aware safety override features.

---

## V2.8 Implementation Reference

The next polling, MQTT timing, Lux classification, Lux outlier, occupancy
override, and local daily schedule-engine behavior is defined in
`docs/V2.8_Planning.md`.

Important current schedule status:

- `PRE_CLASS_ON` and `CLASS_ENDED` event handling is implemented.
- Daily payload parsing, overwrite, NVS persistence, catch-up, and automatic
  local slot execution are implemented.

---

## V2.7.1 Checkpoint

V2.7.1 is a documentation checkpoint for tightening the V2.7 automation logic
before the next implementation pass. It keeps the V2.7 feature direction, but
fixes the parts that were too fixed-threshold or too easy to misinterpret:

- Projector verification should use an adaptive Lux baseline instead of only a
  fixed `50 lx` delta.
- Occupancy safety rules should only trust presence when the presence sensor
  value is valid.
- Smart shutdown should recheck slowly when a room remains occupied, rather
  than effectively keeping an expired timer hot.
- Alert Bit 7 is finalized as after-hours empty-room active-load anomaly.

---

## 1. Projector State Verification using Lux Sensor (BH1750)

### Background
Currently, the Projector IR control operates as a one-way (simplex) transmission. The Master cannot verify if the projector actually powered ON or if it failed (e.g., due to bulb burnout or blocked IR transceiver). By utilizing the ambient Lux sensor (BH1750) on the slave device, the Master can verify the projector's power state through a light-level feedback loop.

### Logic & State Machine
1. **State Addition**: Projector verification should expose enough state for UI
   and MQTT/server phrasing:
   - `PROJ_STATE_OFF`
   - `PROJ_STATE_POWERING_ON`
   - `PROJ_STATE_VERIFIED_ON`
   - `PROJ_STATE_RETRYING`
   - `PROJ_STATE_NO_LUX`
   - `PROJ_STATE_CHECK_LUX`
   - `PROJ_STATE_CHECK_PROJECTOR`
2. **Ambient Baseline Learning**:
   - While the projector is OFF and Lux is valid, the master should learn the
     room ambient baseline slowly per Lux channel.
   - The baseline should update only when the room is stable enough for a useful
     reference, not while a projector verification is active.
   - Minimal tracked values:
     - `proj_lux_baseline[4]`
     - `proj_lux_baseline_channel_valid[4]`
     - optional aggregate `proj_lux_baseline_avg` for logging/status
   - The baseline is a room condition, not a projector state. It may persist in
     RAM first; NVS persistence can be added only if later testing shows boot
     recalibration is too slow.
2. **Initial Trigger**:
   - When the user (via local HMI or MQTT control) triggers Projector ON:
     1. Freeze the latest valid ambient baseline for each Lux channel.
     2. Transition the internal state to `PROJ_STATE_POWERING_ON`.
     3. Send the IR ON Modbus command to the slave.
     4. Immediately publish `1` (ON) to MQTT topic `HD01/data/projector` to maintain responsive UI status.
     5. Start an 8-second `warmup_timer`.
3. **Verification**:
   - After the 8-second `warmup_timer` expires:
     1. Read the latest Lux channel values.
     2. For every valid channel, calculate:
        `delta_lux[i] = L_current[i] - L_baseline[i]`.
     3. Calculate an adaptive threshold:
        `threshold_lux[i] = max(20 lx, min(80 lx, L_baseline[i] * 0.20))`.
     4. Optionally calculate a ratio guard:
        `ratio[i] = L_current[i] / max(L_baseline[i], 1 lx)`.
     5. A channel verifies projector ON if either:
        - `delta_lux[i] >= threshold_lux[i]`
        - `ratio[i] >= 1.25` and `delta_lux[i] >= 15 lx`
     6. If verification passes:
        - If every expected Lux channel is valid and verified, transition to
          `PROJ_STATE_VERIFIED_ON`.
        - If at least one channel verified ON but another expected channel is
          invalid or unchanged, transition to `PROJ_STATE_CHECK_LUX`.
        - Clear Projector Alert Bit 5.
     7. If verification fails:
        - If `retry_count == 0`:
          - Increment `retry_count` to `1`.
          - Re-send the IR ON command to the slave.
          - Restart the 8-second timer.
        - If `retry_count == 1`:
          - Transition to `PROJ_STATE_CHECK_PROJECTOR`.
          - Keep projector state ON because one-way IR verification cannot prove
            the projector is actually OFF.
          - Set the **Projector Alert Flag** (Bit 5 in alert bitmask) to request
            projector/IR/path inspection.
          - Keep publishing `1` (ON) to MQTT topic `HD01/data/projector`.
          - Reset `retry_count` to `0`.
4. **No Valid Lux Fallback**:
   - If no BH1750 is installed, Lux is invalid (`< 0.0 lx`), or no baseline is available, the master may
     keep the optimistic projector ON state so the command remains responsive.
   - The verification state must be `PROJ_STATE_NO_LUX`, not
     `PROJ_STATE_VERIFIED_ON`.
   - Projector Alert Bit 5 should not be raised from verification skip alone.

---

## 2. Scheduler & Occupancy-Based Shutdown (Smart Shutdown)

### Logic & Event Flow
The master does not need a local UI for schedule editing. It always listens to
`HD01/control/schedule` and accepts schedule input from the server.

1. **Daily/Weekly Bitmask Schedule Payload (Active V2.8 Flow)**:
   - Topic: `HD01/control/schedule`
   - Payload format: `S1S2S3S4S5S6` (today-only) or `S1S2S3S4S5S6;S1S2S3S4S5S6;S1S2S3S4S5S6;S1S2S3S4S5S6;S1S2S3S4S5S6;S1S2S3S4S5S6;S1S2S3S4S5S6` (separated by semicolons for Monday to Sunday) representing sessions S1 to S6.
   - **Right-to-Left parsing order (LSB on the right)** is used to support payloads sent or stored as integers where leading zeros are omitted. The rightmost character maps to session S1 (Bit 0), the second rightmost to S2 (Bit 1), etc. Leading zeros are optional.
   - Example (single day): `"10011"` (equivalent to `"010011"`, enabling sessions S2, S5, and S6 for today), `"1"` (equivalent to `"000001"`, enabling S1), or `"10"` (equivalent to `"000010"`, enabling S2).
   - Example (weekly): `"10011;111000;0;0;0;0;0"` (Monday S2, S5, S6 active; Tuesday S4, S5, S6 active; other days none).
   - A valid payload replaces/overwrites the previous stored schedule, re-caches active sessions for today, and resets slot trigger flags.
2. **Daily Schedule Payload (Legacy Flow, kept for NVS compat only)**:
   - Topic: `HD01/control/schedule`
   - Legacy format: `YYYYMMDD;HHMM-HHMM;HHMM-HHMM;...` (e.g. `20260609;0800-0930;1015-1200;1330-1500`).
   - Limits: Max 8 sessions per day, reject invalid times, `start >= end`, or malformed fields.
2. **Local Schedule Execution**:
   - The master checks local RTC/time periodically.
   - For each stored slot:
     - At `start - 20 minutes`, run the same action as `PRE_CLASS_ON`.
     - At `end`, run the same action as `CLASS_ENDED`.
   - Each slot must have trigger flags such as `pre_triggered` and
     `end_triggered` so the same event is not emitted repeatedly.
   - If the master reboots during an active class and local time is valid, it may
     perform a one-time catch-up by ensuring AC/lights are ON when
     `start <= now < end`.
3. **Pre-Class Event Fallback (Server to Master)**:
   - Topic: `HD01/control/schedule`
   - Payload: `"PRE_CLASS_ON"` (sent 20 minutes before a scheduled class).
   - Action: Master immediately sends command to turn ON the AC and Lights.
4. **Class Ended Event Fallback (Server to Master)**:
   - Topic: `HD01/control/schedule`
   - Payload: `"CLASS_ENDED"` (sent at the exact end of class).
   - Action: Master starts a local 20-minute countdown timer (`shutdown_timer`).
5. **Shutdown Verification**:
   - When the 20-minute `shutdown_timer` expires, the Master checks the human presence state:
     - **If `human_presence_valid == true` and `human_presence == false`** (Class is confirmed empty):
       - Master sends commands to turn OFF the AC and Lights.
     - **If `human_presence_valid == true` and `human_presence == true`** (Students or teacher still present):
       - Master delays shutdown and schedules a recheck every 5 minutes.
       - Once human presence becomes `false` during a recheck, the Master turns OFF the AC and Lights.
     - **If `human_presence_valid == false`**:
       - Master must not assume the room is empty.
       - Master should delay shutdown and raise/keep the presence sensor alert
         through Alert Bit 3.

---

## 3. Acceptation Level (Local Occupancy Override)

### Background
To prevent remote scheduling scripts or server-side operators from disrupting active classes (e.g., turning off lights or AC while students are studying or taking exams), the Master enforces a local occupancy safety check.

### Override Rules
When `human_presence_valid == true` and `g_state.sensor.human_presence == true`
(Classroom is confirmed occupied):
1. **MQTT LED Commands Restricted**:
   - Ignore any incoming commands on `HD01/control/led` that attempt to toggle LED power (both ON and OFF commands from the server are discarded).
2. **MQTT AC Commands Restricted**:
   - Ignore any command on `HD01/control/ac` that attempts to turn the AC OFF.
   - Accept commands that adjust target temperature, modes, fan speed, or swing.
3. **Local HMI Control Priority**:
   - Local touchscreen controls on the Master HMI remain fully enabled. Physical users inside the classroom can override the safety block at any time.

When `human_presence_valid == true` and `g_state.sensor.human_presence == false`
(Classroom is confirmed empty):
- All server-side MQTT control commands (ON, OFF, parameter changes) are accepted and executed.

When `human_presence_valid == false`:
- Safety logic should fail conservative for LED/light OFF commands.
- AC OFF commands may still be accepted when presence is unknown so rooms
  without a presence sensor can still be controlled remotely.
- The master should report Alert Bit 3 so the server/Flutter can show that
  occupancy state is unknown.

---

## 4. 7-Day Rolling Average Active-Load Anomaly Alerting

### Background
Detects electrical anomalies (e.g., lights or AC left ON overnight in an empty
room) by monitoring daily active-load durations, without exhausting ESP32 flash
memory or requiring cloud calculations.

### Technical Implementation
1. **Daily Tracking**:
   - The Master tracks the cumulative active-load duration in RAM.
   - Recommended V2.7.1 definition:
     `active_load = light_on || ac_on`.
   - `active_load_minutes` is more useful than lamp-only minutes because the
     energy anomaly is "empty class still consuming power".
2. **Midnight Rolling Log**:
   - The Master synchronizes its clock via NTP.
   - At exactly 00:00 (midnight) or on boot if a date boundary is crossed:
     1. Write `current_day_duration` to the circular history buffer:
        `light_history[day_count % 7] = current_day_duration;`
     2. Increment `day_count`.
     3. Reset `current_day_duration` to `0`.
     4. Persist `light_history` (7 elements of `uint16_t`) and `day_count` (uint32_t) to ESP32 Preferences under the namespace `"light_anom"`. This ensures day 1 is replaced by day 8, maintaining a sliding 7-day window.
3. **Anomaly Logic**:
   - Prerequisites:
     - Local time is valid.
     - At least 5 valid historical days exist; 7 days is preferred.
     - Historical average is meaningful, recommended `avg_7d >= 30 minutes`.
     - Presence is valid and the room is confirmed empty.
   - Recommended threshold:
     `active_load_minutes > max(avg_7d * 1.5, avg_7d + 60)`.
   - The alert is evaluated only during after-hours, recommended `22:00-06:00`.
   - If all conditions pass:
     - Raise **After-Hours Empty-Room Active-Load Anomaly** by setting **Bit 7**
       (value `128`) on the `HD01/data/alert` bitmask.
   - If presence is invalid, do not raise Bit 7. Raise/keep Alert Bit 3 instead.

---

## 5. Documentation Impact Analysis

The following files require updates to integrate these new features:

### 5.1 [FSD_Smart_Building_Master_UPDATED.md](file:///c:/Users/hanse/Documents/PlatformIO/Projects/S3%20Master%20Serial/docs/FSD_Smart_Building_Master_UPDATED.md)
* **Section 1.1 (Firmware V2 Change Summary)**:
  - Add details about the Projector verification loop, Acceptation Level rules, schedule processing, and rolling anomaly checks.
* **Section 5.3 (Screen State Machine)**:
  - Update the dashboard state transitions to include the `Powering On` state for the projector.
* **Section 6 (Shared State Requirements)**:
  - Add variables for projector verification states (`warmup_timer`, `retry_count`, `lux_initial`), scheduler countdowns, and rolling lamp history arrays.
* **Section 7 (Functional Requirements)**:
  - Add new requirements:
    - `CTRL-010` (Projector verification algorithm).
    - `CTRL-011` (Acceptation Level override logic).
    - `NET-011` (Schedule packet processor).
    - `ALRT-002` (7-Day Lamp anomaly alerting logic).

### 5.2 [UIUX.md](file:///c:/Users/hanse/Documents/PlatformIO/Projects/S3%20Master%20Serial/docs/UIUX.md)
* **Section 11 (Projector Widget Behavior)**:
  - Document the `"Powering On"` visual state: the widget should display a pulsing/progress indicator or auxiliary badge color when `PROJ_STATE_POWERING_ON` is active.
  - Document fallback behavior: if the verification fails, the widget returns to the `"OFF"` visual state with an alert icon.

### 5.3 [MQTT_V2.5_Changelog.md](file:///c:/Users/hanse/Documents/PlatformIO/Projects/S3%20Master%20Serial/docs/MQTT_V2.5_Changelog.md)
* **Section Runtime Topic Format**:
  - Register `HD01/control/schedule` commands: `"PRE_CLASS_ON"`, `"CLASS_ENDED"`.
* **Section Payload Format (Alert Decimal Bitmask Table)**:
  - Add Bit 7 (Value `128`) for "After-Hours Empty-Room Active-Load Anomaly".
