#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_bt.h"
#include "esp_hidd.h"
#include "esp_log.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#include "ble_kbd.h"
#include "hid_keycodes.h"

static const char *TAG = "ble_kbd";

/* Provided by NimBLE's config store; declared here because the header lives in
 * a private include directory. */
void ble_store_config_init(void);

#define HID_APPEARANCE_KEYBOARD 0x03C1
#define HID_SERVICE_UUID16      0x1812

/* Advertising payload: flags (3) + appearance (4) + 16-bit UUID list (4) leaves
 * 20 bytes, of which the name AD structure spends 2 on its header. */
#define ADV_NAME_MAX 18
_Static_assert(sizeof(BLE_KBD_DEVICE_NAME) - 1 <= ADV_NAME_MAX,
               "BLE_KBD_DEVICE_NAME does not fit in the advertising payload");

/*
 * Report layout, matching the descriptor below:
 *   [0]    modifier bitmap (bit N == usage 0xE0 + N)
 *   [1]    reserved, always zero
 *   [2..7] up to six held key usages
 * The host pushes a 1-byte LED report back on the same report ID.
 */
#define KBD_REPORT_ID   1
#define KBD_REPORT_LEN  (2 + BLE_KBD_MAX_KEYS)
#define KBD_KEYS_OFFSET 2

static const uint8_t s_report_map[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop)         */
    0x09, 0x06,        /* Usage (Keyboard)                     */
    0xA1, 0x01,        /* Collection (Application)             */
    0x85, KBD_REPORT_ID, /*   Report ID                        */

    0x05, 0x07,        /*   Usage Page (Keyboard/Keypad)       */
    0x19, 0xE0,        /*   Usage Minimum (Left Control)       */
    0x29, 0xE7,        /*   Usage Maximum (Right GUI)          */
    0x15, 0x00,        /*   Logical Minimum (0)                */
    0x25, 0x01,        /*   Logical Maximum (1)                */
    0x75, 0x01,        /*   Report Size (1)                    */
    0x95, 0x08,        /*   Report Count (8)                   */
    0x81, 0x02,        /*   Input (Data,Var,Abs)  -> modifiers */

    0x95, 0x01,        /*   Report Count (1)                   */
    0x75, 0x08,        /*   Report Size (8)                    */
    0x81, 0x03,        /*   Input (Const,Var,Abs) -> reserved  */

    0x95, 0x05,        /*   Report Count (5)                   */
    0x75, 0x01,        /*   Report Size (1)                    */
    0x05, 0x08,        /*   Usage Page (LEDs)                  */
    0x19, 0x01,        /*   Usage Minimum (Num Lock)           */
    0x29, 0x05,        /*   Usage Maximum (Kana)               */
    0x91, 0x02,        /*   Output (Data,Var,Abs) -> LEDs      */
    0x95, 0x01,        /*   Report Count (1)                   */
    0x75, 0x03,        /*   Report Size (3)                    */
    0x91, 0x03,        /*   Output (Const,Var,Abs) -> padding  */

    0x95, BLE_KBD_MAX_KEYS, /*   Report Count (6)              */
    0x75, 0x08,        /*   Report Size (8)                    */
    0x15, 0x00,        /*   Logical Minimum (0)                */
    0x26, 0xE7, 0x00,  /*   Logical Maximum (231)              */
    0x05, 0x07,        /*   Usage Page (Keyboard/Keypad)       */
    0x19, 0x00,        /*   Usage Minimum (0)                  */
    0x29, 0xE7,        /*   Usage Maximum (231)                */
    0x81, 0x00,        /*   Input (Data,Array,Abs) -> keys     */

    0xC0,              /* End Collection                       */
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    { .data = s_report_map, .len = sizeof(s_report_map) },
};

static esp_hid_device_config_t s_hid_config = {
    /* Shared VID/PID from Objective Development's free pool — fine for a
     * one-off device, not for anything you intend to distribute. */
    .vendor_id         = 0x16C0,
    .product_id        = 0x05DF,
    .version           = 0x0100,
    .device_name       = BLE_KBD_DEVICE_NAME,
    .manufacturer_name = "homsar",
    .serial_number     = "000001",
    .report_maps       = s_report_maps,
    .report_maps_len   = 1,
};

static esp_hidd_dev_t *s_hid_dev;
static SemaphoreHandle_t s_lock;
static uint8_t s_report[KBD_REPORT_LEN];
static uint8_t s_led_state;
static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;

static int gap_event(struct ble_gap_event *event, void *arg);

static void advertise(void)
{
    static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(HID_SERVICE_UUID16);
    struct ble_hs_adv_fields fields = { 0 };
    struct ble_gap_adv_params adv_params = { 0 };
    int rc;

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = HID_APPEARANCE_KEYBOARD;
    fields.appearance_is_present = 1;
    fields.uuids16 = (ble_uuid16_t *)&hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.name = (uint8_t *)BLE_KBD_DEVICE_NAME;
    fields.name_len = sizeof(BLE_KBD_DEVICE_NAME) - 1;
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(50);

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as \"%s\"", BLE_KBD_DEVICE_NAME);
}

/*
 * Connection-level GAP callback. esp_hid registers its own global listener for
 * the HID bookkeeping, so this one only owns the advertising lifecycle and
 * pairing edge cases.
 */
