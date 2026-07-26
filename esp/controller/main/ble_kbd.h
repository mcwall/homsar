/*
 * BLE HID keyboard (HOGP peripheral) built on NimBLE + the esp_hid component.
 *
 * The device advertises as a keyboard, bonds with one host at a time and sends
 * standard 8-byte keyboard reports. Key state is tracked internally, so callers
 * only press and release; the report is assembled and sent for them.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Advertised name. Keep it <= 18 characters: the advertising payload also
 * carries flags, appearance and the HID service UUID, and all of it has to fit
 * in the 31-byte legacy limit. ble_kbd.c static-asserts this. */
#define BLE_KBD_DEVICE_NAME "Homsar Controller"

/* Simultaneous non-modifier keys the report descriptor can carry. */
#define BLE_KBD_MAX_KEYS 6

/**
 * @brief Bring up the BLE controller, NimBLE host and HID service, then start
 *        advertising.
 *
 * NVS must already be initialised — bonds are persisted there. Returns once the
 * stack is running; advertising begins asynchronously when the host syncs.
 */
esp_err_t ble_kbd_init(void);

/** @brief Whether a host is currently connected. */
bool ble_kbd_connected(void);

/**
 * @brief Press a key and send the updated report.
 *
 * Modifier usages (0xE0-0xE7) set a bit in the modifier byte; everything else
 * takes one of the BLE_KBD_MAX_KEYS slots. Pressing a key that is already down
 * is a no-op.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if no host is connected,
 *         ESP_ERR_NO_MEM if all key slots are occupied.
 */
esp_err_t ble_kbd_press(uint8_t keycode);

/** @brief Release a previously pressed key and send the updated report. */
esp_err_t ble_kbd_release(uint8_t keycode);

/** @brief Release every held key and modifier. */
esp_err_t ble_kbd_release_all(void);

/** @brief Press a key, hold it for @p hold_ms, then release it. Blocks. */
esp_err_t ble_kbd_tap(uint8_t keycode, uint32_t hold_ms);

/**
 * @brief Keyboard LED state most recently pushed by the host.
 *
 * Bit 0 Num Lock, bit 1 Caps Lock, bit 2 Scroll Lock, bit 3 Compose, bit 4 Kana.
 */
uint8_t ble_kbd_led_state(void);

#ifdef __cplusplus
}
#endif
