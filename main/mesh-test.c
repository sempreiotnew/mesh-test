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

#define TAG "GATEWAY"

// Button
#define BUTTON_GPIO GPIO_NUM_0

// Broadcast MAC for discovery
static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// Node structure
typedef struct {
    uint8_t mac[ESP_NOW_ETH_ALEN];
    uint8_t parent[ESP_NOW_ETH_ALEN];  // MAC of the node that sent its discovery
    int layer; // distance from gateway
} node_info_t;

#define MAX_NODES 20
static node_info_t nodes[MAX_NODES];
static int node_count = 0;

// Semaphore for button press
static SemaphoreHandle_t btn_sem;

/* ================= Button ISR ================= */
static void IRAM_ATTR button_isr(void *arg)
{
    xSemaphoreGiveFromISR(btn_sem, NULL);
}

/* ================= ESP-NOW RX ================= */
static void espnow_rx_cb(const esp_now_recv_info_t *info,
                         const uint8_t *data,
                         int len)
{
    if (len <= 0) return;

    // Node announcing itself
    if (len >= 4 && strncmp((char*)data, "HERE", 4) == 0) {

        bool exists = false;
        for (int i = 0; i < node_count; i++) {
            if (memcmp(nodes[i].mac, info->src_addr, 6) == 0) {
                exists = true;
                break;
            }
        }

        if (!exists && node_count < MAX_NODES) {
            memcpy(nodes[node_count].mac, info->src_addr, 6);
            memset(nodes[node_count].parent, 0, 6); // Root = gateway
            nodes[node_count].layer = 1;            // Directly connected

            // Add as peer so we can send BLINK
            esp_now_peer_info_t peer = {0};
            memcpy(peer.peer_addr, info->src_addr, 6);
            peer.channel = 0;
            peer.ifidx = WIFI_IF_STA;
            peer.encrypt = false;

            if (esp_now_add_peer(&peer) == ESP_OK) {
                ESP_LOGI(TAG, "Node added as peer for BLINK!");
                node_count++;
            } else {
                ESP_LOGW(TAG, "Failed to add node as peer for BLINK");
            }

            ESP_LOGI(TAG, "Node added to tree: %02X:%02X:%02X:%02X:%02X:%02X",
                     info->src_addr[0], info->src_addr[1], info->src_addr[2],
                     info->src_addr[3], info->src_addr[4], info->src_addr[5]);
        }
    }
}

/* ================= Broadcast Discovery ================= */
static void discovery_task(void *arg)
{
    const char *msg = "WHOIS";

    // Add broadcast peer
    esp_now_peer_info_t broadcast_peer = {0};
    memcpy(broadcast_peer.peer_addr, broadcast_mac, ESP_NOW_ETH_ALEN);
    broadcast_peer.channel = 0;
    broadcast_peer.ifidx = WIFI_IF_STA;
    broadcast_peer.encrypt = false;

    if (esp_now_add_peer(&broadcast_peer) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to add broadcast peer (might exist already)");
    }

    while (1) {
        esp_err_t err = esp_now_send(broadcast_mac, (uint8_t *)msg, strlen(msg));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send discovery broadcast: %d", err);
        } else {
            ESP_LOGI(TAG, "BROADCAST - Sent : %s", msg);
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/* ================= Print Tree ================= */
static void print_tree_task(void *arg)
{
    while(1) {
        ESP_LOGI(TAG, "===== ESP-NOW TREE =====");
        printf("Gateway (root)\n");

        for (int i = 0; i < node_count; i++) {
            for (int j = 0; j < nodes[i].layer; j++) printf("  "); // indent by layer
            printf("|- Node: %02X:%02X:%02X:%02X:%02X:%02X\n",
                   nodes[i].mac[0], nodes[i].mac[1], nodes[i].mac[2],
                   nodes[i].mac[3], nodes[i].mac[4], nodes[i].mac[5]);
        }
        printf("=====================\n\n");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ================= Blink task (button-controlled) ================= */
static void blink_task(void *arg)
{
    while (1) {
        // Wait for button press
        if (xSemaphoreTake(btn_sem, portMAX_DELAY)) {
            if (node_count > 0) {
                for (int i = 0; i < node_count; i++) {
                    esp_err_t err = esp_now_send(nodes[i].mac, (uint8_t *)"BLINK", 5);
                    if (err == ESP_OK) {
                        ESP_LOGI(TAG, "Sent BLINK to node %02X:%02X:%02X:%02X:%02X:%02X",
                                 nodes[i].mac[0], nodes[i].mac[1], nodes[i].mac[2],
                                 nodes[i].mac[3], nodes[i].mac[4], nodes[i].mac[5]);
                    } else {
                        ESP_LOGW(TAG, "Failed to send BLINK to node %02X:%02X:%02X:%02X:%02X:%02X (%d)",
                                 nodes[i].mac[0], nodes[i].mac[1], nodes[i].mac[2],
                                 nodes[i].mac[3], nodes[i].mac[4], nodes[i].mac[5], err);
                    }
                }
            } else {
                ESP_LOGW(TAG, "No nodes in tree, cannot send BLINK");
            }
        }
    }
}

/* ================= MAIN ================= */
void app_main(void)
{
    // Button GPIO config
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,  // falling edge = pressed
    };
    gpio_config(&io_conf);

    btn_sem = xSemaphoreCreateBinary();
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);

    // Wi-Fi + ESP-NOW init
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_rx_cb));

    ESP_LOGI(TAG, "Gateway ready, broadcasting discovery messages...");

    // Tasks
    xTaskCreate(discovery_task, "discovery_task", 4096, NULL, 5, NULL);
    xTaskCreate(print_tree_task, "print_tree", 4096, NULL, 4, NULL);
    xTaskCreate(blink_task, "blink_task", 4096, NULL, 5, NULL);
}