static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connection failed: %d", event->connect.status);
            advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected, reason 0x%04x", event->disconnect.reason);
        advertise();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change, status %d", event->enc_change.status);
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* The host is pairing again with a peer we still hold a bond for —
         * typically because it forgot us. Drop the stale bond and let the new
         * pairing proceed, otherwise the host can never reconnect. */
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) != 0) {
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    default:
        break;
    }

    return 0;
}

static void hidd_event_handler(void *args, esp_event_base_t base, int32_t id, void *data)
{
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)data;

    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT:
        /* NimBLE host has synced with the controller; the identity address is
         * only valid from here on. */
        if (ble_hs_util_ensure_addr(0) != 0 ||
            ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
            ESP_LOGW(TAG, "could not infer own address type, falling back to public");
            s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
        }
        advertise();
        break;

    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "host connected");
        break;

    case ESP_HIDD_DISCONNECT_EVENT:
        /* Drop any keys that were down when the link went away, so the next
         * host does not inherit a stuck key. */
        ble_kbd_release_all();
        break;

    case ESP_HIDD_OUTPUT_EVENT:
        if (param->output.report_id == KBD_REPORT_ID && param->output.length >= 1) {
            s_led_state = param->output.data[0];
            ESP_LOGD(TAG, "LED state 0x%02x", s_led_state);
        }
        break;

    case ESP_HIDD_PROTOCOL_MODE_EVENT:
        ESP_LOGI(TAG, "protocol mode: %s",
                 param->protocol_mode.protocol_mode ? "report" : "boot");
        break;

    default:
        break;
    }
}

static void nimble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();  /* returns only once nimble_port_stop() is called */
    nimble_port_freertos_deinit();
}

esp_err_t ble_kbd_init(void)
{
    esp_err_t err;
    int rc;

    if (s_hid_dev != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* BLE only — hand the Classic Bluetooth controller memory back to the heap. */
    err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_mem_release failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#if CONFIG_IDF_TARGET_ESP32
    bt_cfg.mode = ESP_BT_MODE_BLE;
#endif
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_enable failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_nimble_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_nimble_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Just Works pairing: the controller has no display or keypad of its own,
     * so it cannot take part in a passkey exchange. Bonding is on so a paired
     * host reconnects without user interaction. Switch sm_io_cap to
     * BLE_SM_IO_CAP_DISP_ONLY and sm_mitm to 1 if you add a display and want
     * MITM protection. */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    /* Registers the HID, battery, device-information and GAP services and
     * installs NimBLE's sync callback, so it has to run before we touch the
     * GAP service or start the host task. */
    err = esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE, hidd_event_handler, &s_hid_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_dev_init failed: %s", esp_err_to_name(err));
        return err;
    }

    rc = ble_svc_gap_device_name_set(BLE_KBD_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_svc_gap_device_name_set failed: %d", rc);
    }

    err = esp_hidd_dev_battery_set(s_hid_dev, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_hidd_dev_battery_set failed: %s", esp_err_to_name(err));
    }

    /* Persist bonds in NVS so pairing survives a reboot. */
    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    nimble_port_freertos_init(nimble_host_task);
    return ESP_OK;
}

bool ble_kbd_connected(void)
{
    return s_hid_dev != NULL && esp_hidd_dev_connected(s_hid_dev);
}

/* Caller must hold s_lock. */
static esp_err_t send_report(void)
{
    if (!ble_kbd_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_hidd_dev_input_set(s_hid_dev, 0, KBD_REPORT_ID, s_report, KBD_REPORT_LEN);
}

static bool is_modifier(uint8_t keycode)
{
    return keycode >= HID_KEY_LEFT_CTRL && keycode <= HID_KEY_RIGHT_GUI;
}

esp_err_t ble_kbd_press(uint8_t keycode)
{
    esp_err_t err;

    if (keycode == HID_KEY_NONE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (is_modifier(keycode)) {
        s_report[0] |= (uint8_t)(1u << (keycode - HID_KEY_LEFT_CTRL));
    } else {
        int free_slot = -1;
        for (int i = 0; i < BLE_KBD_MAX_KEYS; i++) {
            if (s_report[KBD_KEYS_OFFSET + i] == keycode) {
                xSemaphoreGive(s_lock);
                return ESP_OK;  /* already held */
            }
            if (free_slot < 0 && s_report[KBD_KEYS_OFFSET + i] == HID_KEY_NONE) {
                free_slot = i;
            }
        }
        if (free_slot < 0) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_NO_MEM;  /* more than BLE_KBD_MAX_KEYS keys down */
        }
        s_report[KBD_KEYS_OFFSET + free_slot] = keycode;
    }

    err = send_report();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t ble_kbd_release(uint8_t keycode)
{
    esp_err_t err;

    if (keycode == HID_KEY_NONE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (is_modifier(keycode)) {
        s_report[0] &= (uint8_t)~(1u << (keycode - HID_KEY_LEFT_CTRL));
    } else {
        for (int i = 0; i < BLE_KBD_MAX_KEYS; i++) {
            if (s_report[KBD_KEYS_OFFSET + i] == keycode) {
                s_report[KBD_KEYS_OFFSET + i] = HID_KEY_NONE;
                break;
            }
        }
    }

    err = send_report();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t ble_kbd_release_all(void)
{
    esp_err_t err;

    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_report, 0, sizeof(s_report));
    err = send_report();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t ble_kbd_tap(uint8_t keycode, uint32_t hold_ms)
{
    esp_err_t err = ble_kbd_press(keycode);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    return ble_kbd_release(keycode);
}

uint8_t ble_kbd_led_state(void)
{
    return s_led_state;
}
