# picobusarrival

A Raspberry Pi Pico W project that displays real-time London bus, DLR, Overground, and Tube arrival times on a 16×2 I2C LCD, with a lightweight web interface for configuration.

## Features

- Fetches live arrival data from the [TfL API](https://api.tfl.gov.uk)
- Works with bus stops, DLR, Overground, and Tube stations
- Cycles through up to 5 configured stops
- Scrolls between the next two arrivals on the LCD
- Web UI (served from the Pico W) to set stop IDs and display labels — no reflashing needed
- Configuration persisted to EEPROM

## LCD layout

```
Row 0: <stop label>
Row 1: <destination or line>     <Xm>   ← scrolls every 4s
```

Examples:

| Stop type   | Row 0          | Row 1              |
|-------------|----------------|--------------------|
| Bus         | My Bus Stop    | 123 Somewhere   3m |
| DLR         | City DLR       | Terminus        2m |
| Overground  | My Station     | Somewhere      12m |

## Hardware

| Part | Details |
|------|---------|
| Microcontroller | Raspberry Pi Pico W |
| Display | 16×2 LCD with I2C backpack (address `0x27`) |
| Connection | I2C — SDA → GP4, SCL → GP5 (default for earlephilhower core) |

## Software

Built with [PlatformIO](https://platformio.org) using the [earlephilhower RP2040 Arduino core](https://github.com/earlephilhower/arduino-pico).

**Dependencies** (installed automatically by PlatformIO):
- `marcoschwartz/LiquidCrystal_I2C` — LCD driver
- `bblanchon/ArduinoJson` — JSON parsing

## Setup

### 1. Clone and configure

```bash
git clone https://github.com/hskuan/picobusarrival.git
cd picobusarrival
cp src/config.h.example src/config.h
```

Edit `src/config.h` and set your WiFi credentials:

```cpp
const char* WIFI_SSID     = "your-wifi-ssid";
const char* WIFI_PASSWORD = "your-wifi-password";
```

### 2. Build and flash

```bash
pio run --target upload
```

### 3. Configure stops via the web UI

1. On first boot the LCD shows the Pico W's IP address
2. Open that address in a browser
3. Enter TfL stop IDs and optional display labels, then click **Save & Apply**

![Web UI](docs/webui.png)

## Finding TfL stop IDs

Use the TfL StopPoint search API, substituting your station name and mode:

```
https://api.tfl.gov.uk/StopPoint/Search?query=<station name>&modes=<mode>
```

Supported modes: `bus`, `dlr`, `overground`, `tube`, `elizabeth-line`

Common stop ID formats:

| Mode | Format |
|------|--------|
| Bus | `490…` |
| DLR | `940GZZD…` |
| Overground | `910G…` |
| Tube | `940GZZU…` |
| Elizabeth line | `910G…` |

## Timing defaults

All timings are in `src/config.h`:

| Constant | Default | Description |
|----------|---------|-------------|
| `UPDATE_INTERVAL` | 30 s | How often to fetch fresh data from TfL |
| `CYCLE_INTERVAL` | 15 s | How long to show each stop before rotating |
| `SCROLL_INTERVAL` | 4 s | How long each arrival shows before scrolling |

## Project structure

```
src/
  main.cpp          — application logic
  config.h          — your local config (git-ignored)
  config.h.example  — template to copy
platformio.ini      — build configuration
```
