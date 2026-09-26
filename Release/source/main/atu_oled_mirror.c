#include "atu_oled_mirror.h"

#include <string.h>

#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ATU_OLED_MIRROR";

#define ATU_OLED_I2C_PORT                I2C_NUM_0
#define ATU_OLED_I2C_SDA_GPIO            21
#define ATU_OLED_I2C_SCL_GPIO            22
#define ATU_OLED_I2C_ADDR                0x3C  // PIC/N7DDC EEPROM value 8-bit 0x78
#define ATU_OLED_I2C_RX_BUF_LEN          1024

#define ATU_OLED_N7DDC_EEPROM_TYPE_NORMAL    2
#define ATU_OLED_N7DDC_EEPROM_TYPE_INVERTED  3
#define ATU_OLED_FIXED_EEPROM_TYPE           ATU_OLED_N7DDC_EEPROM_TYPE_INVERTED

#define ATU_OLED_RX_CHUNK_SIZE           128
#define ATU_OLED_BURST_BUFFER_SIZE       4096
#define ATU_OLED_BURST_TIMEOUT_MS        30
#define ATU_OLED_READ_TIMEOUT_MS         10
#define ATU_OLED_RESYNC_QUIET_MS         120

#define ATU_OLED_PAGES                   (ATU_OLED_HEIGHT / 8)
#define ATU_OLED_FIXED_SEG_REMAP         true
#define ATU_OLED_FIXED_COM_SCAN_REMAP    true

typedef enum {
    OLED_STREAM_COMMAND = 0,
    OLED_STREAM_DATA
} oled_stream_t;

typedef enum {
    OLED_ADDR_MODE_HORIZONTAL = 0,
    OLED_ADDR_MODE_VERTICAL = 1,
    OLED_ADDR_MODE_PAGE = 2
} oled_addr_mode_t;

typedef struct {
    oled_stream_t stream;
    oled_addr_mode_t addr_mode;
    uint8_t page;
    uint8_t col;
    uint8_t page_start;
    uint8_t page_end;
    uint8_t col_start;
    uint8_t col_end;
    bool seg_remap;
    bool com_scan_remap;

    uint8_t pending_cmd;
    uint8_t pending_needed;
    uint8_t pending_len;
    uint8_t pending_args[2];
} oled_parser_t;

typedef struct {
    uint16_t gddram_writes;
} burst_parse_stats_t;

static oled_parser_t s_parser = {
    .stream = OLED_STREAM_COMMAND,
    .addr_mode = OLED_ADDR_MODE_PAGE,
    .page = 0,
    .col = 0,
    .page_start = 0,
    .page_end = (uint8_t)(ATU_OLED_PAGES - 1),
    .col_start = 0,
    .col_end = (uint8_t)(ATU_OLED_WIDTH - 1),
    // Fixed project profile: SSD1306 128x32, ATU/N7DDC EEPROM cell 01 type 03.
    // Type 03 uses A1 + C8 orientation on the physical OLED.
    .seg_remap = ATU_OLED_FIXED_SEG_REMAP,
    .com_scan_remap = ATU_OLED_FIXED_COM_SCAN_REMAP,
    .pending_cmd = 0,
    .pending_needed = 0,
    .pending_len = 0,
    .pending_args = {0, 0},
};

static uint8_t s_rx_chunk[ATU_OLED_RX_CHUNK_SIZE];
static uint8_t s_burst_buffer[ATU_OLED_BURST_BUFFER_SIZE];
static uint8_t s_gddram[ATU_OLED_PAGES][ATU_OLED_WIDTH];
static atu_oled_frame_t s_frame = {0};
static SemaphoreHandle_t s_frame_mutex = NULL;
static bool s_started = false;

