#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_system.h"
#include "lwip/ip4_addr.h"
#include "atu_oled_mirror.h"



#define TAG "ESP32_UI"

#define WS_PUSH_INTERVAL_MS 120
#define WS_IDLE_INTERVAL_MS 500

// Define GPIO pins
#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<GPIO_NUM_2) | (1ULL<<GPIO_NUM_5) | (1ULL<<GPIO_NUM_19) | (1ULL<<GPIO_NUM_18))
#define GPIO_INPUT_PIN_SEL   (1ULL<<GPIO_NUM_23)
#define GPIO_STATUS_LED      GPIO_NUM_2   // ESP32 DevKit V1 onboard blue LED (often active LOW)
#define GPIO_STATUS_LED_ON_LEVEL  1
#define GPIO_STATUS_LED_OFF_LEVEL (!GPIO_STATUS_LED_ON_LEVEL)
#define GPIO_OUTPUT_IO_1     GPIO_NUM_5  //AUTO-MANUAL
#define GPIO_OUTPUT_IO_2     GPIO_NUM_19  //TUNE
#define GPIO_OUTPUT_IO_3     GPIO_NUM_18  //BYPASS (placeholder pin)
#define GPIO_TUNE_SENSE      GPIO_NUM_23 // Monitors the real TUNE/RESET line through an external divider.
#define AP_RECOVERY_POLL_MS  100

// Global socket descriptor used by the current WebSocket client path.
static int client_fd = -1;

// Wi-Fi config persisted in NVS (avoids hard-coded SSID/password)
#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_KEY_SSID  "ssid"
#define WIFI_NVS_KEY_PASS  "pass"
#define WIFI_TX_POWER_QDBM 54  // 13.5 dBm (54 * 0.25 dBm)

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static httpd_handle_t s_http_server = NULL;
static TaskHandle_t s_ws_task_handle = NULL;
static int s_wifi_retry_count = 0;
static const int s_ap_recovery_hold_ms = 10000;
static volatile bool s_has_active_client = false;
static volatile bool s_sta_connected = false;
static volatile bool s_ap_mode_active = false;
static volatile bool s_ap_recovery_requested = false;
static bool s_oled_mirror_started = false;

// Keep large WS/OLED buffers out of task stack to avoid stack overflow in ws task.
static atu_oled_frame_t s_ws_oled_frame;
static char s_ws_oled_hex[(ATU_OLED_WIDTH * ATU_OLED_HEIGHT / 4) + 1]; // 1024 + '\0'
static char s_ws_payload[1536];

static void start_services_once(void);
static void wifi_start_station(const char *ssid, const char *pass);
static void wifi_start_softap(void);
static bool wifi_load_credentials(char *ssid, size_t ssid_size, char *pass, size_t pass_size);
static esp_err_t wifi_save_credentials(const char *ssid, const char *pass);
static esp_err_t wifi_clear_credentials(void);
static esp_err_t trigger_ap_recovery(const char *reason, int restart_delay_ms);
static void request_restart_after_ms(int delay_ms);
static void status_led_task(void *pvParameter);
static void ap_recovery_monitor_task(void *pvParameter);
static size_t oled_frame_to_hex(const atu_oled_frame_t *frame, char *dst, size_t dst_size);
static void ensure_oled_mirror_started(void);


// Forward declaration of start_webserver
httpd_handle_t start_webserver(void);

// Forward declaration of websocket_broadcast_task
void websocket_broadcast_task(void *pvParameter);

static void restart_after_delay_task(void *pvParameter) {
    int delay_ms = (int)(intptr_t)pvParameter;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
}

static void request_restart_after_ms(int delay_ms) {
    xTaskCreate(restart_after_delay_task, "restart_task", 2048, (void *)(intptr_t)delay_ms, 5, NULL);
}

