# River Waste Collection Boat

A small, low-cost, phone-controlled catamaran that finds floating waste, lifts it with a conveyor belt into an on-board bin, and shows when the bin is full.

Final-year project, Dept. of IoT & Cybersecurity incl. Blockchain Technology, SMVIT, Bengaluru (VTU), 2026–27.

| | |
|---|---|
| **Team** | Abhinav Sharma (1MV23IC001), Aditya Raj Singh (1MV23IC004), Anurag Patel (1MV23IC009), Shaswat Sandilya (1MV23IC050) |
| **Guide** | Ms. Pooja Bujurke |

## How it works

1. **Drive:** the phone joins the boat's own Wi-Fi and steers with on-screen buttons.
2. **Detect:** an HC-SR04 ultrasonic sensor at the conveyor mouth spots waste under 20 cm (3 readings in a row).
3. **Lift:** the boat stops and the conveyor belt carries the waste into the bin.
4. **Alert:** an IR sensor on the bin wall detects a full bin, stops the conveyor and shows **FULL** on the phone.

## Hardware

| Part | Qty | Role |
|---|---|---|
| ESP32 (38-pin) | 1 | Controller, Wi-Fi access point, web server |
| L298N motor driver | 1 | Drives the 4 propulsion motors |
| BO motor, 100 RPM | 6 | 4 drive (paddle wheels) + 2 conveyor |
| BO wheel | 4 | Paddle propulsion |
| HC-SR04 | 1 | Waste detection |
| IR obstacle sensor | 1 | Bin full / empty |
| 5 V relay module, 1-channel, active LOW | 1 | Conveyor on/off |
| LM2596 buck converter | 1 | 5.0 V rail |
| 18650 cell, 2600 mAh + 2-cell holder | 2 + 1 | 2S pack, 7.4–8.4 V |
| 5 A fuse + holder, rocker switch | 1 each | Protection and main switch |
| 1N5819 diode | 1 | Flyback protection across conveyor motors |
| 1 kΩ resistors | 3 | HC-SR04 ECHO voltage divider |
| TP4056 module | 1 | Charging cells outside the boat |

Hull: catamaran, two floats about 30 × 6.5 × 7 cm, 30 × 20 cm deck, about 1.1 kg target. Bin about 10 × 8 × 6 cm. Conveyor belt about 6 cm wide at a 25–30° incline.

## Pin map

| ESP32 pin | Function |
|---|---|
| GPIO 25 | L298N ENA (left speed, PWM) |
| GPIO 26 / 27 | L298N IN1 / IN2 |
| GPIO 13 | L298N ENB (right speed, PWM) |
| GPIO 32 / 33 | L298N IN3 / IN4 |
| GPIO 18 | Relay IN (conveyor, active LOW) |
| GPIO 5 | HC-SR04 TRIG |
| GPIO 34 | HC-SR04 ECHO (through divider) |
| GPIO 35 | Bin IR sensor OUT |
| GPIO 36 | Optional battery divider (off by default) |
| VIN | 5.0 V from LM2596 |
| 3V3 | IR sensor VCC |

## Power

```
2 x 18650 (7.4-8.4 V) -> 5 A fuse -> rocker switch -> BAT+ rail
BAT+ rail -> L298N +12V terminal, relay COM, LM2596 IN+
LM2596 OUT+ (set to 5.0 V first) -> ESP32 VIN, relay VCC, HC-SR04 VCC
All grounds (battery -, L298N, LM2596, ESP32, relay, sensors) join at one common ground
```

## Wiring summary

- **Propulsion:** left-front + left-rear motors in parallel on OUT1/OUT2; right-front + right-rear in parallel on OUT3/OUT4. Remove the ENA/ENB jumper caps so PWM works.
- **Conveyor:** relay COM to BAT+, NO to two motors in series, back to battery minus. A 1N5819 diode sits across the two motors (stripe toward +).
- **HC-SR04 ECHO:** `ECHO -[1k]- GPIO34`, with `[1k]+[1k]` to GND from the same point, giving 3.3 V.

See [`wiring_guide.md`](wiring_guide.md) for the full step-by-step guide, test order and troubleshooting.

## Firmware

Arduino C++ for the ESP32 (board: **ESP32 Dev Module**, ESP32 core by Espressif).

1. Open `boat_firmware.ino` in Arduino IDE and upload.
2. On your phone, join Wi-Fi **RiverBoat** (password `boat12345`).
3. Open **http://192.168.4.1**.

Page features: hold-to-drive buttons, speed slider, conveyor switch, auto-collect switch, live bin / distance / battery status.

Key settings (top of the sketch): `WASTE_TRIGGER_CM` 20, `WASTE_HITS_NEEDED` 3, `CONVEYOR_RUN_MS` 10000, `AUTO_COOLDOWN_MS` 3000, `MOVE_TIMEOUT_MS` 1000, `LINK_TIMEOUT_MS` 4000, `BIN_CONFIRM_MS` 1500.

Use `boat_test.ino` first to check motors, relay and sensors one by one over the Serial Monitor (115200 baud).

### Safety features

- Motors stop if no drive command arrives for 1 s.
- Everything stops if the phone is silent for 4 s.
- A full bin stops the conveyor and blocks forward driving.
- The relay pin is released at boot so the conveyor never starts by accident.
- 5 A fuse and a main switch; optional low-battery cutoff.

## Simulation

Open `river_boat_simulation.html` in any browser (no install, works offline). It runs the same detection and auto-collect logic and shows the ESP32 pin states live. It is a schematic demo, not a physics-accurate model. If you host it with GitHub Pages, rename it to `index.html`.

## Repository contents

```
boat_firmware.ino            Main ESP32 firmware
boat_test.ino                Step-by-step hardware test sketch
wiring_guide.md              Wiring, test order, troubleshooting
river_boat_simulation.html   Browser simulation
docs/                        Report and presentation (add your files)
```

## Estimated cost

About ₹1,800–2,900 (electronics ₹1,300–1,700, cells ₹0–312, mechanical ₹480–900). Planning estimate, not final invoices.

## Limitations

- Small bin and light waste only.
- Conveyor is on/off only (no speed control).
- No BMS: charge the cells one at a time outside the boat.
- Wi-Fi range is limited to the phone's reach.
- Ultrasonic readings can be disturbed by waves.

## Future scope

Camera and AI to recognise plastic, GPS navigation and autonomous routes, solar charging, conveyor speed control with a second driver, and a blockchain log of collected waste.

## License

Add a license of your choice (for example MIT) before publishing.
