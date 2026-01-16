#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_mesh.h"
#include "driver/gpio.h"

#define TAG "MESH_NODE"

#define MESH_CHANNEL 6
#define LED_GPIO     GPIO_NUM_2
#define BUTTON_GPIO  GPIO_NUM_0

static bool is_root = false;

/* ================= ESP-NOW ================= */
static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static void espnow_rx_cb(const esp_now_recv_info_t *info,
                         const uint8_t *data,
                         int len)
{
    if (len == 5 && memcmp(data, "BLINK", 5) == 0) {
        ESP_LOGI(TAG,
            "from: %02X:%02X:%02X:%02X:%02X:%02X -> to: %02X:%02X:%02X:%02X:%02X:%02X",
            info->src_addr[0], info->src_addr[1], info->src_addr[2],
            info->src_addr[3], info->src_addr[4], info->src_addr[5],
            info->des_addr[0], info->des_addr[1], info->des_addr[2],
            info->des_addr[3], info->des_addr[4], info->des_addr[5]
        );

        gpio_set_level(LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level(LED_GPIO, 0);
    }
}

/* ================= ESP-NOW TX ================= */
static void espnow_tx_task(void *arg)
{
    const char *msg = "BLINK";
    while (1) {
        if (is_root) {
            esp_now_send(broadcast_mac, (uint8_t *)msg, strlen(msg));
            ESP_LOGI(TAG, "Broadcast BLINK");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ================= Mesh EVENT ================= */
static void mesh_event_handler(void *arg,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    if (base != MESH_EVENT) return;

    switch (event_id) {
        case MESH_EVENT_STARTED:
            ESP_LOGI(TAG, "Mesh started");
            break;

        case MESH_EVENT_PARENT_CONNECTED: {
            mesh_event_connected_t *conn = (mesh_event_connected_t *)event_data;
            ESP_LOGI(TAG, "Parent connected, layer=%d", conn->self_layer);
            break;
        }

        case MESH_EVENT_ROOT_ADDRESS:
            ESP_LOGW(TAG, "I AM ROOT");
            is_root = true;
            break;

        case MESH_EVENT_PARENT_DISCONNECTED:
            ESP_LOGW(TAG, "Parent disconnected");
            is_root = false;
            break;

        default:
            ESP_LOGI(TAG, "Mesh event id: %ld", event_id);
            break;
    }
}

/* ================= Mesh Tree Printer ================= */
static void print_mesh_tree_task(void *arg)
{
    while (1) {
        int layer = esp_mesh_get_layer();

        mesh_addr_t parent_bssid;
        esp_err_t parent_ok = esp_mesh_get_parent_bssid(&parent_bssid);

        uint8_t total_nodes = esp_mesh_get_total_node_num();

        ESP_LOGI(TAG, "===== MESH TREE =====");
        ESP_LOGI(TAG, "Self Layer: %d", layer);
        if (parent_ok == ESP_OK) {
            ESP_LOGI(TAG, "Parent MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                     parent_bssid.addr[0], parent_bssid.addr[1], parent_bssid.addr[2],
                     parent_bssid.addr[3], parent_bssid.addr[4], parent_bssid.addr[5]);
        } else {
            ESP_LOGI(TAG, "Parent: Not connected");
        }
        ESP_LOGI(TAG, "Total nodes (including self): %d", total_nodes);

        // Simple ASCII tree visualization
        for (int i = 0; i < layer; i++) {
            printf("  "); // indent for layer
        }
        printf("|- [SELF]\n");

        if (parent_ok == ESP_OK) {
            for (int i = 0; i < layer; i++) printf("  ");
            printf("  |- Parent: %02X:%02X:%02X:%02X:%02X:%02X\n",
                   parent_bssid.addr[0], parent_bssid.addr[1], parent_bssid.addr[2],
                   parent_bssid.addr[3], parent_bssid.addr[4], parent_bssid.addr[5]);
        }

        printf("=====================\n\n");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/* ================= MAIN ================= */
void app_main(void)
{
    /* GPIO */
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_GPIO, 0);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(MESH_CHANNEL, WIFI_SECOND_CHAN_NONE));

    /* ESP-NOW */
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_rx_cb));

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, broadcast_mac, ESP_NOW_ETH_ALEN);
    peer.channel = MESH_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    ESP_LOGI(TAG, "ESP-NOW ready");

    /* Mesh */
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &mesh_event_handler,
                                               NULL));
    ESP_ERROR_CHECK(esp_mesh_init());

    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    uint8_t mesh_id[6] = {0x7A,0x7A,0x7A,0x7A,0x7A,0x7A};
    memcpy(cfg.mesh_id.addr, mesh_id, 6);
    cfg.channel = MESH_CHANNEL;
    cfg.mesh_ap.max_connection = 6;

    const char *router_ssid = "teste2";
    const char *router_pass = "teste25g";
    memcpy(cfg.router.ssid, router_ssid, strlen(router_ssid));
    cfg.router.ssid_len = strlen(router_ssid);
    memcpy(cfg.router.password, router_pass, strlen(router_pass));

    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    ESP_ERROR_CHECK(esp_mesh_start());
    ESP_LOGI(TAG, "Mesh initialized");

    /* Tasks */
    xTaskCreate(espnow_tx_task, "espnow_tx", 4096, NULL, 5, NULL);
    xTaskCreate(print_mesh_tree_task, "mesh_tree", 4096, NULL, 4, NULL);
}
