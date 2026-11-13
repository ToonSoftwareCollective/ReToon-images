# Zigbee and Thread

The Toon 2 has one 802.15.4 radio (GreenPeak GP711). ReToon uses it for one
of two things at a time:

- **Zigbee**, for zigbee2mqtt. The Zigbee stack runs in zigbee2mqtt, which
  uses the Toon's radio over the network.
- **Thread**, as a border router. The Toon runs the OpenThread Border Router
  and connects a Thread network to your home network, so Matter devices that
  use Thread can be added in Home Assistant.

## Choosing

Web interface, Connections, "Zigbee or Thread": pick one and save.
The radio switches over within seconds. Zigbee is the standard; the card
shows its address, `tcp://<address of the Toon>:6638`.

## Zigbee

In zigbee2mqtt, set the adapter to `zoh` (Zigbee on Host) and the port to
the address on the card. Use a channel that your other Zigbee network does
not use. The connection has no password and no encryption.

## Thread

The first time Thread is chosen, the Toon starts a network of its own,
`ReToon-<last four of its id>` on channel 25. The card shows its state
(leader, router, child), the network and how many devices are attached.

To use it in Home Assistant:

1. Add the **OpenThread Border Router** integration with the address on the
   card, `http://<address of the Toon>:8081`.
2. Under Settings, Thread, make the ReToon network the **preferred
   network**.
3. On a phone with the Home Assistant app, sync the Thread credentials
   (Settings, Companion app, Troubleshooting).
4. Add the Matter device in Home Assistant.

On Android, Google Home may own the phone's preferred Thread network; the
app then cannot change it. Two ways out:

- Let the Toon join that network: get its dataset (in Home Assistant, from a
  network it knows the credentials of) and paste it under "Join another
  Thread network". The Toon then is one more border router in that network.
- Add the device in Google Home and share it with Home Assistant.

"Start a new network" makes a new one; devices in the old network have to
be added again.

The REST interface on port 8081 is what Home Assistant uses. It is open to
anyone on your network and can read the network key: only use it on a home
network you trust.

### What it does and does not do

- Routing between the Thread network and your home network over IPv6, with
  the Toon's prefix announced on both sides. NAT64 is off: Thread devices
  reach your home network and Home Assistant, not IPv4 addresses on the
  internet.
- Battery devices (sleepy end devices): the radio tells a device that polls
  when there is data waiting for it, for up to 12 such devices at a time.
  Not yet tried with a real battery device.
- No Thread 1.2 extras that need precise timing in the radio (CSL, enhanced
  ACKs). Devices fall back to the plain way.
- Toon 2 only; the Toon 1 image does not have it.

## How it works

`gp711-rcpd` (package `gp711`) drives the chip and offers it as a Spinel
radio co-processor, the protocol both Zigbee on Host and OpenThread speak:
on TCP port 6638 for Zigbee, on the pseudo-terminal `/run/gp711-rcp` for
Thread. `otbr-agent` (package `ot-br-posix`, started by `S62toon-thread`)
runs the Thread stack, the border routing, mDNS and the REST interface.

| Path | What |
|---|---|
| `config/radio.conf` | `mode=thread` or `mode=off`; without it, Zigbee |
| `/run/toon/thread/` | The Thread settings while running (network, frame counters, children) |
| `toon-os/thread/` | Their copy on the data partition, saved every 10 minutes when changed and when stopping |

`toon-thread status | form | join <dataset> | save` is the helper behind the
web interface; `ot-ctl` talks to `otbr-agent` directly (`ot-ctl state`,
`ot-ctl child table`, `ot-ctl netdata show`).

In Thread mode the Toon forwards IPv6 (`net.ipv6.conf.all.forwarding=1`)
and keeps accepting router advertisements on WLAN (`accept_ra=2`).