static void status_led_task(void *pvParameter) {
    (void)pvParameter;

    bool blink_phase = false;
    while (1) {
        if (s_ap_mode_active) {
            // AP setup mode: fast blink to make discovery obvious.
            blink_phase = !blink_phase;
            gpio_set_level(GPIO_STATUS_LED, blink_phase ? GPIO_STATUS_LED_ON_LEVEL : GPIO_STATUS_LED_OFF_LEVEL);
            vTaskDelay(pdMS_TO_TICKS(125));
            continue;
        }

        if (s_sta_connected && s_has_active_client) {
            // STA mode with client connected: slow blink.
            blink_phase = !blink_phase;
            gpio_set_level(GPIO_STATUS_LED, blink_phase ? GPIO_STATUS_LED_ON_LEVEL : GPIO_STATUS_LED_OFF_LEVEL);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (s_sta_connected) {
            // STA mode with no client: solid ON.
            blink_phase = false;
            gpio_set_level(GPIO_STATUS_LED, GPIO_STATUS_LED_ON_LEVEL);
        } else {
            // Not connected yet: OFF.
            blink_phase = false;
            gpio_set_level(GPIO_STATUS_LED, GPIO_STATUS_LED_OFF_LEVEL);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void ap_recovery_monitor_task(void *pvParameter) {
    (void)pvParameter;

    TickType_t low_started_ticks = 0;

    while (1) {
        const bool line_is_low = (gpio_get_level(GPIO_TUNE_SENSE) == 0);

        if (!s_ap_recovery_requested && line_is_low) {
            if (low_started_ticks == 0) {
                low_started_ticks = xTaskGetTickCount();
            } else {
                const int64_t held_ticks = xTaskGetTickCount() - low_started_ticks;
                const int64_t held_ms = held_ticks * portTICK_PERIOD_MS;
                if (held_ms >= s_ap_recovery_hold_ms) {
                    s_ap_recovery_requested = true;
                    char reason[96];
                    snprintf(reason,
                             sizeof(reason),
                             "TUNE/RESET line held low for %lld ms",
                             (long long)held_ms);
                    esp_err_t err = trigger_ap_recovery(reason, 500);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to start AP recovery after long hold: %s", esp_err_to_name(err));
                        s_ap_recovery_requested = false;
                    }
                }
            }
        } else if (!line_is_low) {
            low_started_ticks = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(AP_RECOVERY_POLL_MS));
    }
}

static bool wifi_load_credentials(char *ssid, size_t ssid_size, char *pass, size_t pass_size) {
    if (!ssid || ssid_size == 0 || !pass || pass_size == 0) {
        return false;
    }
    ssid[0] = '\0';
    pass[0] = '\0';

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return false;
    }

    size_t ssid_len = ssid_size;
    err = nvs_get_str(nvs, WIFI_NVS_KEY_SSID, ssid, &ssid_len);
    if (err != ESP_OK || ssid[0] == '\0') {
        nvs_close(nvs);
        ssid[0] = '\0';
        pass[0] = '\0';
        return false;
    }

    size_t pass_len = pass_size;
    err = nvs_get_str(nvs, WIFI_NVS_KEY_PASS, pass, &pass_len);
    if (err != ESP_OK) {
        pass[0] = '\0';
    }

    nvs_close(nvs);
    return true;
}

static esp_err_t wifi_save_credentials(const char *ssid, const char *pass) {
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, WIFI_NVS_KEY_PASS, pass ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

static esp_err_t wifi_clear_credentials(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    (void)nvs_erase_key(nvs, WIFI_NVS_KEY_SSID);
    (void)nvs_erase_key(nvs, WIFI_NVS_KEY_PASS);
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t trigger_ap_recovery(const char *reason, int restart_delay_ms) {
    esp_err_t err = wifi_clear_credentials();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear Wi-Fi credentials for AP recovery: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "AP recovery requested: %s", reason ? reason : "unspecified");
    request_restart_after_ms(restart_delay_ms);
    return ESP_OK;
}

static void wifi_start_station(const char *ssid, const char *pass) {
    s_ap_mode_active = false;
    s_wifi_retry_count = 0;
    s_has_active_client = false;

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (pass) {
        strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM));
    ESP_LOGI(TAG, "Wi-Fi initialized (STA). Connecting to SSID: %s", ssid);
}

static void wifi_start_softap(void) {
    s_ap_mode_active = true;
    s_sta_connected = false;
    s_wifi_retry_count = 0;
    s_has_active_client = false;
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    // Force fixed AP IP (some environments may otherwise end up at 192.168.4.2)
    {
        esp_netif_ip_info_t ip_info = { 0 };
        IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

        esp_err_t dhcps_stop_err = esp_netif_dhcps_stop(s_ap_netif);
        if (dhcps_stop_err != ESP_OK && dhcps_stop_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
            ESP_LOGW(TAG, "Failed to stop DHCP server: %s", esp_err_to_name(dhcps_stop_err));
        }

        ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip_info));

        esp_err_t dhcps_start_err = esp_netif_dhcps_start(s_ap_netif);
        if (dhcps_start_err != ESP_OK && dhcps_start_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGW(TAG, "Failed to start DHCP server: %s", esp_err_to_name(dhcps_start_err));
        }
    }

    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, mac));

    wifi_config_t ap_config = { 0 };
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid),
             "ATU1000-SETUP-%02X%02X%02X", mac[3], mac[4], mac[5]);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    esp_err_t stop_err = esp_wifi_stop();
    if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_ERROR_CHECK(stop_err);
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20));
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM));

    ESP_LOGW(TAG, "Open setup AP started: SSID=%s (no password)", (char *)ap_config.ap.ssid);

}