static esp_err_t oled_ensure_mutex(void)
{
    if (s_frame_mutex == NULL) {
        s_frame_mutex = xSemaphoreCreateMutex();
        if (s_frame_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static esp_err_t atu_oled_i2c_slave_init(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_SLAVE,
        .sda_io_num = ATU_OLED_I2C_SDA_GPIO,
        .scl_io_num = ATU_OLED_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .slave = {
            .slave_addr = ATU_OLED_I2C_ADDR,
            .addr_10bit_en = 0,
        },
        .clk_flags = 0,
    };

    ESP_RETURN_ON_ERROR(i2c_param_config(ATU_OLED_I2C_PORT, &cfg), TAG, "i2c_param_config failed");
    return i2c_driver_install(
        ATU_OLED_I2C_PORT,
        I2C_MODE_SLAVE,
        ATU_OLED_I2C_RX_BUF_LEN,
        0,
        0
    );
}

static bool is_page_cmd(uint8_t b)
{
    return (b >= 0xB0) && (b <= 0xB7);
}

static bool is_col_low_cmd(uint8_t b)
{
    return (b & 0xF0) == 0x00;
}

static bool is_col_high_cmd(uint8_t b)
{
    return (b & 0xF0) == 0x10;
}

static bool looks_like_page_header(const uint8_t *burst, size_t i, size_t len)
{
    if ((i + 4U) >= len) {
        return false;
    }
    if (burst[i] != 0x00 || !is_page_cmd(burst[i + 1])) {
        return false;
    }
    if (is_col_low_cmd(burst[i + 2]) && is_col_high_cmd(burst[i + 3]) && (burst[i + 4] == 0x40)) {
        return true;
    }
    if (is_col_high_cmd(burst[i + 2]) && is_col_low_cmd(burst[i + 3]) && (burst[i + 4] == 0x40)) {
        return true;
    }
    return false;
}

static bool looks_like_page_payload_switch(const uint8_t *burst, size_t i)
{
    if ((burst[i] != 0x40) || (i < 3U)) {
        return false;
    }
    if (!is_page_cmd(burst[i - 3U])) {
        return false;
    }
    if (is_col_low_cmd(burst[i - 2U]) && is_col_high_cmd(burst[i - 1U])) {
        return true;
    }
    if (is_col_high_cmd(burst[i - 2U]) && is_col_low_cmd(burst[i - 1U])) {
        return true;
    }
    return false;
}

static bool is_control_byte(const uint8_t *burst, size_t i, size_t len)
{
    const uint8_t b = burst[i];
    if (b == 0x80) {
        return true;
    }
    if ((i == 0U) && ((b == 0x00) || (b == 0x40))) {
        return true;
    }
    if ((b == 0x00) && looks_like_page_header(burst, i, len)) {
        return true;
    }
    if ((b == 0x40) && ((i + 1U) < len) && looks_like_page_header(burst, i + 1U, len)) {
        return true;
    }
    if ((b == 0x40) && looks_like_page_payload_switch(burst, i)) {
        return true;
    }
    return false;
}

static void oled_parser_reset(oled_parser_t *parser)
{
    parser->stream = OLED_STREAM_COMMAND;
    parser->addr_mode = OLED_ADDR_MODE_PAGE;
    parser->page = 0;
    parser->col = 0;
    parser->page_start = 0;
    parser->page_end = (uint8_t)(ATU_OLED_PAGES - 1);
    parser->col_start = 0;
    parser->col_end = (uint8_t)(ATU_OLED_WIDTH - 1);
    // Keep orientation fixed to the ATU OLED type 03 inverted setup.
    parser->seg_remap = ATU_OLED_FIXED_SEG_REMAP;
    parser->com_scan_remap = ATU_OLED_FIXED_COM_SCAN_REMAP;
    parser->pending_cmd = 0;
    parser->pending_needed = 0;
    parser->pending_len = 0;
    parser->pending_args[0] = 0;
    parser->pending_args[1] = 0;
}

static void oled_advance_address(oled_parser_t *parser)
{
    if (parser->addr_mode == OLED_ADDR_MODE_PAGE) {
        parser->col = (uint8_t)((parser->col + 1U) & 0x7F);
        return;
    }

    if (parser->addr_mode == OLED_ADDR_MODE_HORIZONTAL) {
        if (parser->col < parser->col_end) {
            parser->col++;
        } else {
            parser->col = parser->col_start;
            if (parser->page < parser->page_end) {
                parser->page++;
            } else {
                parser->page = parser->page_start;
            }
        }
        return;
    }

    if (parser->page < parser->page_end) {
        parser->page++;
    } else {
        parser->page = parser->page_start;
        if (parser->col < parser->col_end) {
            parser->col++;
        } else {
            parser->col = parser->col_start;
        }
    }
}

static void oled_apply_command(oled_parser_t *parser, uint8_t cmd, const uint8_t *args)
{
    parser->seg_remap = ATU_OLED_FIXED_SEG_REMAP;
    parser->com_scan_remap = ATU_OLED_FIXED_COM_SCAN_REMAP;

    if ((cmd >= 0xB0) && (cmd <= 0xB7)) {
        parser->page = (uint8_t)(cmd - 0xB0);
        if (parser->page >= ATU_OLED_PAGES) {
            parser->page = (uint8_t)(ATU_OLED_PAGES - 1);
        }
        return;
    }

    if ((cmd & 0xF0) == 0x00) {
        parser->col = (uint8_t)((parser->col & 0xF0) | (cmd & 0x0F));
        return;
    }

    if ((cmd & 0xF0) == 0x10) {
        parser->col = (uint8_t)((parser->col & 0x0F) | ((cmd & 0x0F) << 4));
        parser->col &= 0x7F;
        return;
    }

    switch (cmd) {
        case 0x20:
            parser->addr_mode = (oled_addr_mode_t)(args[0] & 0x03);
            if (parser->addr_mode > OLED_ADDR_MODE_PAGE) {
                parser->addr_mode = OLED_ADDR_MODE_PAGE;
            }
            break;
        case 0x21:
            parser->col_start = (uint8_t)(args[0] & 0x7F);
            parser->col_end = (uint8_t)(args[1] & 0x7F);
            if (parser->col_end < parser->col_start) {
                parser->col_end = parser->col_start;
            }
            parser->col = parser->col_start;
            break;
        case 0x22:
            parser->page_start = (uint8_t)(args[0] & 0x07);
            parser->page_end = (uint8_t)(args[1] & 0x07);
            if (parser->page_start >= ATU_OLED_PAGES) {
                parser->page_start = (uint8_t)(ATU_OLED_PAGES - 1);
            }
            if (parser->page_end >= ATU_OLED_PAGES) {
                parser->page_end = (uint8_t)(ATU_OLED_PAGES - 1);
            }
            if (parser->page_end < parser->page_start) {
                parser->page_end = parser->page_start;
            }
            parser->page = parser->page_start;
            break;
        case 0xA0:
            ESP_LOGI(TAG, "SSD1306 orientation command 0xA0 received; keeping fixed inverted segment remap");
            break;
        case 0xA1:
            ESP_LOGI(TAG, "SSD1306 orientation command 0xA1 received; keeping fixed inverted segment remap");
            break;
        case 0xC0:
            ESP_LOGI(TAG, "SSD1306 orientation command 0xC0 received; keeping fixed inverted COM scan remap");
            break;
        case 0xC8:
            ESP_LOGI(TAG, "SSD1306 orientation command 0xC8 received; keeping fixed inverted COM scan remap");
            break;
        default:
            break;
    }

    parser->seg_remap = ATU_OLED_FIXED_SEG_REMAP;
    parser->com_scan_remap = ATU_OLED_FIXED_COM_SCAN_REMAP;
}

static void oled_handle_command_byte(oled_parser_t *parser, uint8_t byte)
{
    if (parser->pending_len < parser->pending_needed) {
        parser->pending_args[parser->pending_len++] = byte;
        if (parser->pending_len == parser->pending_needed) {
            oled_apply_command(parser, parser->pending_cmd, parser->pending_args);
            parser->pending_needed = 0;
            parser->pending_len = 0;
        }
        return;
    }

    parser->pending_cmd = byte;
    parser->pending_len = 0;

    switch (byte) {
        case 0x20:
        case 0x81:
        case 0x8D:
        case 0xA8:
        case 0xD3:
        case 0xD5:
        case 0xD9:
        case 0xDA:
            parser->pending_needed = 1;
            return;
        case 0x21:
        case 0x22:
            parser->pending_needed = 2;
            return;
        default:
            parser->pending_needed = 0;
            oled_apply_command(parser, byte, parser->pending_args);
            return;
    }
}

static void oled_write_data_byte(oled_parser_t *parser, uint8_t byte, burst_parse_stats_t *stats)
{
    if ((parser->page < ATU_OLED_PAGES) && (parser->col < ATU_OLED_WIDTH)) {
        s_gddram[parser->page][parser->col] = byte;
        stats->gddram_writes++;
    }
    oled_advance_address(parser);
}

static void parser_feed_burst(oled_parser_t *parser, const uint8_t *burst, size_t len, burst_parse_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));

    for (size_t i = 0; i < len; i++) {
        const uint8_t byte = burst[i];

        if (is_control_byte(burst, i, len)) {
            parser->stream = (byte == 0x40) ? OLED_STREAM_DATA : OLED_STREAM_COMMAND;
            continue;
        }

        if (parser->stream == OLED_STREAM_DATA) {
            oled_write_data_byte(parser, byte, stats);
        } else {
            oled_handle_command_byte(parser, byte);
        }
    }
}

