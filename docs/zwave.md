# Z-Wave

Both Toons have a Z-Wave controller: a 500-series chip in the Toon 2, an
older one (SDK 4.55) in the Toon 1. ReToon offers it on the network, so the
software that knows Z-Wave devices can run elsewhere: Z-Wave JS in Home
Assistant. Z-Wave JS drives both; the Toon 1's controller was checked with
Z-Wave JS 15.31 (driver ready, controller read out).

## Switching it on

Web interface, Connections, Z-Wave: tick the box and save. The card
then shows the address, `tcp://<address of the Toon>:6639`. In Home Assistant
add the Z-Wave integration and give it that address where it asks for the
USB path of a stick.

The connection has no password and no encryption (Z-Wave JS has neither for
this kind of connection). Fill in the address of Home Assistant under "Only
from this address", so nothing else on the network can use the radio.

## The network stays

The Z-Wave network (its identity, the devices in it, their routes) is kept by
the chip, not by software. Devices that were added with the Toon's original
software are still there under ReToon, and stay when the setting is switched
off and on. Their names are not: Z-Wave JS finds the devices again by asking
them. A device that was added with encryption needs the network's keys:
the setup app takes those over from the original software at install
(`config/zwave-keys.xml`; ReToon also reads them from the original
software's own place on the data partition), and the Z-Wave card shows them.
Fill them in under the Z-Wave JS integration's security keys: S0 legacy, and
S2 unauthenticated, authenticated and access control.

## Meter adapters

The Toon's meter adapter (Quby 6500-1400-6000, for the P1 port of a smart
meter, a pulse or laser sensor and gas) sends its readings with the Meter
Table Monitor command class, which Z-Wave JS does not know yet: in Home
Assistant they do not show up. ReToon reads them on the side: while the
Z-Wave radio is on, every adapter that sends a reading is listed on the
Z-Wave card. Tick one, and its readings become the ReToon's own values:

- on the screen as `toon.meter_n<node>_<what>` (for example
  `toon.meter_n17_import_t1`), to be put on any card;
- over MQTT to Home Assistant, as sensors of the ReToon: the totals as energy
  (kWh) or gas (m³) that only go up, for the energy dashboard, and the power
  (kW) or gas flow (m³/h) now;
- on the Toon's own broker as `retoon/meter`.

What each endpoint of the Toon's adapter is: 1 gas, 2 and 3 electricity in
and out of a pulse or laser sensor (solar panels), 4 and 5 electricity in and
out on tariff 2, 6 and 7 on tariff 1 (the P1 meter's 1.8.2, 2.8.2, 1.8.1 and
2.8.1), 8 heat. The adapter sends a reading every 10 seconds, but only of
what changed. With Z-Wave JS connected nothing changes for it: ReToon only
listens. Without a client, ReToon answers the radio's frames itself so the
readings still come. Untick the adapter once Z-Wave JS shows the readings
itself, or Home Assistant gets them twice. An adapter added with encryption
(S0, S2) cannot be read on the side.

## How it works

`toon-zwaved` (package `toon-zwave`) opens `/dev/ttymxc1` on a Toon 2 or
`/dev/ttymxc2` on a Toon 1 (115200 8N1, the chip's Serial API; the init
script picks the port by the device-tree model) and passes the bytes to one
TCP client on port 6639. A new
client takes the place of the one before it. `config/zwave.conf` on the data
partition holds `mode=remote` (or `local`: only for software on the Toon
itself, which does not exist yet), `allow=<address>` and `meters=<nodes>`
(the meter adapters that are read); without the file the daemon does not run.
State is in `/run/toon/zwave`; the meter adapters seen are in
`/run/toon/meters`, the readings in `/run/toon/meter` (`meter.c`: it takes
the Serial API's ApplicationCommandHandler frames as they pass, unpacks
CRC-16 and Multi Channel encapsulation and decodes Current Data Reports).
