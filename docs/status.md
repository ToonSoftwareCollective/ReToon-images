# Status

## Works and tested on a real unit

- Boot next to the original software, with fall-back and a rescue mode.
- Display, touch, backlight, WiFi, Bluetooth stack, sensors.
- Sound with `toon-sound` on both Toons: the Toon 1's codec and speaker, the
  Toon 2's buzzer driven by the Cortex-M4 ([sound.md](sound.md)).
- Hardware watchdog on both Toons (on the Toon 1 it counts while the CPU is
  busy; the driver pauses it in the i.MX27's idle state).
- Screen with Home Assistant pages (a five-page dashboard with 154 entities
  runs at a few percent CPU), WiFi setup on the screen, web interface with
  editor, import and settings.
- Web interface over https with the Toon's own certificate; login that
  leaves a long-lived access token on the Toon.
- Boiler module in OpenTherm and on/off mode: detection, switching, values.
- Z-Wave for Z-Wave JS, on the Toon 2 (in use with a meter adapter) and the
  Toon 1 (controller read out by Z-Wave JS; no devices in that network yet).
- Bluetooth proxy for Home Assistant (ESPHome native API): discovered,
  connected and listed as a scanner by Home Assistant 2026.10; it received
  the advertisements of some 30 devices in the house.

## Built, but not proven in real use

- A real heat request from the thermostat (on/off or OpenTherm), and with it
  everything in [thermostat.md](thermostat.md) that needs a boiler: the flow
  temperature rule, learning the heating rate, starting early, frost
  protection. The program, presets, holds and sensor average were tested
  without a boiler module.
- `https://` / `wss://` to Home Assistant: tested through a TLS proxy in
  front of an http installation, not against a real https installation or
  with certificate checking that succeeds.
- First boot without WiFi settings, hidden networks, open networks.
- Zigbee: the network forms; joining a device is untested.
- Thread: a Matter plug (IKEA GRILLPLATS) joined through the Toon and was
  added in Google Home, with the Toon as a router in that network. The
  Toon's own network with Home Assistant's OpenThread Border Router
  integration, and battery (sleepy) devices, are not tried yet. About half
  of the radio's first transmissions miss their ACK; retries cover it.

## Not done

- Pushing the Toon's sensors and thermostat to Home Assistant without a
  broker. The ESPHome API link exists now for the Bluetooth proxy; the
  entities are not on it yet.
- Encryption (Noise) on the ESPHome API link.
- Encrypted MQTT.
- Camera, picture and media-control cards; conditions on cards; on/off
  timelines in history graphs.
- State texts in other languages than English; emoji in names.
- P1 smart-meter port.
- Waking the screen on approach: the ultrasonic transducer only sees a hand
  at the bottom edge of the case.
- Air quality in ppm (eCO2/TVOC): the sensor gives a relative value only; ReToon
  shows a word learned from each Toon's own history.
- The time zone cannot be changed in the web interface.

## Tried and dropped

- **A browser on the Toon** (WPE WebKit + cog on the open GPU driver): Home
  Assistant's web interface keeps the single core at 100 % and is too slow to
  use.
