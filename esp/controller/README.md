# homsar controller

An ESP32 that presents itself to a host as a Bluetooth Low Energy keyboard and
turns button presses into keystrokes.

The device advertises as `Homsar Controller` with the HID service and the
keyboard appearance, so any host that speaks HID-over-GATT (Linux, macOS,
Windows, Android, iOS) pairs with it as an ordinary wireless keyboard — no
driver or companion app. Bonds are stored in NVS, so a host that has paired
once reconnects on its own after a reboot.

## Wiring

| GPIO | Button | Sends |
|------|--------|-------|
| 27   | button | `a`   |

Wire a momentary switch between GPIO 27 and GND. Nothing else is needed — the
internal pull-up holds the pin high while the button is open, so a press reads
as a low.

GPIO 27 has no strapping or boot-time role, drives nothing during reset, and is
RTC-capable, so it can also act as a deep-sleep wake source if the firmware
ever needs one.

To change or extend the layout, edit `k_keymap[]` at the top of
[main/main.c](main/main.c). Keycodes come from
[main/hid_keycodes.h](main/hid_keycodes.h); any usage in the 0x00–0xE7 range
works, and modifiers (Ctrl, Shift, Alt, GUI) can be held alongside six regular
keys.

Pins to avoid when adding more: 6–11 are wired to the SPI flash, 1 and 3 are the
console UART, 34–39 are input-only and have no internal pull-up (they need an
external one), and 0, 2, 5, 12 and 15 are strapping pins. GPIO 25, 26, 32 and 33
are the other clean RTC-capable choices.

## Build and flash

```
. ~/esp/esp-idf/export.sh
idf.py set-target esp32     # only needed once
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Then pair with `Homsar Controller` from the host's Bluetooth settings and the
buttons start producing keystrokes.

## How it fits together

- [main/ble_kbd.c](main/ble_kbd.c) — brings up the BLE controller and the
  NimBLE host, registers the HID service through the `esp_hid` component,
  handles advertising and pairing, and exposes a small press/release API. It
  owns the report state, so callers never assemble a HID report themselves.
- [main/buttons.c](main/buttons.c) — samples the configured GPIOs every 5 ms on
  a background task and reports a change only after the new level has held for
  20 ms, which debounces the contacts in software.
- [main/main.c](main/main.c) — the keymap, and the glue that turns a button
  event into a key press.

NimBLE is used rather than Bluedroid because it needs roughly 100 kB less RAM
and this device has no use for Bluetooth Classic. The stack settings live in
[sdkconfig.defaults](sdkconfig.defaults).

## Pairing security

The controller has no display or keypad, so it cannot take part in a passkey
exchange and pairs with Just Works: the link is encrypted, but it is not
protected against an active man-in-the-middle during the pairing exchange. If
you add a display, switch `ble_hs_cfg.sm_io_cap` to `BLE_SM_IO_CAP_DISP_ONLY`
and `sm_mitm` to 1 in [main/ble_kbd.c](main/ble_kbd.c).
