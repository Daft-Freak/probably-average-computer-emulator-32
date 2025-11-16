#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/ppa.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

#include "Display.h"
#include "config.h"

static esp_lcd_panel_io_handle_t io_handle = nullptr;
static esp_lcd_panel_handle_t panel_handle = nullptr;

static uint16_t *line_buffer; // rgb565

static uint16_t temp_scale_buffer[720 * 2];

static unsigned cur_copy_line = 0;

static bool backlight_enabled = false;
static bool render_needed = true;

static inline void scale_line(uint16_t *out, int line, unsigned w)
{
    display_draw_line(nullptr, line * 2, temp_scale_buffer);
    display_draw_line(nullptr, line * 2 + 1, temp_scale_buffer + 720);

    auto in0 = temp_scale_buffer;
    auto in1 = temp_scale_buffer + 720;

    for(unsigned i = 0; i < w; i++)
    {
        uint32_t col0 = *in0++;
        uint32_t col1 = *in0++;
        uint32_t col2 = *in1++;
        uint32_t col3 = *in1++;

        col0 = (col0 | col0 << 16) & 0x07E0F81F;
        col1 = (col1 | col1 << 16) & 0x07E0F81F;
        col2 = (col2 | col2 << 16) & 0x07E0F81F;
        col3 = (col3 | col3 << 16) & 0x07E0F81F;

        uint32_t col = ((col0 + col1 + col2 + col3) >> 2) & 0x07E0F81F;

        *out++ = col | col >> 16;
    }
}

static void *alloc_display_buffer(size_t size)
{
  // this depends on the bus...
#ifdef LCD_I80
    return esp_lcd_i80_alloc_draw_buffer(io_handle, size, 0);
#else
    return nullptr;
#endif
}

static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    auto task = (TaskHandle_t)user_ctx;

    if(++cur_copy_line == DISPLAY_HEIGHT)
    {
#ifdef LCD_BACKLIGHT_PIN
        // enable backlight
        if(!backlight_enabled) {
            gpio_set_level(gpio_num_t(LCD_BACKLIGHT_PIN), 1);
            backlight_enabled = true;
        }
#endif

        render_needed = true;
    }

    // wake display task
    BaseType_t hp_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(task, &hp_task_woken);

    return hp_task_woken;
}

static void display_task(void *arg)
{
    // register callbacks
    esp_lcd_panel_io_callbacks_t io_callbacks = {};
    io_callbacks.on_color_trans_done = on_color_trans_done;

    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &io_callbacks, xTaskGetCurrentTaskHandle()));

    while(true)
    {
        if(render_needed)
        {
            scale_line(line_buffer, 0, DISPLAY_WIDTH);
            esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, DISPLAY_WIDTH, 1, line_buffer);
            cur_copy_line = 0;

            render_needed = false;
        }
        else if(cur_copy_line < DISPLAY_HEIGHT)
        {
            scale_line(line_buffer, cur_copy_line, DISPLAY_WIDTH);
            esp_lcd_panel_draw_bitmap(panel_handle, 0, cur_copy_line, DISPLAY_WIDTH, cur_copy_line + 1, line_buffer);
        }

        // wait for next completion
        xTaskNotifyWait(0, 0, nullptr, portMAX_DELAY);
    }
}

