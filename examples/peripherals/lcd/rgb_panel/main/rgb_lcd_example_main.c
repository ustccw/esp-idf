/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "lv_demos.h"

static const char *TAG = "example";

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your LCD spec //////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define EXAMPLE_LCD_PIXEL_CLOCK_HZ     (18 * 1000 * 1000)
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL  1
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL
// #define EXAMPLE_LCD_HSYC_BACK_PORCH    40
// #define EXAMPLE_LCD_HSYC_FRONT_PORCH   20
// #define EXAMPLE_LCD_HSYC_PLUS_WIDTH    1
// #define EXAMPLE_LCD_VSYC_BACK_PORCH    8
// #define EXAMPLE_LCD_VSYC_FRONT_PORCH   4
// #define EXAMPLE_LCD_VSYC_PLUS_WIDTH    1
#define EXAMPLE_LCD_HSYC_BACK_PORCH    40
#define EXAMPLE_LCD_HSYC_FRONT_PORCH   48
#define EXAMPLE_LCD_HSYC_PLUS_WIDTH    40
#define EXAMPLE_LCD_VSYC_BACK_PORCH    32
#define EXAMPLE_LCD_VSYC_FRONT_PORCH   13
#define EXAMPLE_LCD_VSYC_PLUS_WIDTH    23
#define EXAMPLE_LCD_PCLK_ACTIVE_NEG    true
#define EXAMPLE_PIN_NUM_BK_LIGHT       4
#define EXAMPLE_PIN_NUM_HSYNC          46
#define EXAMPLE_PIN_NUM_VSYNC          3
#define EXAMPLE_PIN_NUM_DE             0
#define EXAMPLE_PIN_NUM_PCLK           9
#define EXAMPLE_PIN_NUM_DATA0          14 // B0
#define EXAMPLE_PIN_NUM_DATA1          13 // B1
#define EXAMPLE_PIN_NUM_DATA2          12 // B2
#define EXAMPLE_PIN_NUM_DATA3          11 // B3
#define EXAMPLE_PIN_NUM_DATA4          10 // B4
#define EXAMPLE_PIN_NUM_DATA5          39 // G0
#define EXAMPLE_PIN_NUM_DATA6          38 // G1
#define EXAMPLE_PIN_NUM_DATA7          45 // G2
#define EXAMPLE_PIN_NUM_DATA8          48 // G3
#define EXAMPLE_PIN_NUM_DATA9          47 // G4
#define EXAMPLE_PIN_NUM_DATA10         21 // G5
#define EXAMPLE_PIN_NUM_DATA11         1  // R0
#define EXAMPLE_PIN_NUM_DATA12         2  // R1
#define EXAMPLE_PIN_NUM_DATA13         42 // R2
#define EXAMPLE_PIN_NUM_DATA14         41 // R3
#define EXAMPLE_PIN_NUM_DATA15         40 // R4
#define EXAMPLE_PIN_NUM_DISP_EN        -1

// The pixel number in horizontal and vertical
#define EXAMPLE_LCD_H_RES              800
#define EXAMPLE_LCD_V_RES              480

#define EXAMPLE_LVGL_TICK_PERIOD_MS    2
#define EXAMPLE_LVGL_BUFFER_MALLOC     MALLOC_CAP_INTERNAL

// we use two semaphores to sync the last area's flushing of lvgl and the RGB LCD vsync, to avoid potential tearing effect
#if CONFIG_EXAMPLE_AVOID_TEAR_WITH_SYNC_FLUSH
static SemaphoreHandle_t flush_end = NULL;
static SemaphoreHandle_t trans_ready = NULL;
#endif

extern void example_lvgl_demo_ui(lv_obj_t *scr);

static bool example_on_vsync_event(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *event_data, void *user_data)
{
    BaseType_t high_task_awoken = pdFALSE;
#if CONFIG_EXAMPLE_AVOID_TEAR_WITH_SYNC_FLUSH
    xSemaphoreGiveFromISR(flush_end, &high_task_awoken);
    xSemaphoreGiveFromISR(trans_ready, &high_task_awoken);
#endif
    return high_task_awoken == pdTRUE;
}

