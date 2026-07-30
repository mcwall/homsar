# homsar controller

An ESP32 that presents itself to a host as a Bluetooth Low Energy keyboard and
turns button presses into keystrokes.

The device advertises as `Homsar Controller` with the HID service and the
keyboard appearance, so any host that speaks HID-over-GATT (Linux, macOS,
Windows, Android, iOS) pairs with it as an ordinary wireless keyboard — no
driver or companion app. Bonds are stored in NVS, so a host that has paired
once reconnects on its own after a reboot.

## Buttons

Nine momentary buttons in a 3×3 grid, numbered down each column. Each sends the
matching numeral key:

```
 ┌───┬───┬───┐
 │ 1 │ 4 │ 7 │
 ├───┼───┼───┤
 │ 2 │ 5 │ 8 │
 ├───┼───┼───┤
 │ 3 │ 6 │ 9 │
 └───┴───┴───┘
```

### The harness

The buttons are not nine separate signals. Every button bridges one of two
shared lines to one of six sense lines, so the whole keypad rides on eight wires
and eight GPIOs rather than ten.

Each button is identified by the pair of wire colours it joins:

|            | Green    | Brown    |
|------------|----------|----------|
| **Orange** | button 1 | button 8 |
| **Black**  | button 2 | button 9 |
| **Red**    | button 4 | button 3 |
| **Yellow** | button 5 | —        |
| **Blue**   | button 6 | —        |
| **Purple** | button 7 | —        |

Twelve intersections, nine of them populated. The three empty cells on the brown
line are simply unwired; the firmware logs a warning if one of them ever closes,
which is a useful signal that the loom does not match this table.

### Pin assignment

The board is an **MH-ET LIVE ESP32 MiniKit** (D1 Mini form factor). All eight
wires land in one 2×4 block on the two inner header columns, rows 3–6.

The ribbon is a resistor-code rainbow, so its conductors run black, brown, red,
orange, yellow, green, blue, purple. The pins are assigned in exactly that
order: split the cable in half and each half drops onto one column, top to
bottom, with no wire crossing another.

```
   inner-left           inner-right
   IO22   1 black       IO26   5 yellow
   IO21   2 brown  *    IO18   6 green  *
   IO17   3 red         IO19   7 blue
   IO16   4 orange      IO23   8 purple

   * driven line; the other six are sensed
```

Which wire is a drive rather than a sense is purely a software choice, so the
two drive lines simply fall where the ribbon puts them — row 4 of each column.

| # | Wire   | Role  | GPIO | Buttons on this line |
|--:|--------|-------|-----:|----------------------|
| 1 | Black  | sense | 22   | 2, 9                 |
| 2 | Brown  | drive | 21   | 8, 9, 3              |
| 3 | Red    | sense | 17   | 4, 3                 |
| 4 | Orange | sense | 16   | 1, 8                 |
| 5 | Yellow | sense | 26   | 5                    |
| 6 | Green  | drive | 18   | 1, 2, 4, 5, 6, 7     |
| 7 | Blue   | sense | 19   | 6                    |
| 8 | Purple | sense | 23   | 7                    |

And the same thing from the button's point of view:

| Button | Sends | Grid position    | Drive wire | Sense wire | GPIO pair |
|-------:|-------|------------------|------------|------------|-----------|
| 1      | `1`   | column 1, top    | Green      | Orange     | 18 / 16   |
| 2      | `2`   | column 1, middle | Green      | Black      | 18 / 22   |
| 3      | `3`   | column 1, bottom | Brown      | Red        | 21 / 17   |
| 4      | `4`   | column 2, top    | Green      | Red        | 18 / 17   |
| 5      | `5`   | column 2, middle | Green      | Yellow     | 18 / 26   |
| 6      | `6`   | column 2, bottom | Green      | Blue       | 18 / 19   |
| 7      | `7`   | column 3, top    | Green      | Purple     | 18 / 23   |
| 8      | `8`   | column 3, middle | Brown      | Orange     | 21 / 16   |
| 9      | `9`   | column 3, bottom | Brown      | Black      | 21 / 22   |

### How the scan works

No resistors needed. The six sense pins are inputs with their internal pull-ups
on, so they idle high. The firmware pulls green low, reads all six sense lines,
releases it, then does the same for brown — a full pass every 5 ms. A button
closing at an intersection drags its sense line low while that button's drive
line is the one being pulled down, which is what identifies it.

Between passes, and while the other line is being scanned, an idle drive line is
left high-impedance rather than driven high. That detail matters: buttons 1 and
8 share the orange sense line, so if green were driven high while brown was
driven low, pressing both would short the two pins straight through the closed
switches.

Because the sense pull-ups are weak (roughly 45 kΩ), the scan waits 150 µs after
selecting a drive line before reading, giving the harness capacitance time to
charge. Lengthen `settle_us` in [main/main.c](main/main.c) if you extend the
loom and start seeing phantom presses.