void init_display()
{
#ifdef LCD_BACKLIGHT_PIN
    // backlight
    gpio_config_t backlight_gpio_config = {};
    backlight_gpio_config.mode = GPIO_MODE_OUTPUT;
    backlight_gpio_config.pin_bit_mask = 1ULL << LCD_BACKLIGHT_PIN;

    ESP_ERROR_CHECK(gpio_config(&backlight_gpio_config));
    gpio_set_level(gpio_num_t(LCD_BACKLIGHT_PIN), 0);
#endif

#ifdef LCD_I80
    // init "I80" bus (8-bit)
    esp_lcd_i80_bus_handle_t i80_bus = nullptr;

    esp_lcd_i80_bus_config_t bus_config = {};

    bus_config.clk_src = LCD_CLK_SRC_DEFAULT;
    bus_config.dc_gpio_num = LCD_DC_PIN;
    bus_config.wr_gpio_num = LCD_WR_PIN;
    bus_config.data_gpio_nums[0] = LCD_DATA0_PIN;
    bus_config.data_gpio_nums[1] = LCD_DATA1_PIN;
    bus_config.data_gpio_nums[2] = LCD_DATA2_PIN;
    bus_config.data_gpio_nums[3] = LCD_DATA3_PIN;
    bus_config.data_gpio_nums[4] = LCD_DATA4_PIN;
    bus_config.data_gpio_nums[5] = LCD_DATA5_PIN;
    bus_config.data_gpio_nums[6] = LCD_DATA6_PIN;
    bus_config.data_gpio_nums[7] = LCD_DATA7_PIN;

    bus_config.bus_width = 8;
    bus_config.max_transfer_bytes = DISPLAY_WIDTH * sizeof(uint16_t);
    bus_config.dma_burst_size = 64;

    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));

    // init panel io
    esp_lcd_panel_io_i80_config_t io_config = {};
    io_config.cs_gpio_num = LCD_CS_PIN,
    io_config.pclk_hz = LCD_CLOCK,
    io_config.trans_queue_depth = 10,

    io_config.dc_levels.dc_idle_level = 0,
    io_config.dc_levels.dc_cmd_level = 0,
    io_config.dc_levels.dc_dummy_level = 0,
    io_config.dc_levels.dc_data_level = 1,

    io_config.lcd_cmd_bits = 8,
    io_config.lcd_param_bits = 8,
    io_config.flags.swap_color_bytes = 1;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &io_handle));
#endif

    // init panel
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = LCD_RESET_PIN;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;

#ifdef LCD_ST7789
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);

    esp_lcd_panel_invert_color(panel_handle, false);
    esp_lcd_panel_set_gap(panel_handle, 0, 0);
    //esp_lcd_panel_swap_xy(panel_handle, true);
    //esp_lcd_panel_mirror(panel_handle, false, true);

    if(DISPLAY_WIDTH == 320 && DISPLAY_HEIGHT == 240)
    {
        esp_lcd_panel_io_tx_param(io_handle, 0xB2/*PORCTRL*/, (uint8_t[]) {
            0x0c, 0x0c, 0x00, 0x33, 0x33
        }, 5);

        esp_lcd_panel_io_tx_param(io_handle, 0xB7/*GCTRL*/, (uint8_t[]) {
            0x35
        }, 1);

        esp_lcd_panel_io_tx_param(io_handle, 0xBB/*VCOMS*/, (uint8_t[]) {
            0x1f
        }, 1);

        esp_lcd_panel_io_tx_param(io_handle, 0xC0/*LCMCTRL*/, (uint8_t[]) {
            0x1f
        }, 1);

        esp_lcd_panel_io_tx_param(io_handle, 0xC2/*VDVVRHEN*/, (uint8_t[]) {
            0x01
        }, 1);

        esp_lcd_panel_io_tx_param(io_handle, 0xC3/*VRHS*/, (uint8_t[]) {
            0x12
        }, 1);

        esp_lcd_panel_io_tx_param(io_handle, 0xC4/*VDVS*/, (uint8_t[]) {
            0x20
        }, 1);

        esp_lcd_panel_io_tx_param(io_handle, 0xD0/*PWCTRL1*/, (uint8_t[]) {
            0xa4, 0xa1
        }, 2);

        esp_lcd_panel_io_tx_param(io_handle, 0xD6/*???*/, (uint8_t[]) {
            0xa1
        }, 1);

        esp_lcd_panel_io_tx_param(io_handle, 0xE0/*PVGAMCTRL*/, (uint8_t[]) {
            0xD0, 0x08, 0x11, 0x08, 0x0C, 0x15, 0x39, 0x33, 0x50, 0x36, 0x13, 0x14, 0x29, 0x2D
        }, 14);

        esp_lcd_panel_io_tx_param(io_handle, 0xE1/*NVGAMCTRL*/, (uint8_t[]) {
            0xD0, 0x08, 0x10, 0x08, 0x06, 0x06, 0x39, 0x44, 0x51, 0x0B, 0x16, 0x14, 0x2F, 0x31
        }, 14);
    }
#endif

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    // alloc buffers
    // TODO: larger?
    line_buffer = (uint16_t *)alloc_display_buffer(DISPLAY_WIDTH * 2);

    xTaskCreate(display_task, "display", 2048, nullptr, 5, nullptr);
}