static void start_services_once(void) {
    if (s_http_server != NULL) {
        return;
    }

    s_http_server = start_webserver();
    if (s_http_server != NULL && s_ws_task_handle == NULL) {
        ESP_LOGI(TAG, "Creating websocket_broadcast_task...");
        xTaskCreate(websocket_broadcast_task, "ws_broadcast_task", 4096, s_http_server, 5, &s_ws_task_handle);
    }
}

// Wi-Fi event handler.
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_ap_mode_active) {
            return;
        }

        s_sta_connected = false;
        s_wifi_retry_count++;
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        int reason = event ? event->reason : 0;
        ESP_LOGI(TAG,
                 "Trying to reconnect to Wi-Fi... (attempt %d, reason=%d)",
                 s_wifi_retry_count,
                 reason);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected to Wi-Fi, IP: " IPSTR, IP2STR(&event->ip_info.ip));

        // Start the web server after STA receives an IP address.
        s_sta_connected = true;
        s_wifi_retry_count = 0;
        start_services_once();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "AP mode started (Wi-Fi setup).");
        s_ap_mode_active = true;
        s_sta_connected = false;
        start_services_once();
    }
}

// Initialize Wi-Fi.
void wifi_init(void) {
    ESP_LOGI(TAG, "Initializing Wi-Fi...");
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop_err);
    }
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Register Wi-Fi event handlers.
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    char ssid[33];
    char pass[65];
    if (wifi_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        s_wifi_retry_count = 0;
        wifi_start_station(ssid, pass);
    } else {
        ESP_LOGW(TAG, "Wi-Fi is not configured. Starting AP mode for setup.");
        s_sta_connected = false;
        wifi_start_softap();
    }
}

// Initialize SPIFFS.
void spiffs_init(void) {
    ESP_LOGI(TAG, "Initializing SPIFFS...");
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SPIFFS: total: %d, used: %d", total, used);
    }
}