static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
#if CONFIG_EXAMPLE_AVOID_TEAR_WITH_SYNC_FLUSH_CRITICAL
    static int area_index = 0;
    if (area_index == 0) {
        xSemaphoreTake(trans_ready, portMAX_DELAY);
    }
    area_index++;
    if (lv_disp_flush_is_last(drv)) {
        area_index = 0;
    }
#endif

    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    // pass the draw buffer to the driver
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);

#if CONFIG_EXAMPLE_AVOID_TEAR_WITH_SYNC_FLUSH
    if (lv_disp_flush_is_last(drv)) {
#if !CONFIG_EXAMPLE_AVOID_TEAR_WITH_SYNC_FLUSH_CRITICAL
        xSemaphoreTake(flush_end, portMAX_DELAY);
#endif
        esp_lcd_rgb_panel_start_transmission(panel_handle);
    }
#endif

    lv_disp_flush_ready(drv);
}

static void example_increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

void app_main(void)
{
    static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
    static lv_disp_drv_t disp_drv;      // contains callback functions

#if CONFIG_EXAMPLE_AVOID_TEAR_WITH_SYNC_FLUSH
    ESP_LOGI(TAG, "Create semaphores");
    flush_end = xSemaphoreCreateBinary();
    assert(flush_end);
    xSemaphoreGive(flush_end);
    trans_ready = xSemaphoreCreateBinary();
    assert(trans_ready);
    xSemaphoreGive(trans_ready);
#endif

#if EXAMPLE_PIN_NUM_BK_LIGHT >= 0
    ESP_LOGI(TAG, "Turn off LCD backlight");
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << EXAMPLE_PIN_NUM_BK_LIGHT
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
#endif

    ESP_LOGI(TAG, "Install RGB LCD panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_rgb_panel_config_t panel_config = {
        .data_width = 16,               // RGB565 in parallel mode, thus 16bit in width
        .psram_trans_align = 64,
#if CONFIG_EXAMPLE_USE_BOUNCE_BUFFER
        .bounce_buffer_size_px = 10 * EXAMPLE_LCD_H_RES,
#endif
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .disp_gpio_num = EXAMPLE_PIN_NUM_DISP_EN,
        .pclk_gpio_num = EXAMPLE_PIN_NUM_PCLK,
        .vsync_gpio_num = EXAMPLE_PIN_NUM_VSYNC,
        .hsync_gpio_num = EXAMPLE_PIN_NUM_HSYNC,
        .de_gpio_num = EXAMPLE_PIN_NUM_DE,
        .data_gpio_nums = {
            EXAMPLE_PIN_NUM_DATA0,
            EXAMPLE_PIN_NUM_DATA1,
            EXAMPLE_PIN_NUM_DATA2,
            EXAMPLE_PIN_NUM_DATA3,
            EXAMPLE_PIN_NUM_DATA4,
            EXAMPLE_PIN_NUM_DATA5,
            EXAMPLE_PIN_NUM_DATA6,
            EXAMPLE_PIN_NUM_DATA7,
            EXAMPLE_PIN_NUM_DATA8,
            EXAMPLE_PIN_NUM_DATA9,
            EXAMPLE_PIN_NUM_DATA10,
            EXAMPLE_PIN_NUM_DATA11,
            EXAMPLE_PIN_NUM_DATA12,
            EXAMPLE_PIN_NUM_DATA13,
            EXAMPLE_PIN_NUM_DATA14,
            EXAMPLE_PIN_NUM_DATA15,
        },
        .timings = {
            .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,
            .h_res = EXAMPLE_LCD_H_RES,
            .v_res = EXAMPLE_LCD_V_RES,
            /* The following parameters should refer to LCD spec */
            .hsync_back_porch = EXAMPLE_LCD_HSYC_BACK_PORCH,
            .hsync_front_porch = EXAMPLE_LCD_HSYC_FRONT_PORCH,
            .hsync_pulse_width = EXAMPLE_LCD_HSYC_PLUS_WIDTH,
            .vsync_back_porch = EXAMPLE_LCD_VSYC_BACK_PORCH,
            .vsync_front_porch = EXAMPLE_LCD_VSYC_FRONT_PORCH,
            .vsync_pulse_width = EXAMPLE_LCD_VSYC_PLUS_WIDTH,
            .flags.pclk_active_neg = EXAMPLE_LCD_PCLK_ACTIVE_NEG,
        },
#if CONFIG_EXAMPLE_AVOID_TEAR_WITH_SYNC_FLUSH
        .flags.refresh_on_demand = 1,   // Mannually control refresh operation
#endif
        .flags.fb_in_psram = true,      // allocate frame buffer in PSRAM
#if CONFIG_EXAMPLE_DOUBLE_FB
        .flags.double_fb = true,        // allocate double frame buffer
#endif
    };
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));

    ESP_LOGI(TAG, "Register event callbacks");
    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_vsync = example_on_vsync_event,
    };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &cbs, &disp_drv));

    ESP_LOGI(TAG, "Initialize RGB LCD panel");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