There are no diodes in the matrix, so in principle certain three-button
combinations would produce a phantom fourth. In practice it cannot arise here:
buttons 1-2, 4-5 and 7-8 are rocker pairs that cannot close together, and the
keypad is only ever used one button at a time.

### Why these pins

They are chosen for a short loom rather than for capability. Eight adjacent pins
in a 2×4 block beat eight scattered ones, and every pin in that block is free,
output-capable and non-strapping.

The tradeoff: only IO26 is RTC-capable, so this layout cannot wake from deep
sleep on a button press. That is fine here — the controller is USB-powered and
never sleeps. If it ever goes to battery, the pins have to move to the
RTC-capable set (4, 13, 14, 25, 26, 27, 32, 33), which is scattered across all
four header columns.

Using these eight also spends the default I²C pins (21, 22) and the VSPI trio
(18, 19, 23). Nothing in this project wants them, but adding an I²C display
later would mean relocating two wires.

Pins left alone on purpose:

| Pins                | Silk on the MiniKit    | Reason                                          |
|---------------------|------------------------|-------------------------------------------------|
| 6–11                | CLK, SDD, SD1, SD2, SD3, CMD | Wired to the SPI flash                    |
| 0, 2, 5, 12, 15     | IO0, IO2, IO5, TDI, TDO | Strapping pins — a held button at reset changes boot mode |
| 1, 3                | TXD, RXD               | Console UART, used by `idf.py monitor`           |
| 34–39               | IO34, IO35, SVP, SVN   | Input-only with no internal pull-up, and they cannot drive |

Two traps on this board specifically. The silkscreen labels GPIO 12 and 15 as
**TDI** and **TDO**, which makes them look like spare pins — they are strapping
pins. GPIO 12 is the worst of them: it is sampled at reset to pick the flash
voltage, so pulling it high, which a button with a pull-up does, makes most
boards fail to boot. And **SVP**/**SVN** are GPIO 36 and 39, input-only with no
internal pull-up, so they cannot serve as either a drive or a sense line.

Also worth checking once: GPIO 16 and 17 are the PSRAM pins on WROVER modules.
The MiniKit normally carries a plain WROOM-32, where they are free.

### Changing the layout

Everything about the wiring lives at the top of [main/main.c](main/main.c):
`k_drive_pins` and `k_sense_pins` map wire colours to GPIOs, and `k_keymap`
maps each intersection to a keycode. Nothing downstream needs touching — the
scanner sizes itself from those arrays, and an intersection left out of the
keymap is ignored.

Keycodes come from [main/hid_keycodes.h](main/hid_keycodes.h); any usage in the
0x00–0xE7 range works, and modifiers (Ctrl, Shift, Alt, GUI) can be held
alongside six regular keys. Six is the limit on simultaneous non-modifier keys,
set by the report descriptor; a seventh concurrent press is dropped with a log
warning rather than corrupting the report.

## Build and flash

Built with ESP-IDF v5.4. From this directory:

```sh
idf.py set-target esp32     # first time only
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Exit the monitor with Ctrl-]. Button presses show up in the log, so it is the
quickest way to check the loom before pairing:

```
I (1234) homsar: button 5 (green/yellow) pressed
```

Then pair with `Homsar Controller` from the host's Bluetooth settings.

`idf.py erase-flash` clears stored BLE bonds if pairing ever gets stuck.

## How it fits together

- [main/ble_kbd.c](main/ble_kbd.c) — brings up the BLE controller and the
  NimBLE host, registers the HID service through the `esp_hid` component,
  handles advertising and pairing, and exposes a small press/release API. It
  owns the report state, so callers never assemble a HID report themselves.
- [main/keymatrix.c](main/keymatrix.c) — scans the drive lines every 5 ms on a
  background task and reports an intersection's change only after the new
  reading has held for 20 ms, which debounces the contacts in software. It knows
  nothing about wire colours or keycodes; it reports coordinates.
- [main/main.c](main/main.c) — the wiring tables and keymap, and the glue that
  turns an intersection event into a key press.

NimBLE is used rather than Bluedroid because it needs roughly 100 kB less RAM
and this device has no use for Bluetooth Classic. The stack settings live in
[sdkconfig.defaults](sdkconfig.defaults).

## Pairing security

The controller has no display, and its nine buttons cannot enter a six-digit
passkey — there is no zero and no confirm key — so it cannot take part in a
passkey exchange and pairs with Just Works. The link is still encrypted, but it
is not protected against an active man-in-the-middle during the pairing
exchange itself. If you add a display, switch `ble_hs_cfg.sm_io_cap` to
`BLE_SM_IO_CAP_DISP_ONLY` and `sm_mitm` to 1 in [main/ble_kbd.c](main/ble_kbd.c).
