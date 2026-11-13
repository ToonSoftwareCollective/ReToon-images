# Apps

An app is a small JavaScript program that draws its own card on a page, and
optionally a full screen that opens when the card is tapped. The apps, the
guide to writing one (also for AI assistants) and how to try one before it
is published are in the public catalogue
[ToonSoftwareCollective/ReToon-apps](https://github.com/ToonSoftwareCollective/ReToon-apps).

## Installing

In the web interface, open **Apps**. It has two parts:

- **Installed**: the apps on this Toon with their settings. Opening it asks
  the catalogue for updates (again with **Check for updates**); the top line
  and each app say whether a newer version is there, and the app then shows
  **Update to x.y.z** at the top of its card. **Remove** is at the bottom.
- **Available**: what the catalogue has that is not installed, with
  **Install**.

The screen restarts after each of these, because it loads its apps when it
starts. Updating keeps the app's settings; removing deletes them.

**Update apps by themselves** (at the top of Apps, off by default) updates
the installed apps at the moments ReToon looks for an update of itself
(System, **Update by itself**: every hour, every night or every week; with
**Never** apps are not updated by themselves either). When ReToon itself has
an update, that goes first; the apps follow at the next look. Each app that
the catalogue has a newer version of, and that this ReToon can run, is
updated with its settings kept; the screen then restarts and a banner says
which apps were updated.

On a page, an app card gets the cells it has in the page's grid
(`position`, see [screen.md](screen.md#the-grid)); a new one starts as tall as
the app asks for.

An app whose `api` is higher than this ReToon supports cannot be installed:
the button says "Needs a newer ReToon".

Nothing about installing is on the Toon's screen itself.

## How it works

The catalogue's CI publishes every app as `<id>-<version>.tar.gz` in its
release `apps`, with `index.json` (for the web page) and `index.txt` (one
line per app: id, version, api, sha256, file name). `toon-apps` on the Toon
does the work:

| Command | What |
|---|---|
| `toon-apps list` | Print the catalogue's `index.json` |
| `toon-apps install <id>` | Fetch the app, check its sha256 and api level, unpack it next to the old version and swap it in |
| `toon-apps remove <id>` | Remove the app and its settings |
| `toon-apps update` | Update every installed app the catalogue has a newer version of (and this ReToon can run); a line per app updated. `toon-autoupdate` runs it when `config/apps-autoupdate` exists |

| Path | What |
|---|---|
| `/mnt/data/toon-os/apps/<id>/` | The installed app: `manifest.json` and its code |
| `/mnt/data/toon-os/config/apps/<id>.json` | Its settings, saved by the web interface |
| `/usr/share/toon/apps-api` | The app interface level of this ReToon |

`TOON_APPS_SRC` points `toon-apps` at another catalogue, for testing.

What an app can use at each level is described in the catalogue's
[guide to writing apps](https://github.com/ToonSoftwareCollective/ReToon-apps/blob/main/docs/writing-apps.md).

ReToon checks an app's level since 1.5.

| Level | Since | Added |
|---|---|---|
| 1 | before 1.1 | cards, screens, settings, `retoon.fetch`, timers |
| 2 | 1.1 | `retoon.watch`, `retoon.state`, icons, circles, lines, flows, `el.set` |
| 3 | 1.15 | `retoon.notify(msg, fn)`: a banner or popup (the JSON of [mqtt.md](mqtt.md#notifications-on-the-screen)), `fn(button)` after a tap on a button, `fn(null)` when closed without one; `retoon.sound(name, volume)` |
| 4 | 1.19 | `retoon.fetch` with `user`, `password`, `auth` (basic/digest) and the answer's `headers`; `el.time`, a time picker |
| 5 | 1.20 | `retoon.publish(topic, payload, { retain })`, `retoon.subscribe(pattern, fn)` with `fn(topic, payload)`, `retoon.unsubscribe(pattern)`: MQTT on the Toon's own broker ([mqtt.md](mqtt.md#the-toons-own-broker)), for the Toon's state (`retoon/boiler`, ...), its commands (`retoon/boiler/set`, ...), and data shared between apps and devices |
| 6 | 1.33 | `el.image({ url, every, user, password, auth, headers, x, y, w, h, align, radius })`: a JPEG (baseline, not progressive) or PNG from a URL, for example a camera snapshot. Fitted into `w` × `h` (standard 320 × 240) without growing; `every` fetches a new one that often (seconds, at least 2) while it is on the screen. A picture is kept per URL and size, so drawing the card again shows it at once. A Home Assistant camera: `url: ha + '/api/camera_proxy/camera.door?width=640'` with `headers: { Authorization: 'Bearer ' + token }`. Keep pictures small (640 pixels wide is plenty): a Toon 1 needs about a second for a large one |
| 7 | 1.36 | `retoon.call("domain.service", data, { response })`: a Home Assistant service over the Toon's own connection, so an app needs no token of its own. Only the services listed in the manifest's `"services"` (`"media_player.join"`, or `"media_player.*"` for the whole domain); the web interface shows that list with the app, in the catalogue too. `data` is the service data with `entity_id` in it; `response: true` for a service that answers (`calendar.get_events`, `weather.get_forecasts`). A Promise of the answer (`null` without one); it rejects with Home Assistant's reason. A call made while the Toon is not connected to Home Assistant (just after a start, or during a short drop) waits for the connection, up to a minute (from 1.37; 1.36 rejects at once), then rejects with the connection's status. A `toon.*` or `mqtt.*` `entity_id` goes to the Toon itself. Also from this level `retoon.watch` calls `fn(id)` when an attribute changes too (a speaker's volume or title, a weather entity's temperature), not only the state |
| 8 | 1.39 | `el.image` with `camera: "camera.door"`: a Home Assistant camera over the Toon's own connection; the Toon has Home Assistant sign a short-lived address for it, so an app needs no token. Only the cameras listed in the manifest's `"cameras"` (`"camera.door"`, or `"camera.*"` for any); the web interface shows that list with the app. Without `stream` it is a snapshot (with `every` as before), sized by Home Assistant to `w` × `h`. `stream: true`: MJPEG, the frames as they come, from a `camera` (Home Assistant's `camera_proxy_stream`) or from a `url` that gives `multipart/x-mixed-replace` (JPEG or PNG frames); `fps` at most that many a second (1–10, default 5). A frame that comes while the one before is still being decoded is skipped, so the picture never runs behind. A stream is open only while its picture is on the screen, and opens again by itself when it ends (Home Assistant ends one every 30 seconds). JPEGs are decoded with libjpeg-turbo (NEON on a Toon 2), progressive ones too from this version, and one much larger than shown at 1/2, 1/4 or 1/8 of its size. Measured with a 640 × 360 stream at 5 frames a second: a Toon 2 shows all 5 (about half its processor, the page included), a Toon 1 about 4 (nearly all of it); a 1280 × 720 stream at 15: a Toon 2 about 8 with `fps: 10`, a Toon 1 about 2. Keep streams small, and put them on a page that is not always shown. Home Assistant gives no pictures of cameras it only has as WebRTC (newer Google Nest cameras): those show its placeholder. Also from this level `every` (and a stream) only fetches while the picture is on the screen, not on a page next to it |
| 9 | 1.40 | The card's own colour: `el.set({ bg: "#1d4e89" })` on the `el` that the `retoon.tile` function gets colours the whole card, its padding and rounded corners too, instead of the standard dark grey; `border`, `borderColor` and `radius` work the same way. On the `el` of `retoon.screen` it colours the app's whole screen. The card goes back to the standard colour when the app draws it again without it, so set it each time (a colour that follows a state, for example). Before this level the same call only coloured the area inside the card's padding. `el.set({ points: [...] })` on a line moves it (with `width`, `color`, `opacity`): a clock's hand or a meter's needle is one line. `el.set` on a circle takes `x`, `y` as its centre and `r` as its radius, as `el.circle` does (before: its top left). Colours may also be the dashboard's names (`red`, `teal`, `white`…). Manifest settings of type `color` (the web interface shows the colour list, with an own colour; the value is a name or `#rrggbb`, empty for the app's own) and `select` (`options`: values, or `{ value, label }`) |
