/*
 * Polled, debounced GPIO buttons.
 *
 * A background task samples every configured pin on a fixed period and reports
 * a state change only once the new level has held steady for the debounce
 * window, which filters contact bounce without any external RC network.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Called from the button task whenever a debounced state change occurs.
 *
 * @param index   Position of the button in the configured pin array.
 * @param pressed True on press, false on release.
 * @param arg     Opaque pointer from buttons_config_t.
 */
typedef void (*button_event_cb_t)(int index, bool pressed, void *arg);

typedef struct {
    const gpio_num_t *pins;  /*!< Pins to watch; copied, need not outlive the call. */
    size_t count;            /*!< Number of pins. */
    bool active_low;         /*!< True: button shorts the pin to GND and the
                                  internal pull-up is enabled. False: button
                                  drives the pin high and the internal pull-down
                                  is enabled. */
    uint32_t poll_ms;        /*!< Sampling period, e.g. 5. */
    uint32_t debounce_ms;    /*!< How long a new level must hold, e.g. 20. */
    button_event_cb_t cb;    /*!< Event callback, required. */
    void *arg;               /*!< Passed through to the callback. */
} buttons_config_t;

/**
 * @brief Configure the pins and start the sampling task.
 *
 * The callback runs on that task, so it may block, but doing so delays every
 * other button.
 *
 * Note that GPIO 34-39 on the ESP32 are input-only and have no internal pull
 * resistors; those pins need an external one.
 */
esp_err_t buttons_start(const buttons_config_t *config);

#ifdef __cplusplus
}
#endif
