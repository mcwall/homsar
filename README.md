# homsar
ADSB radar view with bluetooth controller

Two halves: a radar scope that runs in a browser on a wall display, and an
ESP32 keypad that drives it over Bluetooth. The scope is
[www/adsb/index.html](www/adsb/index.html); the controller is its own project
under [esp/controller](esp/controller/README.md).

## The ADS-B scope

A green-phosphor radar view of the traffic your own receiver is hearing, laid
out for a portrait kiosk screen: the scope on top, the three nearest contacts
in a table underneath.

It is one self-contained HTML file — no build step, no dependencies, no server
of its own. Everything is inline, so deploying it means copying one file
somewhere a browser can reach.

### Deploy
```
rsync www/adsb/index.html /mnt/haos/www/adsb/index.html

### Where the data comes from

The page polls a readsb / tar1090 receiver's `aircraft.json` every ten seconds
and keeps nothing between polls except the position trails. `DATA_URL` at the
top of the file points at the feed.

Two constraints on that fetch are worth knowing before you move the page
anywhere:

- readsb sends `Access-Control-Allow-Origin: *`, so the feed is readable
  cross-origin. That is what lets the page live somewhere other than the
  receiver.
- The feed is plain `http`. A page served over `https` cannot read it — the
  browser blocks it as mixed content, silently, and the scope simply shows
  `NO FEED`. **Serve the page over `http`.**

Contacts are dropped unless they carry a position and an altitude, their
position is fresher than `MAX_POS_AGE_S` (60 s), and they are inside
`MAX_RANGE_NM` (50 nm). readsb already computes range and bearing from the
receiver, so those are used when present and only computed locally
(haversine) when they are not.

Setting `DEMO_FALLBACK: true` synthesises a dozen moving aircraft whenever the
feed is unreachable, which is how to work on the layout away from the
receiver.

### What is drawn

The receiver sits at the centre, north-up, and everything is projected flat —
degrees of latitude and longitude scaled to nautical miles. At 50 nm the error
from ignoring the earth's curvature is far below one pixel, so there is no
reason for anything fancier.

- **Range rings** in white, five of them, labelled in nm at the 6 o'clock
  position. Bearing ticks every 30°, cardinals inside the edge.
- **Airports** as diamonds. Tier 1 (towered / major) are white and always
  drawn; tier 0 are gray and appear only once zoomed inside
  `LABEL_MAX_RANGE_NM` (40 nm), so the wide views stay readable.
- **Contacts** as circles, with a dashed trail of up to the last eight polls
  (~80 s of history) and a short stalk showing ground track.
- **The nearest three** are filled and ringed, and numbered to match the table
  rows.

Labelling changes with zoom, because zoomed out the nearest contacts pile up
on top of each other. Inside 40 nm every contact gets its callsign, altitude
and groundspeed. Beyond that only the top three are marked, with a numbered
badge on a leader line — and the badges fan around their dot until they find a
slot clear of the badges already placed, so two close contacts do not stack
their numbers. The detail lives in the table instead.

A rotating wedge sweeps the scope every six seconds. It is a CSS
`conic-gradient` on a pseudo-element, animated by transform alone, so it runs
on the compositor and costs nothing per frame — the canvas is only redrawn on
a poll, a keypress or a resize, not at 60 fps. It honours
`prefers-reduced-motion`.

### The HUD

Contact count, current range, a build stamp, a UTC clock, and a link
indicator. `LINK NO` blinks amber, and the scope prints how long it has been
since the last successful poll. That distinction matters on a wall display:
an empty scope with `LINK OK` means quiet skies, an empty scope with `LINK NO`
means something is broken.

### Portrait on a landscape display

The layout is designed for a 480×960 portrait screen. The Echo Show it runs on
reports a *landscape* viewport even when the display is stood on end, so the
page sizes a `#stage` element with the axes swapped and rotates it into place.
`CONFIG.ROTATE` sets the angle; `?rot=N` overrides it live — `?rot=0` for a
desktop browser, `?rot=270` if the display comes out upside down.

Everything else scales from there. A single `--s` custom property is derived
from the stage size against that 480×960 baseline, and every dimension in the
CSS is a multiple of it. The canvas reads the same number, so page text and
canvas text never disagree.

For the same reason the phosphor colour is defined once, as `--green-rgb` in
the CSS, and read from there by the canvas at boot. Changing that one value
recolours the whole scope — and setting it to something obvious like
`255,45,45` is a fast way to prove which copy of the file a kiosk is actually
running.

### Caching

Home Assistant serves `/local/` with a 31-day `Cache-Control`, so a kiosk will
cheerfully display a month-old copy of this page. The `<meta http-equiv>` tags
in the file are a hint at best — browsers ignore them for the document itself
and they cannot override a real HTTP header.

The lever that actually works is a changed URL: **load it with a fresh `?v=N`
every time you deploy.** The `BUILD` constant is rendered into the HUD so you
can see at a glance which copy you got; bump it whenever you change the file.

### Controls

The scope takes keyboard input, which is how the
[Bluetooth controller](esp/controller/README.md) drives it — the ESP32
advertises as an ordinary HID keyboard and its nine buttons send the nine
digits, so the number row is the command surface.

| Key | Action |
|-----|--------|
| `1` | Zoom out (longer range) |
| `2` | Zoom in (shorter range) |
| `3`–`9` | Reserved — accepted and ignored |

Range steps through 5, 10, 20, 30, 40, 50 nm and stops at the ends.

`3`–`9` are deliberately swallowed rather than left unhandled, so an unmapped
button cannot fall through to whatever the browser would otherwise do with a
keystroke. They are declared in the `DIGIT_CMD` table near the bottom of the
file; giving one a behaviour means filling in its entry and nothing else.

There are also a few keys from before the controller existed, useful when
debugging with a real keyboard attached: `+`/`-` or the arrow keys zoom, `L`
toggles contact labels, `S` toggles the sweep. And because the Echo Show has
no keyboard at all, a pair of `+`/`−` buttons sits in the corner of the scope.

### Tuning it for your receiver

The `CONFIG` block at the top of the script is the whole configuration
surface. At minimum, change `RECEIVER_LAT` / `RECEIVER_LON` (take them from
your receiver's `/data/receiver.json`), `DATA_URL`, and the `AIRPORTS` table —
the fields listed there are central Ohio, and they are only reference marks,
so add and remove freely. `TOP_N` sets how many rows the table holds; it is 3
because that is what fits above the fold on a 960 px-tall screen without
crowding the scope.
