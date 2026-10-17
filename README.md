# pebble-casual-salaryman

"Casual Salaryman": a retro LCD-style watchface for the Pebble Time 2 (platform `emery`), written in C
using the Pebble SDK.

Styled after classic digital LCD watches, with a "CASUAL SALARYMAN" header,
labels next to each physical button, and a framed LCD panel. Unlit segments
show as faint "ghost" `8`s, as on a real LCD.

## What's on the face

- **Weekday strip**: MO–SU, with today highlighted (week starts Monday or Sunday)
- **Time**: large segmented `HH:MM`, with seconds alongside and an AM/PM indicator in 12-hour mode
- **Date and week**: `DD-MM` or `MM-DD`, plus the ISO week number
- **Battery**: 10-bar gauge
- **TMP**: current temperature from [Open-Meteo](https://open-meteo.com),
  based on your phone's location and refreshed every 30 minutes
- **AL**: time of the next alarm set on the watch (blank if there is none)

## Settings

The settings page on the phone uses [Clay](https://github.com/pebble-dev/clay):

| Setting               | Options                                  | Default            |
| --------------------- | ---------------------------------------- | ------------------ |
| Clock format          | Watch setting / 24-hour / 12-hour        | Watch setting      |
| Show seconds          | On / off                                 | On                 |
| Hide seconds at night | On / off, with night start and end hours | Off (22:00–06:59)  |
| Date format           | DD-MM / MM-DD                            | DD-MM              |
| First day of week     | Monday / Sunday                          | Monday             |
| Temperature unit      | Celsius / Fahrenheit                     | Celsius            |

When seconds are hidden, whether turned off or during the night hours, the
watchface updates once a minute instead of every second, which saves battery.
The night range can wrap past midnight. Settings are saved on the watch.

## Building & running

```sh
pebble build                          # build for emery (Pebble Time 2)
pebble install --emulator emery       # install on the emery emulator
pebble install --phone <ip>           # install to a paired phone
pebble emu-app-config                 # open the settings page in the emulator
```

Clay is a dependency; if it needs installing or updating:

```sh
pebble package install @rebble/clay
```

## Project layout

```
src/c/pebble-casual-salaryman.c   Watchface C source
src/pkjs/config.js                Clay settings schema
src/pkjs/index.js                 Wires up Clay, fetches weather (Open-Meteo)
resources/fonts/                  Orbitron font + its SIL OFL license text
package.json                      Project metadata (UUID, platform, resources, message keys)
wscript                           Build rules — usually no need to edit
```

This project targets `emery` (Pebble Time 2) only. The app requests the
`location` capability for weather.

## Fonts

All digits (time, seconds, date, week, temperature, alarm) are seven-segment
digits drawn in code, so they share one segment shape at every size. The header
uses [Orbitron](https://github.com/theleagueof/orbitron) (SIL Open Font
License; see `resources/fonts/`).

## Documentation

Full SDK docs, tutorials, and API reference: <https://developer.repebble.com>
