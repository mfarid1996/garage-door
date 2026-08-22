# ESP32 Garage Door — Setup Notes

## How it works

**Flow:** Browser → Netlify Function (validates token, holds MQTT creds) → HiveMQ Cloud broker → ESP32

**Web app:** https://garage-door-mfarid.netlify.app — tap the button on an enrolled device to trigger.

**To enroll a new device:** generate a token, add it to the `VALID_TOKENS` Netlify env var (comma-separated), redeploy, then open `https://garage-door-mfarid.netlify.app/?t=<token>` on the new device once to save it silently.

---

## Guest access

There is one shared, permanent guest token in `VALID_TOKENS`, prefixed `guest-` so it is
identifiable in the list. Hand the same link to every guest:

```
https://garage-door-mfarid.netlify.app/?t=guest-<...>
```

Guests do **not** install anything — the link opens in whatever browser they already have and
shows the same button you get. The token is saved to `localStorage` and stripped from the address
bar on first load, so reopening the link later still works.

A guest token is functionally identical to a personal one: no expiry, no usage cap, no per-guest
identity, and no audit trail. It is a permanent key. Anyone who keeps or forwards the link can open
the door indefinitely.

**To revoke all guest access at once** (the only revocation granularity — every guest shares this
token, so rotating it cuts off all of them):

```powershell
# read current value, drop the guest- entry, append a fresh one, redeploy
netlify env:list --json      # find the guest- entry
netlify env:set VALID_TOKENS "<other,tokens>,guest-<new-random>" --force
netlify deploy --prod
```

Generate the random part in PowerShell (note: the static `RandomNumberGenerator.GetBytes` does not
exist in Windows PowerShell 5.1 — use `RNGCryptoServiceProvider`):

```powershell
$rng = New-Object System.Security.Cryptography.RNGCryptoServiceProvider
$bytes = New-Object byte[] 18; $rng.GetBytes($bytes)
"guest-" + [Convert]::ToBase64String($bytes).Replace('+','-').Replace('/','_').TrimEnd('=')
```

---

## Hardware

- **Board:** Arduino Nano ESP32 (ABX00083) — u-blox NORA-W106-10B module, ESP32-S3 dual-core
  240 MHz, 512 KB SRAM + 8 MB PSRAM, 16 MB flash, certified internal antenna
- **USB:** native USB-C on the ESP32-S3 itself — **no CH340**. See the port gotcha below.
- **Built-in LED:** `LED_BUILTIN` = D13 = GPIO48, active HIGH (HIGH = lit).
  The separate RGB LED (`LED_RED`/`LED_GREEN`/`LED_BLUE` = GPIO46/0/45) is **active LOW**
  and sits on strapping pins — the firmware does not use it.
- **Power:** USB-C only. Do not use VIN.
- **Servo:** SG90 (180° positional, not continuous rotation)
  - Brown/Black → `GND` (the one below D2) · Red → **`VBUS`** (below A7) · Orange/Yellow → **`D6`** (GPIO9)
  - On trigger: rotates from 50° (rest) to 15°, holds 150 ms, returns to 50°, then **detaches** —
    see the `SERVO_*` constants and `servoActuate()` in `garageButton.ino`
  - **Driven with raw LEDC, not ESP32Servo** — see the warning below

> ### ⚠️ ESP32Servo does not work on this board, and fails silently
> Two faults compound on the Nano ESP32:
> 1. The `nano_nora` variant's boot animation (`variant.cpp` `rgb_pulse_delay`) drives the RGB LED
>    with `analogWrite()`, which configures LEDC timers at **1000 Hz** and never releases them.
>    ESP32Servo keeps its own private channel bookkeeping, cannot see those, and hands the servo a
>    channel whose timer is already at 1000 Hz. A servo needs a 20 ms frame; at 1 ms it sees nothing
>    it recognises, so it does not move and does not even buzz.
> 2. ESP32Servo ignores the return value of `ledcAttachChannel()`, and `Servo::attached()` only
>    reports its own internal flag. So `attach()` "succeeds", `write()` does nothing, and the
>    firmware acks a press that never happened.
>
> Measured on hardware: via ESP32Servo `ledcReadFreq(D6)` was **1000**; via raw LEDC it is **50**.
> The firmware now frees the RGB LED's LEDC channels at boot, then calls
> `ledcAttach(D6, 50, 14)` and **checks the return value**.
>
> The 14-bit width is pinned deliberately: **16-bit at 50 Hz is rejected outright** by the ESP32-S3
> (`ledcAttach` returns false). Verified by probing 16/14/12/10 on hardware.

