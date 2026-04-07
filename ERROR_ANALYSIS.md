# Error Analysis: ESP32 Power-On Resets

## Problem Statement

The ESP32 experiences unexpected POWERON resets (`rtc_reason=1`) approximately every 10 cycles (~10 hours). These resets occur when the ESP32 is actively drawing current (WiFi + HTTP image download) and appear to coincide with the RP2040 triggering an e-paper panel refresh on the shared 3.3V power rail.

The 7.3" Waveshare e-paper display refresh draws significant current through its boost converter, causing the shared rail voltage to collapse below the ESP32's minimum operating voltage (~2.3V), resulting in a full power-on reset.

## Environment

- **ESP32 board:** Lolin NodeMCU-32S (ESP32-D0WD-V3, rev 3.1)
- **Co-processor:** Raspberry Pi Pico (RP2040)
- **Display:** 7.3" Waveshare 7-color e-paper
- **Power:** Shared 3.3V rail between ESP32, RP2040, and e-paper panel
- **Pico cycle:** Requests new image every 60 minutes
- **ESP32 cycle:** Deep sleep 58 min → wake → wait for SENDIMG → WiFi + HTTP stream → sleep

---

## Observations

### Log Evidence

| Date       | Log File                          | Key Findings |
|------------|-----------------------------------|--------------|
| 2026-04-05 | esp32-debug-20260405-213926.log   | 64 boots. Frequent POWERON resets during long idle wait with WiFi active. `rtc_reason` not yet logged. |
| 2026-04-06 | esp32-debug-20260406-113728.log   | 19 boots. WiFi deferred but `maintain_wifi_connection()` was reconnecting after 30s, defeating the fix. |
| 2026-04-06 | esp32-debug-20260406-173201.log   | 14 boots. Deferred WiFi + guarded maintenance working. POWERON resets still occur (`rtc_reason=1`), confirming true power-on not brownout. 2-minute sleep too short — ESP32 awake during display refresh. |
| 2026-04-07 | esp32-debug-20260407-165637.log   | 35 boots. 58-min sleep working well. Long clean streaks (10+ deep sleep cycles). POWERON resets occur ~every 10 cycles when ESP32 active transfer overlaps with e-paper refresh. |

### Reset Pattern

- Resets are always `reset=POWERON (1) | rtc_reason=1` — true voltage collapse, not brownout (`rtc_reason=15`) or watchdog.
- Resets never happen during deep sleep — only when ESP32 is awake and active (WiFi + HTTP).
- After a POWERON reset, the next boot typically catches the SENDIMG quickly and completes successfully.
- The system is physically stationary; loose cables are ruled out.

### Timing Analysis

- ESP32 active draw during WiFi + HTTP: ~150–300mA
- E-paper 7.3" refresh boost converter: high transient current (several hundred mA)
- ~~Combined peak on shared 3.3V rail likely exceeds supply capacity → voltage collapse.~~
- **DISPROVEN:** Separate power sources did not eliminate resets (see Test 1 results).
- Resets still occur with only UART (GPIO16/17) + GND connecting the two boards.

### Revised Hypotheses

1. **Ground loop / noise through shared GND** — during e-paper refresh, current through the shared ground shifts the ESP32's ground reference, causing an effective voltage dip or transient on the EN pin.
2. **Pico reboots during e-paper refresh** — if the Pico power-cycles, its UART pins go to undefined states, possibly sending transients into the ESP32.
3. **ESP32 board-level issue** — the NodeMCU-32S auto-reset circuit (USB-serial DTR/RTS → EN pin) may be floating and triggering spurious resets when no USB host is connected.
4. **Deep sleep wake misreported** — some ESP32 revisions occasionally report deep sleep wake as POWERON.

---

## Measures Taken (Software)

| # | Measure | Status | Result |
|---|---------|--------|--------|
| 1 | Deferred WiFi — radio off until SENDIMG received | ✅ Implemented | Eliminated idle WiFi power draw. Resets no longer happen during idle wait. |
| 2 | WiFi disconnect on stream failure | ✅ Implemented | Prevents radio staying active after failed transfer. |
| 3 | Guarded `maintain_wifi_connection()` with `wifiActivated` flag | ✅ Implemented | Prevents background reconnect from re-enabling radio during idle. |
| 4 | Extended deep sleep from 2 min to 58 min | ✅ Implemented | ESP32 sleeps through most display refreshes. Reduced reset frequency from every cycle to ~every 10 cycles. |
| 5 | RTC-level reset reason logging (`rtc_get_reset_reason()`) | ✅ Implemented | Confirmed resets are true POWERON (`rtc_reason=1`), not brownout or watchdog. |
| 6 | Brownout detector left enabled (default) | ✅ In place | Avoids masking power issues; no brownout events logged (rail drops too fast for detector). |

---

## Candidate Fixes (Not Yet Tested)

### Hardware

