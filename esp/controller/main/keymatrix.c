#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_rom_sys.h"

#include "keymatrix.h"

static const char *TAG = "keymatrix";

#define KEYMATRIX_TASK_STACK 3072
#define KEYMATRIX_TASK_PRIO  5

typedef struct {
    gpio_num_t *drive_pins;
    size_t drive_count;
    gpio_num_t *sense_pins;
    size_t sense_count;
    bool *pressed;       /*!< Debounced state, drive-major. */
    uint16_t *disagree;  /*!< Consecutive readings differing from `pressed`. */
    TickType_t period;
    uint32_t settle_us;
    uint16_t debounce_samples;
    keymatrix_event_cb_t cb;
    void *arg;
} keymatrix_ctx_t;

/* The drive pins keep a latched output level of 0 from startup, so selecting a
 * line is just a direction flip: output (driving low) or input (high-Z). */
static inline void drive_select(keymatrix_ctx_t *ctx, size_t d)
{
    gpio_set_direction(ctx->drive_pins[d], GPIO_MODE_OUTPUT);
    esp_rom_delay_us(ctx->settle_us);
}

static inline void drive_release(keymatrix_ctx_t *ctx, size_t d)
{
    gpio_set_direction(ctx->drive_pins[d], GPIO_MODE_INPUT);
}

/* One full pass over the matrix. With `report` false the debounced state is
 * simply overwritten, which is how the initial state is seeded without firing
 * events for buttons that happen to be held at startup. */
static void scan(keymatrix_ctx_t *ctx, bool report)
{
    for (size_t d = 0; d < ctx->drive_count; d++) {
        drive_select(ctx, d);

        for (size_t s = 0; s < ctx->sense_count; s++) {
            size_t i = d * ctx->sense_count + s;
            bool raw = gpio_get_level(ctx->sense_pins[s]) == 0;

            if (!report) {
                ctx->pressed[i] = raw;
            } else if (raw == ctx->pressed[i]) {
                ctx->disagree[i] = 0;
            } else if (++ctx->disagree[i] >= ctx->debounce_samples) {
                ctx->pressed[i] = raw;
                ctx->disagree[i] = 0;
                ctx->cb((int)d, (int)s, raw, ctx->arg);
            }
        }

        drive_release(ctx, d);
    }
}

static void keymatrix_task(void *arg)
{
    keymatrix_ctx_t *ctx = (keymatrix_ctx_t *)arg;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        scan(ctx, true);
        xTaskDelayUntil(&last_wake, ctx->period);
    }
}

esp_err_t keymatrix_start(const keymatrix_config_t *config)
{
    if (config == NULL || config->drive_pins == NULL || config->sense_pins == NULL ||
        config->drive_count == 0 || config->sense_count == 0 ||
        config->cb == NULL || config->poll_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t drive_mask = 0;
    for (size_t i = 0; i < config->drive_count; i++) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(config->drive_pins[i])) {
            ESP_LOGE(TAG, "GPIO %d cannot drive an output", config->drive_pins[i]);
            return ESP_ERR_INVALID_ARG;
        }
        drive_mask |= 1ULL << config->drive_pins[i];
    }

    uint64_t sense_mask = 0;
    for (size_t i = 0; i < config->sense_count; i++) {
        if (!GPIO_IS_VALID_GPIO(config->sense_pins[i])) {
            ESP_LOGE(TAG, "GPIO %d is not a valid pin", config->sense_pins[i]);
            return ESP_ERR_INVALID_ARG;
        }
        sense_mask |= 1ULL << config->sense_pins[i];
    }

    if (drive_mask & sense_mask) {
        ESP_LOGE(TAG, "a pin is listed as both a drive and a sense line");
        return ESP_ERR_INVALID_ARG;
    }

    /* Drive lines start high-impedance. No pulls: a floating drive line must not
     * fight the sense pull-ups through a closed button. */
    gpio_config_t drive_conf = {
        .pin_bit_mask = drive_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&drive_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config (drive) failed: %s", esp_err_to_name(err));
        return err;
    }

    gpio_config_t sense_conf = {
        .pin_bit_mask = sense_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&sense_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config (sense) failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Latch the output level now so that scanning only ever flips direction and
     * a drive line can never briefly present a high. */
    for (size_t i = 0; i < config->drive_count; i++) {
        gpio_set_level(config->drive_pins[i], 0);
    }

    size_t cells = config->drive_count * config->sense_count;
    keymatrix_ctx_t *ctx = calloc(1, sizeof(keymatrix_ctx_t));
    gpio_num_t *drives = calloc(config->drive_count, sizeof(gpio_num_t));
    gpio_num_t *senses = calloc(config->sense_count, sizeof(gpio_num_t));
    bool *pressed = calloc(cells, sizeof(bool));
    uint16_t *disagree = calloc(cells, sizeof(uint16_t));
    if (ctx == NULL || drives == NULL || senses == NULL || pressed == NULL || disagree == NULL) {
        free(ctx);
        free(drives);
        free(senses);
        free(pressed);
        free(disagree);
        return ESP_ERR_NO_MEM;
    }

    memcpy(drives, config->drive_pins, config->drive_count * sizeof(gpio_num_t));
    memcpy(senses, config->sense_pins, config->sense_count * sizeof(gpio_num_t));

    uint32_t samples = config->debounce_ms / config->poll_ms;
    ctx->drive_pins = drives;
    ctx->drive_count = config->drive_count;
    ctx->sense_pins = senses;
    ctx->sense_count = config->sense_count;
    ctx->pressed = pressed;
    ctx->disagree = disagree;
    ctx->period = pdMS_TO_TICKS(config->poll_ms);
    ctx->settle_us = config->settle_us;
    ctx->debounce_samples = samples < 1 ? 1 : (uint16_t)samples;
    ctx->cb = config->cb;
    ctx->arg = config->arg;

    if (ctx->period == 0) {
        ctx->period = 1;  /* poll_ms rounded below one tick */
    }

    /* Seed from the current state so a button already held at boot does not
     * fire a spurious press once the task starts. */
    scan(ctx, false);

    if (xTaskCreate(keymatrix_task, "keymatrix", KEYMATRIX_TASK_STACK, ctx,
                    KEYMATRIX_TASK_PRIO, NULL) != pdPASS) {
        free(ctx);
        free(drives);
        free(senses);
        free(pressed);
        free(disagree);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "scanning %ux%u matrix, %" PRIu32 " ms period, %u sample debounce",
             (unsigned)ctx->drive_count, (unsigned)ctx->sense_count,
             config->poll_ms, ctx->debounce_samples);
    return ESP_OK;
}
