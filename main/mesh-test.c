#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "mdns.h"

#define AP_SSID "SempreIoT"
#define AP_PASS "12345678"

static const char *TAG = "PROV_AP";

/* ================= EVENT GROUP ================= */

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static bool connectivity_started = false;
static httpd_handle_t http_server = NULL;

/* ================= CONNECTIVITY TASK ================= */

static void connectivity_task(void *arg)
{
    esp_http_client_config_t cfg = {
        .url = "http://clients3.google.com/generate_204",
        .timeout_ms = 3000,
    };

    while (1) {
        xEventGroupWaitBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT,
            false,
            true,
            portMAX_DELAY
        );

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        esp_err_t err = esp_http_client_perform(client);

        if (err == ESP_OK &&
            esp_http_client_get_status_code(client) == 204) {
            ESP_LOGI(TAG, "Internet OK");
        } else {
            ESP_LOGW(TAG, "No internet access");
        }

        esp_http_client_cleanup(client);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ================= WIFI EVENTS ================= */

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED) {

        ESP_LOGW(TAG, "STA disconnected, retrying...");
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    }

    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP) {

        ESP_LOGI(TAG, "STA got IP");
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);

        if (!connectivity_started) {
            connectivity_started = true;
            xTaskCreate(connectivity_task, "connectivity", 4096, NULL, 5, NULL);
        }
    }
}


static bool load_wifi_from_nvs(wifi_config_t *sta_cfg)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err != ESP_OK) return false;

    size_t ssid_len = sizeof(sta_cfg->sta.ssid);
    size_t pass_len = sizeof(sta_cfg->sta.password);

    err = nvs_get_str(nvs, "ssid", (char *)sta_cfg->sta.ssid, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return false;
    }

    nvs_get_str(nvs, "pass", (char *)sta_cfg->sta.password, &pass_len);
    nvs_close(nvs);

    return true;
}



/* ================= HTML + SCAN ================= */

static esp_err_t root_get(httpd_req_t *req)
{
    uint16_t ap_count = 0;
    wifi_ap_record_t ap_records[20];

    esp_wifi_scan_start(NULL, true);
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;
    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    httpd_resp_set_type(req, "text/html");

    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width'>"
        "<script>function s(x){document.getElementById('ssid').value=x;}</script>"
        "</head><body><h2>Configure WiFi</h2><ul>"
    );

    for (int i = 0; i < ap_count; i++) {
        char line[128];
        snprintf(line, sizeof(line),
            "<li><a href='#' onclick=\"s('%s')\">%s (%d dBm)</a></li>",
            ap_records[i].ssid,
            ap_records[i].ssid,
            ap_records[i].rssi);
        httpd_resp_sendstr_chunk(req, line);
    }

    httpd_resp_sendstr_chunk(req,
        "</ul><hr>"
        "<form action='/save' method='post'>"
        "SSID:<br><input id='ssid' name='ssid'><br>"
        "Password:<br><input name='pass' type='password'><br><br>"
        "<input type='submit' value='Save'></form></body></html>"
    );

    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ================= SWITCH TO STA ================= */

static void switch_to_sta_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500)); // allow HTTP to flush

    if (http_server) {
        httpd_stop(http_server);
        http_server = NULL;
    }

    char ssid[32] = {0};
    char pass[64] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(pass);

    nvs_handle_t nvs;
    nvs_open("wifi", NVS_READONLY, &nvs);
    nvs_get_str(nvs, "ssid", ssid, &ssid_len);
    nvs_get_str(nvs, "pass", pass, &pass_len);
    nvs_close(nvs);

    wifi_config_t sta = {0};
    strcpy((char *)sta.sta.ssid, ssid);
    strcpy((char *)sta.sta.password, pass);

    ESP_LOGI(TAG, "Switching to STA (%s)", ssid);

    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    esp_wifi_start();
    esp_wifi_connect();

    vTaskDelete(NULL);
}

/* ================= SAVE HANDLER ================= */

static esp_err_t save_post(httpd_req_t *req)
{
    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;
    buf[len] = 0;

    char ssid[32] = {0};
    char pass[64] = {0};

    httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(buf, "pass", pass, sizeof(pass));

    ESP_LOGI(TAG, "SSID=%s PASS=%s", ssid, pass);

    nvs_handle_t nvs;
    nvs_open("wifi", NVS_READWRITE, &nvs);
    nvs_set_str(nvs, "ssid", ssid);
    nvs_set_str(nvs, "pass", pass);
    nvs_commit(nvs);
    nvs_close(nvs);

    httpd_resp_sendstr(req, "Saved. Connecting...");

    xTaskCreate(switch_to_sta_task, "switch_sta", 4096, NULL, 5, NULL);
    return ESP_OK;
}

/* ================= HTTP SERVER ================= */

static void start_http(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    httpd_start(&http_server, &cfg);

    httpd_uri_t root = {.uri="/", .method=HTTP_GET, .handler=root_get};
    httpd_uri_t save = {.uri="/save", .method=HTTP_POST, .handler=save_post};

    httpd_register_uri_handler(http_server, &root);
    httpd_register_uri_handler(http_server, &save);
}

/* ================= MDNS ================= */

static void start_mdns(void)
{
    mdns_init();
    mdns_hostname_set("sempreiot");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

/* ================= MAIN ================= */

void app_main(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    wifi_event_group = xEventGroupCreate();

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t ap = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .password = AP_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        }
    };

    wifi_config_t sta_cfg = {0};

    if (load_wifi_from_nvs(&sta_cfg)) {
        ESP_LOGI(TAG, "Found saved WiFi credentials, starting STA");
        ESP_LOGW(TAG, "ssid: %s pass: %s chanel: %d", sta_cfg.ap.ssid, sta_cfg.ap.password, sta_cfg.ap.channel);
        
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        esp_wifi_start();
        esp_wifi_connect();

    } else {
        ESP_LOGI(TAG, "No WiFi credentials, starting provisioning AP");

        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_AP, &ap);
        esp_wifi_start();

        start_mdns();
        start_http();
    }


    start_mdns();
    start_http();

    ESP_LOGI(TAG, "Provisioning AP ready: http://sempreiot.local");
}