> ### ⚠️ `VBUS` is not `VIN`
> On the old dev board VIN was USB 5 V passthrough. On the Nano ESP32, **VIN is an input**
> (6–21 V into the buck) and sources nothing. `VBUS` is the only 5 V available, and it is live
> **only while the board is powered over USB-C** — power the board from VIN and the servo dies.
> Do not run the servo from `3V3`: the SG90 wants 4.8–6 V and that rail also feeds the S3.

> ### ⚠️ Arduino pin numbers ≠ GPIO numbers
> The `nano_nora` variant builds with `BOARD_HAS_PIN_REMAP`, so a bare integer in the Arduino
> API is an **Arduino** pin number. `13` means D13/GPIO48, not GPIO13; `2` means D2/GPIO5.
> The old `#define SERVO_PIN 13` would not error — it would silently drive the wrong pin.
> Always use the `Dn`/`An` constants. The remap is applied once inside the core's
> `pinMode` / `digitalWrite` / `ledcAttach` macros.
>
> Avoid for the servo signal: `D0`/`D1` (UART0 — D1 spews the boot log at every reset),
> `A2` (GPIO3, JTAG strapping), and the RGB LED pins (GPIO0/45/46, strapping).
> `D2`–`D10` are plain GPIOs and safe.

---

## Software

- **Arduino CLI:** v1.4.1, installed via `winget install ArduinoSA.CLI`
- **ESP32 Arduino core:** `esp32:esp32`, version 3.3.8
- **Board FQBN:** `esp32:esp32:nano_nora`
- **Libraries:** PubSubClient 2.8.0 (ESP32Servo is NOT used — see the servo warning above)
- **Node.js:** v24.15.0 (installed via winget)
- **Netlify CLI:** v26.0.1 (installed via npm)

### Compile & upload

First, copy `esp32/garageButton/secrets.h.example` to `esp32/garageButton/secrets.h` and fill in your WiFi and MQTT credentials.

```powershell
arduino-cli board list    # find the port — see the warning below
arduino-cli compile --fqbn esp32:esp32:nano_nora "<path-to-repo>\esp32\garageButton"
arduino-cli upload --fqbn esp32:esp32:nano_nora --port COM5 "<path-to-repo>\esp32\garageButton"
```

**The COM port moves — and now it also disappears.** Windows still assigns it per physical USB
socket, but the Nano ESP32 enumerates its serial port from the ESP32-S3 itself rather than from a
CH340 bridge, so the port **vanishes and re-enumerates across every reset and upload**. Two
consequences:

- `arduino-cli board list` now identifies the board on its VID/PID (`0x2341`/`0x0070`,
  shown as `Arduino Nano ESP32`) — there is no `USB-SERIAL CH340` row any more.
- **The default DFU upload path does not work on this machine.** `arduino-cli upload` without a
  programmer fails with `Cannot open DFU device 2341:0070 (LIBUSB_ERROR_NOT_FOUND)` — the DFU
  interface has no WinUSB driver bound. Use the **`-P esptool`** programmer instead, as shown above.
- **Upload is a two-port dance.** `-P esptool` does a 1200 bps touch that reboots the board into the
  ROM bootloader, which enumerates as a *different* COM port. The first attempt therefore fails with
  `OSError(22) ... A device which does not exist was specified` — that is expected, not a fault.
  Re-run `arduino-cli board list`, find the `ESP32 Family Device` row (COM4 here), and upload to
  **that** port. After flashing it hard-resets back to the sketch port (COM5).
- If the board is not found at all, force DFU by **double-tapping the white RST button**.

`compile` needs no board attached — only `upload` does.

### Netlify redeploy (after any change to web app or env vars)

```powershell
$env:PATH = [System.Environment]::GetEnvironmentVariable("PATH","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("PATH","User")
netlify deploy --prod
```