| # | Fix | Complexity | Expected Impact | Notes |
|---|-----|-----------|-----------------|-------|
| H1 | **Separate power sources** (ESP32 and Pico on independent supplies) | Low | ~~High~~ | ❌ TESTED — resets persisted. Shared rail is not the root cause. |
| H2 | **470µF electrolytic + 100nF ceramic capacitor** on ESP32 3.3V rail | Low | Medium-High — smooths transient voltage dips during e-paper refresh | Place as close to ESP32 VCC/GND as possible. If insufficient, try 1000µF on 5V input. |
| H3 | **Separate 3.3V regulator** for ESP32 | Medium | High — fully isolates ESP32 power | More robust than caps if supply can't sustain combined load. |
| H4 | **Beefier shared power supply** (higher current capacity) | Medium | High | May not be practical depending on enclosure/USB constraints. |

### Software

| # | Fix | Complexity | Expected Impact | Notes |
|---|-----|-----------|-----------------|-------|
| S1 | **Delay before deep sleep** — give Pico time to start refresh before ESP32 is active on next wake | Low | Low-Medium | Only helps if timing is predictable; Pico clock drift may reduce effectiveness. |
| S2 | **Pico waits for DONE signal** before starting e-paper refresh | Medium | High | Requires firmware change on both devices. ESP32 sends "DONE" after entering low-power state; Pico defers refresh until received. |
| S3 | **Retry-tolerant design** — accept occasional resets, ensure recovery is seamless | Low | N/A (workaround) | Already partially in place: after POWERON reset, next boot catches SENDIMG and completes. Display still updates, just takes 2 attempts. |

---

## Test Plan

### Test 1: Separate Power Sources — ❌ FAILED

**Hypothesis:** If the ESP32 and Pico/e-paper are on separate power supplies, POWERON resets will stop entirely, confirming shared rail contention as root cause.

**Setup:**
- ESP32 powered via its own USB cable
- Pico + e-paper powered via separate USB cable
- UART (Serial1) still connected between the two boards
- GND connected between both boards (required for UART signal reference)
- Multiple USB power supplies tested

**Result:** POWERON resets (`rtc_reason=1`) continued at the same frequency. Boots 35–37 all showed consecutive POWERON resets. Boot 39–40 also showed POWERON resets after a successful deep sleep cycle.

**Conclusion:** Shared power rail is **not** the root cause. The reset trigger comes through the UART/GND connection or is internal to the ESP32 board.

---

### Test 2: ESP32 Isolation (no Pico connection) ⬅️ NEXT

**Hypothesis:** If the ESP32 runs completely alone (no UART, no GND to Pico — nothing), POWERON resets will stop, confirming the reset trigger comes through the inter-board connection.

**Setup:**
- ESP32 powered via USB power supply (not a computer)
- **All wires to Pico disconnected** — no UART, no GND, no shared pins
- ESP32 will sit idle (no SENDIMG will arrive) — just monitoring for resets

**Success criteria:**
- Zero POWERON resets over 1+ hours
- Only the initial boot should show `reset=POWERON (1) | rtc_reason=1`
- No additional boots should appear in the log

**Duration:** Run for at least 1 hour.

**If resets STOP:** Root cause is the inter-board connection (UART pins or shared GND). Next test would isolate GND vs UART.

**If resets CONTINUE:** Root cause is on the ESP32 board itself (floating EN pin, USB-serial auto-reset circuit, or ESP32 silicon issue).

**Log command:**
```sh
python3 export_esp32_logs.py
```

---

### Test 3: Capacitor on Shared Rail (deferred — may not be relevant)

**Hypothesis:** A 470µF electrolytic capacitor on the ESP32 3.3V rail absorbs transient current spikes from the e-paper refresh, preventing voltage collapse.

**Setup:**
- Return to single shared power source
- Solder 470µF electrolytic + 100nF ceramic across ESP32 3V3 and GND pins
- Observe polarity on electrolytic (negative to GND)

**Success criteria:**
- Zero POWERON resets over 5+ consecutive cycles on shared rail
- If still failing, escalate to 1000µF on 5V input or separate regulator (H3)

---

## Decision Log

| Date       | Decision | Rationale |
|------------|----------|-----------|
| 2026-04-06 | Defer WiFi until SENDIMG | Reduce idle power draw; eliminate unnecessary WiFi TX spikes |
| 2026-04-06 | Add `wifiActivated` guard | `maintain_wifi_connection()` was defeating deferred WiFi |
| 2026-04-06 | Extend deep sleep to 58 min | Match Pico's 60-min cycle; keep ESP32 asleep during most refreshes |
| 2026-04-06 | Add RTC reset reason to logs | Distinguish brownout from true power-on resets |
| 2026-04-07 | Test separate power sources first | Cheapest way to confirm root cause before committing to hardware mods |
| 2026-04-07 | Test 1 result: resets persist on separate supplies | Shared power rail disproven as root cause. Multiple USB supplies tested. |
| 2026-04-07 | Test ESP32 in complete isolation next | Determine if reset trigger is internal to ESP32 or comes through UART/GND connection |
