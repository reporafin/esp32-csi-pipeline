#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"

// CONFIGURATION
// Must match exactly what the Receiver AP is broadcasting
#define AP_SSID       "CSI_SENSOR_AP"
#define AP_PASSWORD   "csi12345"

// UDP target: Receiver's default AP IP is always 192.168.4.1
#define RX_IP_ADDRESS "192.168.4.1"
#define UDP_PORT      3333

// Beacon payload
#define BEACON_MSG    "CSI_PING"
#define BEACON_RATE_HZ  100         
#define BEACON_DELAY_MS (1000 / BEACON_RATE_HZ)  

// Retry loop
#define RETRY_DELAY_MS  3000          

static const char *TAG = "CSI_TX";

// Event group to signal Wi-Fi connection
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int retry_count = 0;

// SECTION 1 — WI-FI EVENT HANDLER

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // ADD THIS DELAY: Let the antenna physically wake up and stabilize
        vTaskDelay(pdMS_TO_TICKS(2000)); 
        
        esp_wifi_connect();
        ESP_LOGI(TAG, "Scanning for AP: %s ...", AP_SSID);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // Do NOT restart the board. The Rx may still be doing its cognitive
        // channel scan (up to ~15 sec). Just wait and retry patiently.
        retry_count++;
        ESP_LOGW(TAG, "Rx AP not found yet. Attempt %d — waiting %d ms before retry...",
                 retry_count, RETRY_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
        esp_wifi_connect();   // Try again — no limit, no reboot
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// SECTION 2 — CONNECT TO RECEIVER AP
// Uses MAC-locking via SSID match (the Rx broadcasts a unique SSID).

static void connect_to_rx_ap(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // Register both WIFI and IP events
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &wifi_event_handler,
                                               NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler,
                                               NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Lock to specific SSID (MAC-locking via SSID name)
    wifi_config_t sta_config = {};
    strncpy((char*)sta_config.sta.ssid,     AP_SSID,     sizeof(sta_config.sta.ssid));
    strncpy((char*)sta_config.sta.password, AP_PASSWORD, sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Waiting for connection to %s...", AP_SSID);


    ESP_LOGI(TAG, "Waiting for Rx AP '%s' to appear (Rx may still be scanning)...", AP_SSID);
    xEventGroupWaitBits(wifi_event_group,
                        WIFI_CONNECTED_BIT,   // Only unblock on success
                        pdFALSE,
                        pdTRUE,
                        portMAX_DELAY);       // No timeout — wait forever if needed
    ESP_LOGI(TAG, "Connected to AP: %s", AP_SSID);
}

// SECTION 3 — UDP BEACON TASK

static void udp_beacon_task(void* pvParameters)
{
    // Create UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    // Configure destination address (Rx AP always gets 192.168.4.1 by default)
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(RX_IP_ADDRESS);
    dest_addr.sin_family      = AF_INET;
    dest_addr.sin_port        = htons(UDP_PORT);

    ESP_LOGI(TAG, "UDP beacon started → %s:%d @ %d Hz",
             RX_IP_ADDRESS, UDP_PORT, BEACON_RATE_HZ);

    uint32_t beacon_count = 0;

    while (1) {
        // Fire beacon packet
        int err = sendto(sock,
                         BEACON_MSG,
                         strlen(BEACON_MSG),
                         0,
                         (struct sockaddr*)&dest_addr,
                         sizeof(dest_addr));

        if (err < 0) {
            ESP_LOGW(TAG, "Send failed: errno %d. Retrying...", errno);
        }

        beacon_count++;

        // Log every 100 packets (every 1 second)
        if (beacon_count % 100 == 0) {
            ESP_LOGI(TAG, "Beacons sent: %lu", (unsigned long)beacon_count);
        }

        // Exactly 10ms delay → 100 Hz
        vTaskDelay(pdMS_TO_TICKS(BEACON_DELAY_MS));
    }

    // Should never reach here, but clean up if we do
    shutdown(sock, 0);
    close(sock);
    vTaskDelete(NULL);
}

// SECTION 4 — APP MAIN

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "=== CSI TRANSMITTER BOOTING ===");
    ESP_LOGI(TAG, "Target SSID: %s | Rate: %d Hz", AP_SSID, BEACON_RATE_HZ);

    // Connect to Rx AP
    connect_to_rx_ap();

    // Start UDP beacon task (high priority, pinned to core 1)
    xTaskCreatePinnedToCore(udp_beacon_task,
                            "udp_beacon",
                            4096,      // Stack size (bytes)
                            NULL,
                            5,         // Priority (5 = high)
                            NULL,
                            1);        // Core 1

    ESP_LOGI(TAG, "=== TX READY — Firing UDP beacons ===");
}
