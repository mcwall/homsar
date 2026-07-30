/*
 * Scanned, debounced button matrix.
 *
 * Buttons sit at the intersections of a set of drive lines and a set of sense
 * lines. A background task walks the drive lines one at a time, pulling each
 * low and reading the sense lines, so N x M buttons need only N + M pins.
 *
 * Only the line currently being scanned is driven; the rest are left
 * high-impedance. Without that, two buttons sharing a sense line would short a
 * driven-high line straight to a driven-low one.
 *
 * A state change is reported only once the new reading has held steady for the
 * debounce window, which filters contact bounce without external components.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Called from the scan task on each debounced state change.
 *
 * @param drive   Index into the drive pin array.
 * @param sense   Index into the sense pin array.
 * @param pressed True on press, false on release.
 * @param arg     Opaque pointer from keymatrix_config_t.
 */
typedef void (*keymatrix_event_cb_t)(int drive, int sense, bool pressed, void *arg);

typedef struct {
    const gpio_num_t *drive_pins; /*!< Driven low one at a time, high-impedance
                                       otherwise. Copied; need not outlive the
                                       call. Must be output-capable, which rules
                                       out GPIO 34-39 on the ESP32. */
    size_t drive_count;
    const gpio_num_t *sense_pins; /*!< Read as inputs with internal pull-ups, so
                                       these also cannot be GPIO 34-39. */
    size_t sense_count;
    uint32_t poll_ms;             /*!< Period of a full scan, e.g. 5. */
    uint32_t debounce_ms;         /*!< How long a new reading must hold, e.g. 20. */
    uint32_t settle_us;           /*!< Delay after driving a line before reading
                                       the sense pins. The internal pull-ups are
                                       weak (~45 kOhm), so a long harness needs
                                       time to charge; 150 is a safe default. */
    keymatrix_event_cb_t cb;      /*!< Event callback, required. */
    void *arg;                    /*!< Passed through to the callback. */
} keymatrix_config_t;

/**
 * @brief Configure the pins and start the scan task.
 *
 * The callback runs on that task, so it may block, but doing so delays the rest
 * of the scan.
 */
esp_err_t keymatrix_start(const keymatrix_config_t *config);

#ifdef __cplusplus
}
#endif
