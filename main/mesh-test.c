#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "driver/gpio.h"

#define LED_GPIO GPIO_NUM_2
static const char *TAG = "wifi_ap_web";

// Simple HTML page
static const char *HTML_PAGE = "<!DOCTYPE html>"
"<html>"
"<head><title>ESP32 AP</title></head>"
"<body>"
"<h1>Hello from ESP32!</h1>"
"<p>This is a simple Wi-Fi AP page.</p>"
"</body>"
"</html>";

// HTTP GET handler
esp_err_t root_get_handler(httpd_req_t *req)
{
    gpio_set_level(LED_GPIO, 1); // LED on when page served
    httpd_resp_send(req, HTML_PAGE, HTTPD_RESP_USE_STRLEN);
    gpio_set_level(LED_GPIO, 0); // LED off
    return ESP_OK;
}

// Register HTTP server URIs
httpd_uri_t root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler,
    .user_ctx = NULL
};

// Start HTTP server
httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &root);
    }
    return server;
}

void app_main(void)
{
    // Initialize NVS
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize LED
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    // Initialize Wi-Fi in AP mode
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "ESP32_AP",
            .ssid_len = strlen("ESP32_AP"),
            .channel = 1,
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi AP started. Connect to SSID: %s", wifi_config.ap.ssid);

    // Start web server
    start_webserver();
}
