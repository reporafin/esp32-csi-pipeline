#define USE_QUIET_CHANNEL true

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_mac.h"       
#include "esp_netif.h"     
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

// CONFIGURATION
#define AP_SSID          "CSI_SENSOR_AP"
#define AP_PASSWORD      "csi12345"
#define AP_MAX_CONN      1

#define LED_SCANNING     GPIO_NUM_18
#define LED_AP_READY     GPIO_NUM_19
#define LED_TX_LINKED    GPIO_NUM_21

#define UART_BAUD        460800
#define MAX_CHANNELS     13
#define SCAN_AP_MAX      20
#define MAX_CSI_SUBCARRIERS 64

static const char *TAG = "CSI_RX";

// Data structure to safely pass raw CSI from ISR to processing task
typedef struct {
    uint32_t packet_count;
    uint8_t  subcarrier_count;
    int8_t   raw_data[MAX_CSI_SUBCARRIERS * 2]; // csi value
} csi_data_t;

static QueueHandle_t csi_queue;
static uint32_t packet_count = 0;
static uint8_t  selected_channel = 6;
static int      ap_count_per_channel[MAX_CHANNELS + 1] = {0};

static EventGroupHandle_t wifi_event_group;
#define STA_CONNECTED_BIT BIT0

// SECTION 1 — LED UTILITIES (optional)

static void led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_SCANNING) | (1ULL << LED_AP_READY) | (1ULL << LED_TX_LINKED),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_SCANNING,  0);
    gpio_set_level(LED_AP_READY,  0);
    gpio_set_level(LED_TX_LINKED, 0);
}

// SECTION 2 — Wi-Fi EVENT HANDLER

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Tx connected. MAC: " MACSTR ", AID: %d", MAC2STR(event->mac), event->aid);
        gpio_set_level(LED_TX_LINKED, 1);
        xEventGroupSetBits(wifi_event_group, STA_CONNECTED_BIT);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGW(TAG, "Tx disconnected. Waiting for reconnect...");
        gpio_set_level(LED_TX_LINKED, 0);
        xEventGroupClearBits(wifi_event_group, STA_CONNECTED_BIT);
    }
}

// SECTION 3 — COGNITIVE CHANNEL SCAN

static uint8_t cognitive_channel_scan(void)
{
    ESP_LOGI(TAG, "=== COGNITIVE CHANNEL SCAN STARTED ===");
    gpio_set_level(LED_SCANNING, 1);

    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = true;
    scan_config.scan_type   = WIFI_SCAN_TYPE_ACTIVE;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // CRITICAL FIX: Allow baseband to settle before triggering active scan
    vTaskDelay(pdMS_TO_TICKS(150));

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s. Defaulting to channel 1.", esp_err_to_name(err));
        gpio_set_level(LED_SCANNING, 0);
        return 1;
    }

    uint16_t ap_count = SCAN_AP_MAX;
    wifi_ap_record_t ap_records[SCAN_AP_MAX];
    memset(ap_records, 0, sizeof(ap_records));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_records));

    memset(ap_count_per_channel, 0, sizeof(ap_count_per_channel));
    for (int i = 0; i < ap_count; i++) {
        uint8_t ch = ap_records[i].primary;
        if (ch >= 1 && ch <= MAX_CHANNELS) {
            ap_count_per_channel[ch]++;
        }
    }

    uint8_t quietest_ch = 1, noisiest_ch = 1;
    int min_count = 9999, max_count = -1;

    for (int ch = 1; ch <= MAX_CHANNELS; ch++) {
        if (ap_count_per_channel[ch] < min_count) {
            min_count = ap_count_per_channel[ch];
            quietest_ch = ch;
        }
        if (ap_count_per_channel[ch] > max_count) {
            max_count = ap_count_per_channel[ch];
            noisiest_ch = ch;
        }
    }

    // Stop Wi-Fi to prepare for Access Point mode
    ESP_ERROR_CHECK(esp_wifi_stop());
    
    // CRITICAL FIX: Allow hardware to settle before switching modes
    vTaskDelay(pdMS_TO_TICKS(150));
    
    gpio_set_level(LED_SCANNING, 0);

#if USE_QUIET_CHANNEL
    ESP_LOGI(TAG, "Using QUIET channel: %d (%d APs)", quietest_ch, min_count);
    return quietest_ch;
#else
    ESP_LOGI(TAG, "Using NOISY channel: %d (%d APs)", noisiest_ch, max_count);
    return noisiest_ch;
#endif
}

// SECTION 4 — AP START & UART STREAMING TASK

static void start_wifi_ap(uint8_t channel)
{
    wifi_config_t ap_config = {};
    strncpy((char*)ap_config.ap.ssid, AP_SSID, sizeof(ap_config.ap.ssid));
    strncpy((char*)ap_config.ap.password, AP_PASSWORD, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(AP_SSID);
    ap_config.ap.channel = channel;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.max_connection = AP_MAX_CONN;
    ap_config.ap.beacon_interval = 100;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started. SSID: %s | Channel: %d", AP_SSID, channel);
    gpio_set_level(LED_AP_READY, 1);
}

// Background task to process math and UART printing safely
static void csi_print_task(void *pvParameters)
{
    csi_data_t csi;
    while(1) {
        if (xQueueReceive(csi_queue, &csi, portMAX_DELAY)) {
            printf("CSI_DATA,%lu", (unsigned long)csi.packet_count);
            for (int i = 0; i < csi.subcarrier_count; i++) {
                int8_t imag = csi.raw_data[2 * i];
                int8_t real = csi.raw_data[2 * i + 1];
                int amplitude = (int)sqrtf((float)(real * real + imag * imag));
                printf(",%d", amplitude);
            }
            printf("\n");
        }
    }
}

// SECTION 5 — CSI CALLBACK

static void IRAM_ATTR csi_callback(void* ctx, wifi_csi_info_t* data)
{
    if (data == NULL || data->buf == NULL || csi_queue == NULL) return;

    int num_subcarriers = data->len / 2;
    if (num_subcarriers > MAX_CSI_SUBCARRIERS) {
        num_subcarriers = MAX_CSI_SUBCARRIERS;
    }

    csi_data_t csi_item;
    csi_item.packet_count = packet_count++;
    csi_item.subcarrier_count = num_subcarriers;
    
    // Copy raw data and defer processing to FreeRTOS task
    memcpy(csi_item.raw_data, data->buf, num_subcarriers * 2);

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(csi_queue, &csi_item, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

static void enable_csi(void)
{
    // configuration for modern ESP-IDF compatibility
    wifi_csi_config_t csi_config = {
        .lltf_en           = true,
        .htltf_en          = true,
        .stbc_htltf2_en    = true,   
        .ltf_merge_en      = true,
        .channel_filter_en = false,
        .manu_scale        = false,
        .shift             = false   
    };

    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(&csi_callback, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
}

// SECTION 6 — APP MAIN

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    led_init();
    uart_set_baudrate(UART_NUM_0, UART_BAUD);
    // Initialize CSI queue and start dedicated print task to protect the ISR
    csi_queue = xQueueCreate(100, sizeof(csi_data_t));
    xTaskCreatePinnedToCore(csi_print_task, "csi_print", 4096, NULL, 5, NULL, 1);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    selected_channel = cognitive_channel_scan();
    start_wifi_ap(selected_channel);
    enable_csi();

    ESP_LOGI(TAG, "=== SYSTEM READY ===");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "Packets captured: %lu | Channel: %d", (unsigned long)packet_count, selected_channel);
    }
}