// Generic static-file handler.
esp_err_t file_handler(httpd_req_t *req, const char *filepath) {
    // Select MIME type from the file extension.
    const char *ext = strrchr(filepath, '.');
    if (ext) { // Set the appropriate Content-Type header.
        if (strcmp(ext, ".css") == 0) {
            httpd_resp_set_type(req, "text/css");
        } else if (strcmp(ext, ".js") == 0) {
            httpd_resp_set_type(req, "application/javascript");
        } else if (strcmp(ext, ".html") == 0) {
            httpd_resp_set_type(req, "text/html");
        } else if (strcmp(ext, ".png") == 0) {
            httpd_resp_set_type(req, "image/png");
        } else {
            httpd_resp_set_type(req, "text/plain"); // Default for unknown files.
        }
    }

    // Open the file.
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file : %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    // Stream file contents.
    char buffer[1024];
    size_t chunksize;
    while ((chunksize = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (httpd_resp_send_chunk(req, buffer, chunksize) != ESP_OK) {
            fclose(file);
            ESP_LOGE(TAG, "File sending failed!");
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }

    // Close the file.
    fclose(file);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// Handler for index.html.
esp_err_t index_handler(httpd_req_t *req) {
    return file_handler(req, "/spiffs/index.html");
}

// Handler for style.css.
esp_err_t style_handler(httpd_req_t *req) {
    return file_handler(req, "/spiffs/style.css");
}

// Handler for script.js.
esp_err_t script_handler(httpd_req_t *req) {
    return file_handler(req, "/spiffs/script.js");
}

// Handler for favicon.png.
esp_err_t favicon_handler(httpd_req_t *req) {
    return file_handler(req, "/spiffs/favicon.png");
}

static esp_err_t send_json(httpd_req_t *req, int status_code, const char *json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, (status_code == 200) ? "200 OK" :
                               (status_code == 400) ? "400 Bad Request" :
                               (status_code == 404) ? "404 Not Found" :
                               (status_code == 413) ? "413 Payload Too Large" :
                               (status_code == 500) ? "500 Internal Server Error" : "200 OK");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static void url_decode_inplace(char *s) {
    char *src = s;
    char *dst = s;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            int hi = hex_value(src[1]);
            int lo = hex_value(src[2]);
            if (hi >= 0 && lo >= 0) {
                *dst++ = (char)((hi << 4) | lo);
                src += 3;
            } else {
                *dst++ = *src++;
            }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static esp_err_t wifi_status_handler(httpd_req_t *req) {
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);

    bool connected = false;
    int rssi_dbm = 0;
    bool has_rssi = false;
    char ssid[33] = {0};
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        connected = true;
        rssi_dbm = (int)ap_info.rssi;
        has_rssi = true;
        strncpy(ssid, (const char *)ap_info.ssid, sizeof(ssid) - 1);
    } else {
        char saved_ssid[33];
        char saved_pass[65];
        if (wifi_load_credentials(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass))) {
            strncpy(ssid, saved_ssid, sizeof(ssid) - 1);
        }
    }

    // In AP mode, expose RSSI from connected station(s) as a local-link diagnostic.
    if (!has_rssi && (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)) {
        wifi_sta_list_t sta_list = {0};
        if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK && sta_list.num > 0) {
            connected = true;
            int best_rssi = -127;
            for (int i = 0; i < sta_list.num; i++) {
                if ((int)sta_list.sta[i].rssi > best_rssi) {
                    best_rssi = (int)sta_list.sta[i].rssi;
                }
            }
            rssi_dbm = best_rssi;
            has_rssi = true;
        }
    }

    char ip_str[16] = "0.0.0.0";
    esp_netif_ip_info_t ip_info;
    if (mode == WIFI_MODE_STA && s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    } else if (mode == WIFI_MODE_AP && s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    }

    const char *mode_str = (mode == WIFI_MODE_STA) ? "sta" :
                           (mode == WIFI_MODE_AP) ? "ap" :
                           (mode == WIFI_MODE_APSTA) ? "apsta" : "unknown";

    bool has_saved = (ssid[0] != '\0');

    char json[256];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"mode\":\"%s\",\"connected\":%s,\"has_saved\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi_dbm\":%d,\"has_rssi\":%s}",
             mode_str, connected ? "true" : "false", has_saved ? "true" : "false", ssid, ip_str, rssi_dbm, has_rssi ? "true" : "false");
    return send_json(req, 200, json);
}

