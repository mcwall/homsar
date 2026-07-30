/*
 * homsar controller — an ESP32 that presents itself to a host as a BLE
 * keyboard and turns button presses into keystrokes.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "ble_kbd.h"
#include "hid_keycodes.h"
#include "keymatrix.h"

static const char *TAG = "homsar";

/*
 * Nine buttons in a 3x3 grid, numbered down each column, each sending its own
 * numeral:
 *
 *     | 1 | 4 | 7 |
 *     | 2 | 5 | 8 |
 *     | 3 | 6 | 9 |
 *
 * They are not wired as nine separate signals. The harness is a 2x6 matrix:
 * every button bridges one of two shared lines (green, brown) to one of six
 * sense lines (orange, black, red, yellow, blue, purple). Twelve intersections,
 * nine of them populated. keymatrix.c drives green and brown low in turn and
 * reads the six sense lines, so the whole keypad costs eight pins.
 *
 * Wire colours name the indices below, so the tables read the same way as the
 * loom does. They are listed in ribbon order — the cable is a resistor-code
 * rainbow, black through purple.
 */
enum {
    DRIVE_BROWN,
    DRIVE_GREEN,
    DRIVE_COUNT,
};

enum {
    SENSE_BLACK,
    SENSE_RED,
    SENSE_ORANGE,
    SENSE_YELLOW,
    SENSE_BLUE,
    SENSE_PURPLE,
    SENSE_COUNT,
};

/*
 * The board is an MH-ET LIVE ESP32 MiniKit (D1 Mini form factor). All eight
 * pins come from one 2x4 block on the two inner header columns, rows 3-6.
 *
 * The assignment follows the ribbon's own colour order, so the cable splits in
 * half down the middle and each half lands on one column top to bottom, with no
 * wire crossing another:
 *
 *     inner-left            inner-right
 *       IO22  black   (1)     IO26  yellow (5)
 *       IO21  brown   (2)     IO18  green  (6)
 *       IO17  red     (3)     IO19  blue   (7)
 *       IO16  orange  (4)     IO23  purple (8)
 *
 * Which of the eight is a drive rather than a sense is purely a software
 * choice, so the two drive lines fall wherever the ribbon puts them — here row
 * 4 of each column.
 *
 * Every pin is free, output-capable and non-strapping. Only IO26 is
 * RTC-capable, so this layout gives up deep-sleep wake — deliberately, since
 * the controller is USB-powered and never sleeps.
 *
 * Pins deliberately left free: 6-11 (SPI flash), 1 and 3 (console UART), 34-39
 * and the SVP/SVN pins (input-only, no internal pull-up, and they cannot
 * drive), and 0, 2, 5, 12 and 15 (strapping — the MiniKit prints 12 and 15 as
 * TDI and TDO, which makes them look deceptively free).
 */
static const gpio_num_t k_drive_pins[DRIVE_COUNT] = {
    [DRIVE_BROWN] = GPIO_NUM_21,
    [DRIVE_GREEN] = GPIO_NUM_18,
};

static const gpio_num_t k_sense_pins[SENSE_COUNT] = {
    [SENSE_BLACK]  = GPIO_NUM_22,
    [SENSE_RED]    = GPIO_NUM_17,
    [SENSE_ORANGE] = GPIO_NUM_16,
    [SENSE_YELLOW] = GPIO_NUM_26,
    [SENSE_BLUE]   = GPIO_NUM_19,
    [SENSE_PURPLE] = GPIO_NUM_23,
};

/*
 * The keymap, indexed by intersection. Unlisted intersections stay zeroed and
 * are ignored, which covers the three unpopulated positions on the brown line.
 */
static const struct {
    uint8_t keycode;
    const char *label;
} k_keymap[DRIVE_COUNT][SENSE_COUNT] = {
    [DRIVE_BROWN] = {
        [SENSE_BLACK]  = { HID_KEY_9, "button 9" },
        [SENSE_RED]    = { HID_KEY_3, "button 3" },
        [SENSE_ORANGE] = { HID_KEY_8, "button 8" },
        /* yellow, blue and purple carry no button on the brown line */
    },
    [DRIVE_GREEN] = {
        [SENSE_BLACK]  = { HID_KEY_2, "button 2" },
        [SENSE_RED]    = { HID_KEY_4, "button 4" },
        [SENSE_ORANGE] = { HID_KEY_1, "button 1" },
        [SENSE_YELLOW] = { HID_KEY_5, "button 5" },
        [SENSE_BLUE]   = { HID_KEY_6, "button 6" },
        [SENSE_PURPLE] = { HID_KEY_7, "button 7" },
    },
};

/* Only used for log messages during bring-up. */
static const char *k_drive_names[DRIVE_COUNT] = {
    [DRIVE_BROWN] = "brown",
    [DRIVE_GREEN] = "green",
};

static const char *k_sense_names[SENSE_COUNT] = {
    [SENSE_BLACK]  = "black",
    [SENSE_RED]    = "red",
    [SENSE_ORANGE] = "orange",
    [SENSE_YELLOW] = "yellow",
    [SENSE_BLUE]   = "blue",
    [SENSE_PURPLE] = "purple",
};

static void on_key_event(int drive, int sense, bool pressed, void *arg)
{
    uint8_t keycode = k_keymap[drive][sense].keycode;
    const char *label = k_keymap[drive][sense].label;
    const char *action = pressed ? "pressed" : "released";

    if (keycode == HID_KEY_NONE) {
        /* An unpopulated intersection should never close. If one does, the loom
         * is not wired the way this table claims. */
        ESP_LOGW(TAG, "unmapped intersection %s/%s %s",
                 k_drive_names[drive], k_sense_names[sense], action);
        return;
    }

    esp_err_t err = pressed ? ble_kbd_press(keycode) : ble_kbd_release(keycode);

    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "%s (%s/%s) %s, dropped: no host connected",
                 label, k_drive_names[drive], k_sense_names[sense], action);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s (%s/%s) %s: %s", label, k_drive_names[drive],
                 k_sense_names[sense], action, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "%s (%s/%s) %s", label, k_drive_names[drive],
                 k_sense_names[sense], action);
    }
}

void app_main(void)
{
    /* NimBLE stores bonds in NVS, so this has to come first. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(ble_kbd_init());

    keymatrix_config_t matrix = {
        .drive_pins = k_drive_pins,
        .drive_count = DRIVE_COUNT,
        .sense_pins = k_sense_pins,
        .sense_count = SENSE_COUNT,
        .poll_ms = 5,
        .debounce_ms = 20,
        .settle_us = 150,
        .cb = on_key_event,
        .arg = NULL,
    };
    ESP_ERROR_CHECK(keymatrix_start(&matrix));

    ESP_LOGI(TAG, "ready — pair with \"%s\" from your host", BLE_KBD_DEVICE_NAME);
}
