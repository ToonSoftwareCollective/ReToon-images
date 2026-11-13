# Bluetooth proxy

The Toon 2 has a Bluetooth radio (in the WiFi chip). ReToon lends it to Home
Assistant: the Toon becomes a *Bluetooth proxy*, the same thing an ESP32 with
ESPHome's `bluetooth_proxy` does. Bluetooth devices near the Toon,
thermometers, plant sensors, trackers, Shelly BLU, SwitchBot, Xiaomi, Govee and
so on, then work in Home Assistant as if they were next to it. Home Assistant
does the decoding; the Toon only passes on what it hears.

## How it shows up

The Toon speaks the ESPHome native API (TCP port 6053) and announces itself
on the network (`_esphomelib._tcp`). Nothing has to be installed in Home
Assistant: its built-in ESPHome integration finds the Toon by itself. Under
Settings, Devices & services it appears as a discovered ESPHome device named
"ReToon <id>"; press **Add**. From then on:

- Under Settings, Devices & services, Bluetooth, the Toon is listed as a
  remote scanner next to any USB adapter or other proxy.
- Every Bluetooth integration in Home Assistant sees what the Toon hears.
  A BTHome or Xiaomi sensor near the Toon gets discovered the usual way.
- With several ReToons, each is a scanner; Home Assistant uses the one
  that hears a device best.
- The ESPHome device's options in Home Assistant set passive or active
  scanning. Passive (the default) only listens; active also asks devices
  for their extra data and uses the radio more.

The Toon has no entities of its own on this link (sensors and thermostat go
over [MQTT](mqtt.md)); it only offers the Bluetooth proxy, and no connections
to Bluetooth devices, only advertisements.

## Settings

Web interface, Connections, Bluetooth proxy. It is on by default;
the card shows whether Home Assistant is connected and how many
advertisements came in during the last minute. The link has no password and
no encryption, so fill in the address of Home Assistant under "Only from
this address"; nothing else on the network can then use the radio. Untick the
box to switch the proxy off.

Add the device in Home Assistant *without* an encryption key (leave the key
field empty when it asks).

## WiFi and Bluetooth share the radio

Scanning takes the radio away from WiFi for part of the time. The Toon only
scans while Home Assistant is connected and subscribed, and listens 40 ms out
of every 100 ms. On the test unit, ping times did not get worse with the
scan running. The duty cycle can be changed in
`/mnt/data/toon-os/config/esphome.conf`: `scan_window_ms=` and
`scan_interval_ms=` (defaults 40 and 100). A longer window hears more of a
device's advertisements; sensors send every few seconds, so missing some is
fine.

## Under the hood

`toon-esphomed` is a small C program:
plaintext ESPHome API frames, Avahi for the announcement, a raw HCI socket on
`hci0` for the LE scan (next to `bluetoothd`, which keeps running). Each HCI
advertising report becomes a `BluetoothLERawAdvertisement` (address, RSSI,
address type, raw data), batched up to 16 per message or 50 ms. State is in
`/run/toon/esphome`. It starts from `S71toon-esphomed`, which does nothing on
a unit without `hci0` (the Toon 1).
