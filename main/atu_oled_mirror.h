#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ATU_OLED_WIDTH   128
#define ATU_OLED_HEIGHT  32

typedef struct {
    bool valid;
    bool seg_remap;
    bool com_scan_remap;
    uint32_t hash;
    uint8_t pixels[ATU_OLED_HEIGHT][ATU_OLED_WIDTH];
} atu_oled_frame_t;

esp_err_t atu_oled_mirror_start(void);
esp_err_t atu_oled_mirror_reset(void);
bool atu_oled_mirror_get_frame(atu_oled_frame_t *out_frame);

#ifdef __cplusplus
}
#endif