---

## WiFi

- **Band:** 2.4GHz, channel 11 — ESP32 supports 2.4GHz only
- **Note:** SSID is case-sensitive
- WiFi credentials go in `secrets.h` (gitignored)

---

## MQTT (HiveMQ Cloud)

- **Broker:** HiveMQ Cloud (free tier)
- **Port:** 8883 (MQTT over TLS), 8884 (WSS, used by the browser status feed)
- **Topics:**
  | Topic | Retained | Published by | Purpose |
  |---|---|---|---|
  | `garage/trigger` | no | Netlify Function | `1` = press the button |
  | `garage/ack` | no | ESP32 | trigger received / cooldown-ignored |
  | `garage/status` | **yes** | ESP32 (+ LWT) | `online` / `offline` |
  | `garage/session` | **yes** | ESP32 | `{session, boots, downMs, reason, rst}` on every MQTT connect |
- Credentials go in `secrets.h` (ESP32) and Netlify env vars (`MQTT_HOST`, `MQTT_USER`, `MQTT_PASS`)
- If you rotate the MQTT password, update both Netlify env vars and re-flash the ESP32

### Session / boot counters (NVS)

The firmware keeps two monotonic counters in NVS namespace `garage`: `boots` (bumped once per
`setup()`) and `session` (bumped on every successful MQTT connect). They are written **only** on
those two events — never periodically — because a heartbeat write would burn the flash sector for
no extra information. A full flash erase resets them to 0; the backend resyncs after one poll and
loses at most one recorded reconnect.

Note that **flashing the board is itself a disconnect**, so every upload registers as one
connectivity-loss event in the statistics.

---

## Netlify env vars

Set these in the Netlify dashboard for the `garage-door-mfarid` project:

| Variable       | Value                          |
|----------------|-------------------------------|
| `MQTT_HOST`    | your HiveMQ broker hostname   |
| `MQTT_USER`    | your MQTT username            |
| `MQTT_PASS`    | your MQTT password            |
| `VALID_TOKENS` | comma-separated device tokens |

---

## Gotchas

1. **SSID case sensitivity** — WiFi.begin() silently fails with status 6 (WL_DISCONNECTED) if the case is wrong.
2. **Never open the serial port with DTR asserted** — this is the big one on the Nano ESP32.
   The port is the S3's own native USB-Serial/JTAG peripheral, so asserting DTR on open drops the
   board into the **ROM bootloader**, where it sits silently forever: no sketch, no WiFi, no MQTT,
   and the broker fires the LWT so the dashboard just says `offline`. It looks exactly like a
   firmware hang. Diagnose it with `arduino-cli board list`:

   | Reported board name  | Port | Meaning                     |
   |----------------------|------|-----------------------------|
   | `Arduino Nano ESP32` | COM5 | sketch is running — healthy |
   | `ESP32 Family Device`| COM4 | stuck in the ROM bootloader |

   Set `DtrEnable = $false` **and** `RtsEnable = $false` before `.Open()`. To get a board back out
   of the bootloader without unplugging it:

   ```powershell
   & "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\esptool_py\5.2.0\esptool.exe" --port COM4 --chip esp32s3 run
   ```

   Note the **port number differs between the two modes** (COM5 running vs COM4 bootloader), because
   they enumerate as different USB devices. The port also vanishes and re-enumerates on every reset,
   so a serial reader must reopen rather than assume a stable handle.
3. **Serial output timing** — setup() runs immediately at boot. `delay(3000)` at the start of setup() gives time to open the serial monitor.
4. **PATH refresh in PowerShell** — After installing tools via winget/npm, refresh PATH:
   ```powershell
   $env:PATH = [System.Environment]::GetEnvironmentVariable("PATH","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("PATH","User")
   ```
5. **PowerShell `&&` not supported** — Use `; if ($?) { ... }` to chain commands conditionally.
6. **SG90 is positional (0–180°), not continuous rotation** — cannot do a full 360°. Adjust mechanically.
7. **Netlify CLI needs PowerShell PATH** — netlify is installed via npm into the Windows PATH, not the bash PATH. Run netlify commands via PowerShell.
