/*
 * homsar controller — an ESP32 that presents itself to a host as a BLE
 * keyboard and turns button presses into keystrokes.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "ble_kbd.h"
#include "buttons.h"
#include "hid_keycodes.h"

static const char *TAG = "homsar";

/*
 * The keymap. Each button shorts its GPIO to GND, so the pull-up holds the pin
 * high until it is pressed. Add rows here to grow the controller — nothing else
 * needs to change.
 *
 * GPIO 27 has no strapping or boot-time role, drives nothing during reset, and
 * is RTC-capable, so it can also serve as a deep-sleep wake source later.
 *
 * Pins to avoid when adding more: 6-11 are wired to the SPI flash, 1 and 3 are
 * the console UART, 34-39 are input-only with no internal pull-up, and 0, 2, 5,
 * 12 and 15 are strapping pins. GPIO 25, 26, 32 and 33 are the other clean
 * RTC-capable choices.
 */
static const struct {
    gpio_num_t pin;
    uint8_t keycode;
    const char *name;
} k_keymap[] = {
    { GPIO_NUM_27, HID_KEY_A, "button" },
};

#define KEYMAP_LEN (sizeof(k_keymap) / sizeof(k_keymap[0]))

static void on_button_event(int index, bool pressed, void *arg)
{
    uint8_t keycode = k_keymap[index].keycode;
    esp_err_t err = pressed ? ble_kbd_press(keycode) : ble_kbd_release(keycode);

    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "%s %s (dropped, no host connected)",
                 k_keymap[index].name, pressed ? "pressed" : "released");
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s %s: %s", k_keymap[index].name,
                 pressed ? "press" : "release", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "%s %s", k_keymap[index].name, pressed ? "pressed" : "released");
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

    gpio_num_t pins[KEYMAP_LEN];
    for (size_t i = 0; i < KEYMAP_LEN; i++) {
        pins[i] = k_keymap[i].pin;
    }

    buttons_config_t buttons = {
        .pins = pins,
        .count = KEYMAP_LEN,
        .active_low = true,
        .poll_ms = 5,
        .debounce_ms = 20,
        .cb = on_button_event,
        .arg = NULL,
    };
    ESP_ERROR_CHECK(buttons_start(&buttons));

    ESP_LOGI(TAG, "ready — pair with \"%s\" from your host", BLE_KBD_DEVICE_NAME);
}