static esp_err_t wifi_save_handler(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len > 512) {
        return send_json(req, 413, "{\"ok\":false,\"error\":\"invalid_body\"}");
    }

    char *body = calloc(1, req->content_len + 1);
    if (!body) {
        return send_json(req, 500, "{\"ok\":false,\"error\":\"no_mem\"}");
    }

    int total = 0;
    while (total < req->content_len) {
        int received = httpd_req_recv(req, body + total, req->content_len - total);
        if (received <= 0) {
            free(body);
            return send_json(req, 400, "{\"ok\":false,\"error\":\"recv_failed\"}");
        }
        total += received;
    }
    body[total] = '\0';
    url_decode_inplace(body);

    char ssid[33] = {0};
    char pass[65] = {0};

    // Parse application/x-www-form-urlencoded: ssid=...&password=...
    char *saveptr = NULL;
    for (char *pair = strtok_r(body, "&", &saveptr); pair != NULL; pair = strtok_r(NULL, "&", &saveptr)) {
        char *eq = strchr(pair, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = pair;
        const char *val = eq + 1;
        if (strcmp(key, "ssid") == 0) {
            strncpy(ssid, val, sizeof(ssid) - 1);
        } else if (strcmp(key, "password") == 0) {
            strncpy(pass, val, sizeof(pass) - 1);
        }
    }

    free(body);

    if (ssid[0] == '\0') {
        return send_json(req, 400, "{\"ok\":false,\"error\":\"missing_ssid\"}");
    }

    esp_err_t err = wifi_save_credentials(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save Wi-Fi credentials: %s", esp_err_to_name(err));
        return send_json(req, 500, "{\"ok\":false,\"error\":\"save_failed\"}");
    }

    send_json(req, 200, "{\"ok\":true,\"restarting\":true}");
    request_restart_after_ms(500);
    return ESP_OK;
}

static esp_err_t wifi_forget_handler(httpd_req_t *req) {
    esp_err_t err = trigger_ap_recovery("Web UI Forget Wi-Fi", 500);
    if (err != ESP_OK) {
        return send_json(req, 500, "{\"ok\":false,\"error\":\"clear_failed\"}");
    }

    send_json(req, 200, "{\"ok\":true,\"restarting\":true}");
    return ESP_OK;
}

static esp_err_t mirror_reset_handler(httpd_req_t *req) {
    esp_err_t err = atu_oled_mirror_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset OLED mirror RAM: %s", esp_err_to_name(err));
        return send_json(req, 500, "{\"ok\":false,\"error\":\"mirror_reset_failed\"}");
    }

    ESP_LOGW(TAG, "OLED mirror RAM reset requested from web UI.");
    return send_json(req, 200, "{\"ok\":true}");
}

// WebSocket handler declaration.
esp_err_t websocket_handler(httpd_req_t *req);

// Initialize the web server.
httpd_handle_t start_webserver(void) {
    ESP_LOGI(TAG, "Initializing web server...");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    // Reserve enough URI slots so /ws registration never fails.
    config.max_uri_handlers = 12;
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t index_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = index_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &index_uri);

        httpd_uri_t style_uri = {
            .uri = "/style.css",
            .method = HTTP_GET,
            .handler = style_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &style_uri);

        httpd_uri_t script_uri = {
            .uri = "/script.js",
            .method = HTTP_GET,
            .handler = script_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &script_uri);

        httpd_uri_t favicon_uri = {
            .uri = "/favicon.png",
            .method = HTTP_GET,
            .handler = favicon_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &favicon_uri);

        httpd_uri_t wifi_status_uri = {
            .uri = "/api/wifi",
            .method = HTTP_GET,
            .handler = wifi_status_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &wifi_status_uri);

        httpd_uri_t wifi_save_uri = {
            .uri = "/api/wifi",
            .method = HTTP_POST,
            .handler = wifi_save_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &wifi_save_uri);

        httpd_uri_t wifi_forget_uri = {
            .uri = "/api/wifi/forget",
            .method = HTTP_POST,
            .handler = wifi_forget_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &wifi_forget_uri);

        httpd_uri_t mirror_reset_uri = {
            .uri = "/api/mirror/reset",
            .method = HTTP_POST,
            .handler = mirror_reset_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &mirror_reset_uri);

        httpd_uri_t ws_uri = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = websocket_handler,
            .user_ctx = server,
            .is_websocket = true
        };
        httpd_register_uri_handler(server, &ws_uri);

        ESP_LOGI(TAG, "Web server started and ready for connections.");

        wifi_mode_t mode = WIFI_MODE_NULL;
        wifi_bandwidth_t bw = WIFI_BW_HT20;
        int8_t tx_power_qdbm = 0;
        const char *bw_str = "unknown";

        if (esp_wifi_get_mode(&mode) == ESP_OK) {
            wifi_interface_t iface = (mode == WIFI_MODE_AP) ? WIFI_IF_AP : WIFI_IF_STA;
            if (esp_wifi_get_bandwidth(iface, &bw) == ESP_OK) {
                bw_str = (bw == WIFI_BW_HT40) ? "HT40 (40 MHz)" : "HT20 (20 MHz)";
            }
        }
        if (esp_wifi_get_max_tx_power(&tx_power_qdbm) != ESP_OK) {
            tx_power_qdbm = 0;
        }
        ESP_LOGI(TAG, "Wi-Fi runtime: BW=%s | Max TX Power=%.2f dBm", bw_str, ((float)tx_power_qdbm) / 4.0f);
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }

    return server;
}
void remove_disconnected_client(int client_fd) {
    if (client_fd >= 0) {
        ESP_LOGI(TAG, "Removing disconnected client: client_fd=%d", client_fd);
        // Additional cleanup can be added here if needed.
        client_fd = -1; // Mark the client as disconnected.
    }
}


