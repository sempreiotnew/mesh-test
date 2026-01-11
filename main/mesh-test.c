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


extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t index_js_start[]   asm("_binary_index_js_start");
extern const uint8_t index_js_end[]     asm("_binary_index_js_end");



#define LED_GPIO GPIO_NUM_2
#define BUTTON_GPIO GPIO_NUM_0
static const char *TAG = "wifi_ap_web";

static bool led_state = false; // LED off initially

// HTTP GET handler (main page)
esp_err_t root_get_handler(httpd_req_t *req) {
    size_t len = index_html_end - index_html_start;
    return httpd_resp_send(req, (const char *)index_html_start, len);
}
// HTTP POST handler to toggle LED via web
// esp_err_t led_post_handler(httpd_req_t *req)
// {
//     led_state = !led_state;
//     gpio_set_level(LED_GPIO, led_state ? 1 : 0);

//     httpd_resp_set_status(req, "303 See Other");
//     httpd_resp_set_hdr(req, "Location", "/");
//     httpd_resp_send(req, NULL, 0);
//     ESP_LOGI(TAG, "LED toggled %s via web", led_state ? "ON" : "OFF");
//     return ESP_OK;
// }

esp_err_t led_post_handler(httpd_req_t *req)
{
    led_state = !led_state;
    gpio_set_level(LED_GPIO, led_state ? 1 : 0);

    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "LED toggled %s via web", led_state ? "ON" : "OFF");
    return ESP_OK;
}


// HTTP GET handler to return current LED state (for AJAX)
esp_err_t led_state_get_handler(httpd_req_t *req)
{
    const char *state = led_state ? "ON" : "OFF";
    httpd_resp_send(req, state, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// URI registration
httpd_uri_t root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler,
    .user_ctx = NULL
};

httpd_uri_t led_toggle = {
    .uri = "/led",
    .method = HTTP_POST,
    .handler = led_post_handler,
    .user_ctx = NULL
};

httpd_uri_t led_state_uri = {
    .uri = "/led_state",
    .method = HTTP_GET,
    .handler = led_state_get_handler,
    .user_ctx = NULL
};

// Start webserver
httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &led_toggle);
        httpd_register_uri_handler(server, &led_state_uri);
    }
    return server;
}

// Button task
void button_task(void *arg)
{
    int last_button_state = 1; // not pressed
    while (1) {
        int button_state = gpio_get_level(BUTTON_GPIO);

        if (button_state == 0 && last_button_state == 1) { // falling edge
            led_state = !led_state;
            gpio_set_level(LED_GPIO, led_state ? 1 : 0);
            ESP_LOGI(TAG, "Button pressed! LED is now %s", led_state ? "ON" : "OFF");
        }

        last_button_state = button_state;
        vTaskDelay(pdMS_TO_TICKS(50)); // debounce
    }
}

// Webserver task
void webserver_task(void *arg)
{
    start_webserver();
    vTaskDelete(NULL);
}

void app_main(void)
{
    // NVS
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // LED init
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);

    // Button init
    gpio_reset_pin(BUTTON_GPIO);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_GPIO, GPIO_PULLUP_ONLY);

    // Wi-Fi AP
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

    // Tasks
    xTaskCreate(webserver_task, "webserver_task", 4096, NULL, 5, NULL);
    xTaskCreate(button_task, "button_task", 2048, NULL, 10, NULL);
}
