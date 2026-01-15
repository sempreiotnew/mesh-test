#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#define RED_GPIO       GPIO_NUM_27
#define GREEN_GPIO     GPIO_NUM_26
#define BLUE_GPIO      GPIO_NUM_25
#define BUTTON_GPIO    GPIO_NUM_0

static const char *TAG = "ESP_NOW_BTN_LED";

/* ============================
   CHANGE THIS MAC ADDRESS
   Put the OTHER ESP32 MAC here
   ============================ */
//    30:AE:A4:84:7C:90 - amarela
//.    00:4B:12:2D:F7:8C

//write to amarela
static uint8_t peer_mac[] = {
    0x00, 0x4B, 0x12, 0x2D, 0xF7, 0x8C
};

//write to normal
// static uint8_t peer_mac[] = {
//     0x30, 0xAE, 0xA4, 0x84, 0x7C, 0x90
// };

/* Message format */
typedef struct {
    uint8_t toggle;
} espnow_msg_t;

/* ESP-NOW receive callback */
static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data, int len)
{
    if (len != sizeof(espnow_msg_t)) return;

    espnow_msg_t msg;
    memcpy(&msg, data, sizeof(msg));

    if (msg.toggle) {
        gpio_set_level(RED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(RED_GPIO, 0);

        gpio_set_level(GREEN_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(GREEN_GPIO, 0);

        gpio_set_level(BLUE_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(BLUE_GPIO, 0);

        ESP_LOGI(TAG, "RGB LED blinked by peer");
    }
}

/* Send toggle command */
static void send_toggle(void)
{
    espnow_msg_t msg = {
        .toggle = 1
    };

    esp_err_t err = esp_now_send(peer_mac, (uint8_t *)&msg, sizeof(msg));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Toggle sent");
    } else {
        ESP_LOGE(TAG, "Send failed: %s", esp_err_to_name(err));
    }
}

/* Button task */
static void button_task(void *arg)
{
    bool last_state = true;

    while (1) {
        bool state = gpio_get_level(BUTTON_GPIO);

        if (last_state && !state) {  // falling edge
            vTaskDelay(pdMS_TO_TICKS(50)); // debounce
            if (!gpio_get_level(BUTTON_GPIO)) {
                send_toggle();
            }
        }

        last_state = state;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* WiFi init for ESP-NOW */
static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ESP-NOW init */
static void espnow_init(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, peer_mac, 6);
    peer.channel = 0;
    peer.encrypt = false;

    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    /* GPIO setup */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RED_GPIO) |
                        (1ULL << GREEN_GPIO) |
                        (1ULL << BLUE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);

    io_conf.pin_bit_mask = (1ULL << BUTTON_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    gpio_set_level(RED_GPIO, 0);
    gpio_set_level(GREEN_GPIO, 0);
    gpio_set_level(BLUE_GPIO, 0);

    /* WiFi + ESP-NOW */
    wifi_init();
    espnow_init();

    /* Print MAC address */
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "My MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Button task */
    xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "ESP-NOW ready");
}