void websocket_broadcast_task(void *pvParameter) {
    httpd_handle_t server = (httpd_handle_t)pvParameter;
    // OLED mirror only payload (analog/power metrics removed on purpose).
    char *data = s_ws_payload;
    bool last_oled_valid = false;
    uint32_t last_oled_hash = 0;

    vTaskDelay(pdMS_TO_TICKS(1000)); // Wait 1 second before starting.

    while (1) {
        if (server == NULL) {
            ESP_LOGE(TAG, "WebSocket server is not initialized.");
            s_has_active_client = false;
            vTaskDelay(pdMS_TO_TICKS(WS_IDLE_INTERVAL_MS));
            continue;
        }

        size_t max_clients = CONFIG_LWIP_MAX_LISTENING_TCP;
        int client_fds[max_clients];
        size_t fds = max_clients;

        esp_err_t ret = httpd_get_client_list(server, &fds, client_fds);
        if (ret != ESP_OK || fds == 0) {
            s_has_active_client = false;
            vTaskDelay(pdMS_TO_TICKS(WS_IDLE_INTERVAL_MS));
            continue;
        }

        int ws_client_fds[max_clients];
        size_t ws_fds = 0;
        for (size_t i = 0; i < fds; i++) {
            if (httpd_ws_get_fd_info(server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                ws_client_fds[ws_fds++] = client_fds[i];
            }
        }

        if (ws_fds == 0) {
            s_has_active_client = false;
            vTaskDelay(pdMS_TO_TICKS(WS_IDLE_INTERVAL_MS));
            continue;
        }

        s_has_active_client = true;

        bool has_oled_payload = false;
        bool oled_changed = false;
        if (atu_oled_mirror_get_frame(&s_ws_oled_frame) && s_ws_oled_frame.valid) {
            size_t hex_len = oled_frame_to_hex(&s_ws_oled_frame, s_ws_oled_hex, sizeof(s_ws_oled_hex));
            if (hex_len > 0) {
                has_oled_payload = true;
                oled_changed = (!last_oled_valid || s_ws_oled_frame.hash != last_oled_hash);
                last_oled_valid = true;
                last_oled_hash = s_ws_oled_frame.hash;

                snprintf(
                    data,
                    sizeof(s_ws_payload),
                    "{\"oled\":{\"w\":128,\"h\":32,\"hash\":%lu,\"bits_hex\":\"%s\"}}",
                    (unsigned long)s_ws_oled_frame.hash,
                    s_ws_oled_hex
                );
            } else {
                snprintf(data, sizeof(s_ws_payload), "{}");
            }
        } else {
            last_oled_valid = false;
            snprintf(data, sizeof(s_ws_payload), "{}");
        }

        if (has_oled_payload && oled_changed) {
            ESP_LOGI(TAG, "WS payload: oled_hash=%lu", (unsigned long)s_ws_oled_frame.hash);
        }

        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.payload = (uint8_t *)data;
        ws_pkt.len = strlen(data);
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;

        for (size_t i = 0; i < ws_fds; i++) {
            ret = httpd_ws_send_frame_async(server, ws_client_fds[i], &ws_pkt);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send WS frame to client_fd=%d (%s). Closing session.", ws_client_fds[i], esp_err_to_name(ret));
                httpd_sess_trigger_close(server, ws_client_fds[i]);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(WS_PUSH_INTERVAL_MS));
    }
}


// WebSocket handler with GPIO control and timeout handling.

void gpio_init(void); // Local explicit declaration to avoid compile-order issues.
/*
Tip for future Windows 11 ESP-IDF + VS Code work:
Even when a function is defined globally, ESP-IDF compilation can fail if the
function is referenced before a visible prototype in this translation unit.
Keep a local forward declaration, such as `void function_name(void);`, above
the caller when needed.

*/
esp_err_t websocket_handler(httpd_req_t *req) {
    
    if (req->method == HTTP_GET) {
        
        ESP_LOGI(TAG, "WebSocket handshake completed.");
        ensure_oled_mirror_started();
                 
        return ESP_OK; // Complete the handshake without sending initial data.
    }

    // Check WebSocket state and receive frames.
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "WebSocket disconnected: client_fd=%d", httpd_req_to_sockfd(req));
            remove_disconnected_client(httpd_req_to_sockfd(req));
        } else {
            ESP_LOGE(TAG, "Failed to receive WebSocket frame: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    // Receive and process the message.
    if (ws_pkt.len > 0) {
        uint8_t *buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for payload.");
            return ESP_ERR_NO_MEM;
        }

        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to receive WebSocket payload: %s", esp_err_to_name(ret));
            free(buf);
            return ret;
        }

        //ESP_LOGI(TAG, "Received message: %s", ws_pkt.payload);
    

        // Process the buffer manually.
        if (strstr((char *)buf, "\"action\":\"toggle1\"") && strstr((char *)buf, "\"state\":\"pressed\"")) {
            gpio_set_level(GPIO_OUTPUT_IO_1, 1); // On press, drive the transistor base through a resistor.
            // The transistor collector is connected to the ATU AUTO/MANUAL input.
            ESP_LOGI(TAG, "AUTO/MANUAL pressed");
          
        } else if (strstr((char *)buf, "\"action\":\"toggle1\"") && strstr((char *)buf, "\"state\":\"released\"")) {
            gpio_set_level(GPIO_OUTPUT_IO_1, 0);
            
            ESP_LOGI(TAG, "AUTO/MANUAL released");
        } else if (strstr((char *)buf, "\"action\":\"toggle2\"") && strstr((char *)buf, "\"state\":\"pressed\"")) {
            gpio_set_level(GPIO_OUTPUT_IO_2, 1); // On press, drive the transistor base through a resistor.
            // The transistor collector is connected to the ATU RESET/TUNE input.
            ESP_LOGI(TAG, "TUNE pressed");
            
        } else if (strstr((char *)buf, "\"action\":\"toggle2\"") && strstr((char *)buf, "\"state\":\"released\"")) {
            gpio_set_level(GPIO_OUTPUT_IO_2, 0);
            ESP_LOGI(TAG, "TUNE released");
        } else if (strstr((char *)buf, "\"action\":\"toggle3\"") && strstr((char *)buf, "\"state\":\"pressed\"")) {
            gpio_set_level(GPIO_OUTPUT_IO_3, 1); // Drive the BYPASS open-collector transistor.
            ESP_LOGI(TAG, "BYPASS pressed");
        } else if (strstr((char *)buf, "\"action\":\"toggle3\"") && strstr((char *)buf, "\"state\":\"released\"")) {
            gpio_set_level(GPIO_OUTPUT_IO_3, 0); 
            ESP_LOGI(TAG, "BYPASS released");
        }
/*
Logic notes:
1. Cast `buf` to `char *` before calling `strstr`.
   The WebSocket payload is received as `uint8_t *`, while `strstr` expects a
   C string (`char *`).
2. Search for substrings such as `"action":"toggle1"`.
   If the substring is present, `strstr` returns a pointer to its first
   occurrence. Otherwise it returns NULL.

*/
        free(buf);
    }

    
    

    return ESP_OK;
}



