#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "buttons.h"

static const char *TAG = "buttons";

#define BUTTONS_TASK_STACK 3072
#define BUTTONS_TASK_PRIO  5

typedef struct {
    gpio_num_t pin;
    bool pressed;      /*!< Debounced state. */
    uint16_t disagree; /*!< Consecutive samples that differ from `pressed`. */
} button_state_t;

typedef struct {
    button_state_t *buttons;
    size_t count;
    bool active_low;
    TickType_t period;
    uint16_t debounce_samples;
    button_event_cb_t cb;
    void *arg;
} buttons_ctx_t;

static void buttons_task(void *arg)
{
    buttons_ctx_t *ctx = (buttons_ctx_t *)arg;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        for (size_t i = 0; i < ctx->count; i++) {
            button_state_t *b = &ctx->buttons[i];
            int level = gpio_get_level(b->pin);
            bool raw = ctx->active_low ? (level == 0) : (level != 0);

            if (raw == b->pressed) {
                b->disagree = 0;
            } else if (++b->disagree >= ctx->debounce_samples) {
                b->pressed = raw;
                b->disagree = 0;
                ctx->cb((int)i, raw, ctx->arg);
            }
        }
        xTaskDelayUntil(&last_wake, ctx->period);
    }
}

esp_err_t buttons_start(const buttons_config_t *config)
{
    if (config == NULL || config->pins == NULL || config->count == 0 ||
        config->cb == NULL || config->poll_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t pin_mask = 0;
    for (size_t i = 0; i < config->count; i++) {
        if (!GPIO_IS_VALID_GPIO(config->pins[i])) {
            ESP_LOGE(TAG, "GPIO %d is not a valid pin", config->pins[i]);
            return ESP_ERR_INVALID_ARG;
        }
        pin_mask |= 1ULL << config->pins[i];
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = config->active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = config->active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    buttons_ctx_t *ctx = calloc(1, sizeof(buttons_ctx_t));
    button_state_t *states = calloc(config->count, sizeof(button_state_t));
    if (ctx == NULL || states == NULL) {
        free(ctx);
        free(states);
        return ESP_ERR_NO_MEM;
    }

    /* Seed from the current level so a button already held at boot does not
     * fire a spurious press once the task starts. */
    for (size_t i = 0; i < config->count; i++) {
        int level = gpio_get_level(config->pins[i]);
        states[i].pin = config->pins[i];
        states[i].pressed = config->active_low ? (level == 0) : (level != 0);
    }

    uint32_t samples = config->debounce_ms / config->poll_ms;
    ctx->buttons = states;
    ctx->count = config->count;
    ctx->active_low = config->active_low;
    ctx->period = pdMS_TO_TICKS(config->poll_ms);
    ctx->debounce_samples = samples < 1 ? 1 : (uint16_t)samples;
    ctx->cb = config->cb;
    ctx->arg = config->arg;

    if (ctx->period == 0) {
        ctx->period = 1;  /* poll_ms rounded below one tick */
    }

    if (xTaskCreate(buttons_task, "buttons", BUTTONS_TASK_STACK, ctx, BUTTONS_TASK_PRIO, NULL) != pdPASS) {
        free(states);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "watching %u buttons, %" PRIu32 " ms poll, %u sample debounce",
             (unsigned)ctx->count, config->poll_ms, ctx->debounce_samples);
    return ESP_OK;
}