#if EXAMPLE_PIN_NUM_BK_LIGHT >= 0
    ESP_LOGI(TAG, "Turn on LCD backlight");
    gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, EXAMPLE_LCD_BK_LIGHT_ON_LEVEL);
#endif

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    void *buf1 = NULL;
    void *buf2 = NULL;
#if CONFIG_EXAMPLE_DOUBLE_FB
    ESP_LOGI(TAG, "Use frame buffers as LVGL draw buffers");
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &buf1, &buf2));
    // initialize LVGL draw buffers
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES);
#else
    ESP_LOGI(TAG, "Allocate single LVGL draw buffer");
    buf1 = heap_caps_malloc(EXAMPLE_LCD_H_RES * 100 * sizeof(lv_color_t), EXAMPLE_LVGL_BUFFER_MALLOC);
    assert(buf1);
    (void)buf2;
    // initialize LVGL draw buffers
    lv_disp_draw_buf_init(&disp_buf, buf1, NULL, EXAMPLE_LCD_H_RES * 100);
#endif // CONFIG_EXAMPLE_DOUBLE_FB

    ESP_LOGI(TAG, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = EXAMPLE_LCD_H_RES;
    disp_drv.ver_res = EXAMPLE_LCD_V_RES;
    disp_drv.flush_cb = example_lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
#if CONFIG_EXAMPLE_DOUBLE_FB
    disp_drv.full_refresh = true; // the full_refresh mode can maintain the synchronization between the two frame buffers
#endif
    lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG, "Install LVGL tick timer");
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &example_increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

    ESP_LOGI(TAG, "Display LVGL Music Demo");
    lv_demo_music();

    while (1) {
        // raise the task priority of LVGL and/or reduce the handler period can improve the performance
        vTaskDelay(pdMS_TO_TICKS(10));
        // The task running lv_task_handler should have lower priority than that running `lv_tick_inc`
        lv_task_handler();
#if CONFIG_EXAMPLE_AVOID_TEAR_WITH_SYNC_FLUSH
#if CONFIG_EXAMPLE_AVOID_TEAR_WITH_SYNC_FLUSH_CRITICAL
    if (xSemaphoreTake(trans_ready, 0) == pdTRUE) {
        esp_lcd_rgb_panel_start_transmission(panel_handle);
    }
#else
    if (xSemaphoreTake(flush_end, 0) == pdTRUE) {
        if (xSemaphoreTake(trans_ready, 0) == pdTRUE) {
            esp_lcd_rgb_panel_start_transmission(panel_handle);
        }
        else {
            xSemaphoreGive(flush_end);
        }
    }
#endif
#endif
    }
}