// Initialize GPIO outputs to 0 so the external transistors remain off.
void gpio_init(void) {
    ESP_LOGI(TAG, "Initializing GPIO...");
    gpio_config_t io_conf = {0};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    esp_err_t ret = gpio_config(&io_conf);
    if (ret == ESP_OK) {
        gpio_config_t input_conf = {0};
        input_conf.intr_type = GPIO_INTR_DISABLE;
        input_conf.mode = GPIO_MODE_INPUT;
        input_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
        input_conf.pull_down_en = 0;
        input_conf.pull_up_en = 0;
        ret = gpio_config(&input_conf);
    }
    gpio_set_level(GPIO_STATUS_LED, GPIO_STATUS_LED_OFF_LEVEL);
    gpio_set_level(GPIO_OUTPUT_IO_1, 0);
    gpio_set_level(GPIO_OUTPUT_IO_2, 0);
    gpio_set_level(GPIO_OUTPUT_IO_3, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize GPIO: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "GPIO initialized successfully.");
    }
}

// Main application entry point.
void app_main(void) {
    ESP_LOGI(TAG, "Starting system...");

    // Initialize NVS.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize SPIFFS.
    spiffs_init();

    // Initialize GPIO.
    gpio_init();
    xTaskCreate(status_led_task, "status_led", 2048, NULL, 4, NULL);
    xTaskCreate(ap_recovery_monitor_task, "ap_recovery", 3072, NULL, 4, NULL);

    // Initialize Wi-Fi.
    wifi_init();

}

