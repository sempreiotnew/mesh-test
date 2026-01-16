#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "esp_timer.h"

#define TAG "MESH_NODE"

// Pins
#define LED_GPIO     GPIO_NUM_2
#define BUTTON_GPIO  GPIO_NUM_0

// Discovery
static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// Peer structure
#define MAX_PEERS 20
#define PEER_TIMEOUT_MS 15000   // remove peers if silent for 15s
typedef struct {
    uint8_t mac[ESP_NOW_ETH_ALEN];
    int64_t last_seen;  // timestamp in ms
} peer_info_t;

static peer_info_t peers[MAX_PEERS];
static int peer_count = 0;
static SemaphoreHandle_t peer_mutex;

// Button semaphore
static SemaphoreHandle_t btn_sem;

// =================== UTILS ===================
static int64_t millis() {
    return (int64_t)(esp_timer_get_time() / 1000ULL);
}

// =================== BUTTON ISR ===================
static void IRAM_ATTR button_isr(void *arg) {
    xSemaphoreGiveFromISR(btn_sem, NULL);
}

// =================== PEER MANAGEMENT ===================
static void add_or_update_peer(const uint8_t *mac) {
    xSemaphoreTake(peer_mutex, portMAX_DELAY);
    bool exists = false;
    for (int i = 0; i < peer_count; i++) {
        if (memcmp(peers[i].mac, mac, 6) == 0) {
            peers[i].last_seen = millis();
            exists = true;
            break;
        }
    }
    if (!exists && peer_count < MAX_PEERS) {
        memcpy(peers[peer_count].mac, mac, 6);
        peers[peer_count].last_seen = millis();

        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, mac, 6);
        peer.channel = 0;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        esp_now_add_peer(&peer);

        peer_count++;
        ESP_LOGI(TAG, "New peer added: %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        // Blink LED on new peer
        gpio_set_level(LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(LED_GPIO, 0);
    }
    xSemaphoreGive(peer_mutex);
}

// Remove stale peers
static void cleanup_peers() {
    xSemaphoreTake(peer_mutex, portMAX_DELAY);
    int64_t now = millis();
    for (int i = 0; i < peer_count;) {
        if ((now - peers[i].last_seen) > PEER_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Removing stale peer: %02X:%02X:%02X:%02X:%02X:%02X",
                     peers[i].mac[0], peers[i].mac[1], peers[i].mac[2],
                     peers[i].mac[3], peers[i].mac[4], peers[i].mac[5]);
            esp_now_del_peer(peers[i].mac);
            // shift remaining peers
            for (int j = i; j < peer_count-1; j++) peers[j] = peers[j+1];
            peer_count--;
        } else {
            i++;
        }
    }
    xSemaphoreGive(peer_mutex);
}

// =================== ESP-NOW RX ===================
static void espnow_rx_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len <= 0) return;

    // WHOIS received -> send HERE and add peer
    if (len >= 5 && strncmp((char*)data, "WHOIS", 5) == 0) {
        add_or_update_peer(info->src_addr);

        const char *here_msg = "HERE";
        esp_now_send(info->src_addr, (uint8_t*)here_msg, strlen(here_msg));
        ESP_LOGI(TAG, "Sent HERE to peer");
    }

    // HERE received -> add peer
    if (len >= 4 && strncmp((char*)data, "HERE", 4) == 0) {
        add_or_update_peer(info->src_addr);
    }

    // BLINK received -> blink LED
    if (len >= 5 && strncmp((char*)data, "BLINK", 5) == 0) {
        ESP_LOGI(TAG, "BLINK received!");
        gpio_set_level(LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level(LED_GPIO, 0);
    }
}

// =================== DISCOVERY TASK ===================
static void discovery_task(void *arg) {
    const char *msg = "WHOIS";

    // Add broadcast peer
    esp_now_peer_info_t broadcast_peer = {0};
    memcpy(broadcast_peer.peer_addr, broadcast_mac, ESP_NOW_ETH_ALEN);
    broadcast_peer.channel = 0;
    broadcast_peer.ifidx = WIFI_IF_STA;
    broadcast_peer.encrypt = false;
    esp_now_add_peer(&broadcast_peer);

    while(1) {
        esp_now_send(broadcast_mac, (uint8_t*)msg, strlen(msg));
        cleanup_peers();  // remove stale peers regularly
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// =================== BUTTON TASK ===================
static void button_task(void *arg) {
    while(1) {
        if (xSemaphoreTake(btn_sem, portMAX_DELAY)) {
            xSemaphoreTake(peer_mutex, portMAX_DELAY);
            for (int i = 0; i < peer_count; i++) {
                esp_now_send(peers[i].mac, (uint8_t*)"BLINK", 5);
                ESP_LOGI(TAG, "Sent BLINK to %02X:%02X:%02X:%02X:%02X:%02X",
                         peers[i].mac[0], peers[i].mac[1], peers[i].mac[2],
                         peers[i].mac[3], peers[i].mac[4], peers[i].mac[5]);
            }
            xSemaphoreGive(peer_mutex);
        }
    }
}

// =================== APP MAIN ===================
void app_main(void) {
    // GPIO setup
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_GPIO, 0);

    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&btn_conf);

    // Button semaphore
    btn_sem = xSemaphoreCreateBinary();
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);

    // Peer mutex
    peer_mutex = xSemaphoreCreateMutex();

    // Wi-Fi init
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // ESP-NOW init
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_rx_cb));

    ESP_LOGI(TAG, "MESH Node ready: broadcasting WHOIS, handling peers...");

    // Tasks
    xTaskCreate(discovery_task, "discovery_task", 4096, NULL, 5, NULL);
    xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL);
}