static uint32_t pixel_hash(const uint8_t pixels[ATU_OLED_HEIGHT][ATU_OLED_WIDTH])
{
    uint32_t hash = 2166136261u;
    for (int y = 0; y < ATU_OLED_HEIGHT; y++) {
        for (int x = 0; x < ATU_OLED_WIDTH; x++) {
            hash ^= (pixels[y][x] ? 1U : 0U);
            hash *= 16777619u;
        }
    }
    return hash;
}

static void gddram_to_frame(const oled_parser_t *parser, atu_oled_frame_t *frame)
{
    memset(frame->pixels, 0, sizeof(frame->pixels));

    for (int page = 0; page < ATU_OLED_PAGES; page++) {
        for (int col = 0; col < ATU_OLED_WIDTH; col++) {
            const uint8_t cell = s_gddram[page][col];
            for (int bit = 0; bit < 8; bit++) {
                const int y_mem = (page * 8) + bit;
                if (y_mem >= ATU_OLED_HEIGHT) {
                    continue;
                }
                const int x_vis = parser->seg_remap ? (ATU_OLED_WIDTH - 1 - col) : col;
                const int y_vis = parser->com_scan_remap ? (ATU_OLED_HEIGHT - 1 - y_mem) : y_mem;
                frame->pixels[y_vis][x_vis] = (uint8_t)((cell >> bit) & 0x01);
            }
        }
    }

    frame->seg_remap = parser->seg_remap;
    frame->com_scan_remap = parser->com_scan_remap;
    frame->hash = pixel_hash(frame->pixels);
    frame->valid = true;
}