// Starts OLED capture only when a WebSocket client path is active.
// This avoids initializing the I2C mirror task at boot with zero web clients.
static void ensure_oled_mirror_started(void) {
    if (s_oled_mirror_started) {
        return;
    }

    esp_err_t oled_ret = atu_oled_mirror_start();
    if (oled_ret == ESP_OK) {
        s_oled_mirror_started = true;
        ESP_LOGI(TAG, "OLED mirror task started on demand.");
    } else {
        ESP_LOGW(TAG, "Failed to start OLED mirror task: %s", esp_err_to_name(oled_ret));
    }
}
// Converts the latest 128x32 OLED frame into a compact hex string (512 bytes -> 1024 hex chars).
// Packing is row-major, 8 pixels per byte, MSB first. This keeps WS payload small and deterministic.
static size_t oled_frame_to_hex(const atu_oled_frame_t *frame, char *dst, size_t dst_size) {
    if (frame == NULL || dst == NULL) {
        return 0;
    }

    const size_t total_bits = (size_t)ATU_OLED_WIDTH * (size_t)ATU_OLED_HEIGHT;
    const size_t total_bytes = total_bits / 8U;   // 4096 / 8 = 512
    const size_t total_hex = total_bytes * 2U;    // 1024 chars

    if (dst_size < (total_hex + 1U)) {
        return 0;
    }

    static const char HEX[] = "0123456789abcdef";
    size_t out = 0;

    for (int y = 0; y < ATU_OLED_HEIGHT; y++) {
        for (int x = 0; x < ATU_OLED_WIDTH; x += 8) {
            uint8_t b = 0;
            for (int k = 0; k < 8; k++) {
                if (frame->pixels[y][x + k]) {
                    b |= (uint8_t)(0x80U >> k);
                }
            }
            dst[out++] = HEX[(b >> 4) & 0x0F];
            dst[out++] = HEX[b & 0x0F];
        }
    }

    dst[out] = '\0';
    return out;
}

