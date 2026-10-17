# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Retro LCD-style watchface for Pebble Time 2 (platform: `emery`). Uses C for the watchface and JavaScript for phone-side settings/weather. All digits are hand-drawn seven-segment readouts with stippled "ghost" unlit segments for the authentic LCD look.

## Build & Test Commands

```sh
pebble build                      # Build the watchface
pebble install --emulator emery   # Install on emulator
pebble install --phone <ip>       # Install to paired phone
pebble logs --emulator emery      # View live logs
pebble screenshot                 # Capture screenshot
pebble emu-battery --percent 50   # Simulate battery level
pebble emu-app-config             # Test settings page
```

If dependencies need updating: `pebble package install @rebble/clay`

## Architecture

### C Watchface (`src/c/pebble-casual-salaryman.c`)

Single-file watchface using layered rendering:
- **Segment displays** (`prv_create_seg_display`) for time, seconds, date, week, temperature and alarm: a `pattern` like `"88:88"` drawn as a stippled ghost, with the lit segments of the text on top; digit size, segment thickness and spacing are per display
- **Custom layer update procs** for battery bars, weekday strip, bottom band
- **Text layers** only for labels (Gothic) and the header (Orbitron)
- **Tick service** subscribes to SECOND_UNIT while seconds are shown, MINUTE_UNIT otherwise (ShowSeconds off, or inside the night range); the tick handler re-checks at each minute and switches

Layout constants:
- `ROW_TOP_Y (41)`: Weekday strip on a black band
- `ROW_MIDDLE_Y (70)`: Main time/date/week display
- `ROW_BOTTOM_Y (173)`: Black band with TEMP and ALARM LCD windows

Settings persistence uses `persist_read_data`/`persist_write_data` with key `1` for `ClaySettings` struct; key `2` holds the last weather reading and its arrival time (`SavedWeather`), blanked after 3 h. Add new fields only at the end of the struct, so older saved copies still load (missing fields keep their defaults).

### JavaScript Phone-Side (`src/pkjs/`)

- `index.js`: Clay settings init, weather fetching from Open-Meteo API, geolocation. Fetches on JS ready and whenever the watch sends `RequestWeather` (every 30 min, driven by the watch's minute tick)
- `config.js`: Clay settings schema (clock format, show seconds, hide seconds at night + hours, date format, first day of week, temperature unit)

### Message Keys

Communication between watch and phone uses these keys (defined in package.json):
- `ShowSeconds` - boolean toggle
- `ClockFormat` - "0" watch setting, "1" 24h, "2" 12h (Clay sends selects as strings)
- `TemperatureUnit` - "0" Celsius, "1" Fahrenheit
- `FirstDayOfWeek` - "0" Monday, "1" Sunday
- `DateFormat` - "0" DD-MM, "1" MM-DD
- `HideSecondsAtNight` - boolean toggle; per-minute instead of per-second ticks during the night range
- `NightStartHour` / `NightEndHour` - "0".."23"; the range wraps past midnight (22-7 = 22:00-06:59)
- `TemperatureTenths` - weather temperature in tenths of a degree Celsius (the watch converts to the chosen unit)
- `WeatherCondition` - WMO weather code
- `RequestWeather` - watch → phone: fetch weather now

Test settings without the config page: `pebble send-app-message --emulator emery --string <key id>=<value>`, with numeric key ids from `build/js/message_keys.json`.

## SDK Reference

Full docs: https://developer.repebble.com