static esp_err_t oled_clear_and_publish_blank(void)
{
    ESP_RETURN_ON_ERROR(oled_ensure_mutex(), TAG, "OLED mirror mutex init failed");

    if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    memset(s_gddram, 0, sizeof(s_gddram));
    oled_parser_reset(&s_parser);
    gddram_to_frame(&s_parser, &s_frame);

    xSemaphoreGive(s_frame_mutex);
    return ESP_OK;
}

static void atu_oled_mirror_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Starting OLED mirror task (I2C slave addr=0x%02X, SDA=%d, SCL=%d)",
             ATU_OLED_I2C_ADDR, ATU_OLED_I2C_SDA_GPIO, ATU_OLED_I2C_SCL_GPIO);

    esp_err_t err = atu_oled_i2c_slave_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C slave init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    size_t burst_len = 0;
    TickType_t last_rx_tick = 0;
    TickType_t last_rx_any_tick = 0;
    bool burst_active = false;
    bool burst_overflowed = false;
    bool resync_pending = false;

    for (;;) {
        const int rx_len = i2c_slave_read_buffer(
            ATU_OLED_I2C_PORT,
            s_rx_chunk,
            sizeof(s_rx_chunk),
            pdMS_TO_TICKS(ATU_OLED_READ_TIMEOUT_MS)
        );
        const TickType_t now = xTaskGetTickCount();

        if (rx_len > 0) {
            const size_t available = ATU_OLED_BURST_BUFFER_SIZE - burst_len;
            const size_t copy_len = ((size_t)rx_len > available) ? available : (size_t)rx_len;
            if (copy_len > 0) {
                memcpy(&s_burst_buffer[burst_len], s_rx_chunk, copy_len);
                burst_len += copy_len;
            }
            if (copy_len < (size_t)rx_len) {
                burst_overflowed = true;
                resync_pending = true;
            }
            last_rx_tick = now;
            last_rx_any_tick = now;
            burst_active = true;
        }

        if (burst_active && ((now - last_rx_tick) >= pdMS_TO_TICKS(ATU_OLED_BURST_TIMEOUT_MS))) {
            if (!burst_overflowed) {
                burst_parse_stats_t stats;
                if (s_frame_mutex != NULL && xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                    parser_feed_burst(&s_parser, s_burst_buffer, burst_len, &stats);
                    if (stats.gddram_writes > 0) {
                        gddram_to_frame(&s_parser, &s_frame);
                    }
                    xSemaphoreGive(s_frame_mutex);
                }
            }
            burst_len = 0;
            burst_active = false;
            burst_overflowed = false;
        }

        if (resync_pending && !burst_active &&
            ((now - last_rx_any_tick) >= pdMS_TO_TICKS(ATU_OLED_RESYNC_QUIET_MS))) {
            ESP_LOGW(TAG, "I2C burst overflow/noise detected, forcing OLED mirror resync");
            esp_err_t reset_err = oled_clear_and_publish_blank();
            if (reset_err != ESP_OK) {
                ESP_LOGW(TAG, "OLED mirror resync failed: %s", esp_err_to_name(reset_err));
            }
            resync_pending = false;
        }
    }
}

esp_err_t atu_oled_mirror_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(oled_ensure_mutex(), TAG, "OLED mirror mutex init failed");

    BaseType_t ok = xTaskCreate(atu_oled_mirror_task, "atu_oled_mirror", 6144, NULL, 5, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    return ESP_OK;
}

esp_err_t atu_oled_mirror_reset(void)
{
    esp_err_t err = oled_clear_and_publish_blank();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "OLED mirror RAM reset: virtual GDDRAM cleared and parser restored to fixed 128x32 inverted profile");
    }
    return err;
}

bool atu_oled_mirror_get_frame(atu_oled_frame_t *out_frame)
{
    if (out_frame == NULL || s_frame_mutex == NULL) {
        return false;
    }

    bool ok = false;
    if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (s_frame.valid) {
            *out_frame = s_frame;
            ok = true;
        }
        xSemaphoreGive(s_frame_mutex);
    }
    return ok;
}
