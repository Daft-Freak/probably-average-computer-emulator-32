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

static uint16_t temp_scale_buffer[720 * 5];

static unsigned cur_copy_line = 0;
static unsigned copy_out_step = 1;

static bool backlight_enabled = false;
static bool render_needed = true;

static int new_mode_h = 480;

#if SOC_PPA_SUPPORTED
static ppa_client_handle_t ppa_client = nullptr;
#endif

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

    if(cur_copy_line * copy_out_step == DISPLAY_HEIGHT)
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

#if SOC_PPA_SUPPORTED
static bool on_ppa_trans_done(ppa_client_handle_t ppa_client, ppa_event_data_t *event_data, void *user_data) {
  esp_lcd_panel_draw_bitmap(panel_handle, 0, cur_copy_line * copy_out_step, DISPLAY_WIDTH, (cur_copy_line + 1) * copy_out_step, line_buffer);
  cur_copy_line++;
  return false;
}
#endif

static void display_task(void *arg)
{
    // register callbacks
    esp_lcd_panel_io_callbacks_t io_callbacks = {};
    io_callbacks.on_color_trans_done = on_color_trans_done;

    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &io_callbacks, xTaskGetCurrentTaskHandle()));

    int in_lines = 2;

#if SOC_PPA_SUPPORTED
    int cur_mode_w = 640; // TODO: remove text mode hack and use width from vga emu?
    int cur_mode_h = 480;

    int out_lines = 1;
#endif

    while(true)
    {
        if(render_needed)
        {
            cur_copy_line = 0;
            render_needed = false;

#if SOC_PPA_SUPPORTED
            // update scale
            if(new_mode_h != cur_mode_h)
            {
                cur_mode_h = new_mode_h;

                if(DISPLAY_HEIGHT == 240)
                {
                    if(new_mode_h == 480)
                    {
                        // 480:240 == 2:1 
                        in_lines = 2;
                        out_lines = 1;
                    }
                    else if(new_mode_h == 400)
                    {
                        // 400:240 == 5:3
                        in_lines = 5;
                        out_lines = 3;
                    }
                }
                else if(DISPLAY_HEIGHT == 320)
                {
                    if(new_mode_h == 480)
                    {
                        // 480:320 == 3:2
                        in_lines = 3;
                        out_lines = 2;
                    }
                    else if(new_mode_h == 400)
                    {
                        // 400:320 == 5:4
                        in_lines = 5;
                        out_lines = 4;
                    }
                }

                copy_out_step = out_lines;
            }
#endif
        }

        if(cur_copy_line < DISPLAY_HEIGHT)
        {
            for(int i = 0; i < in_lines; i++)
                display_draw_line(nullptr, cur_copy_line * in_lines + i, temp_scale_buffer + i * 720);

#if SOC_PPA_SUPPORTED
            // use PPA to scale
            ppa_srm_oper_config_t copy_config = {};

            copy_config.in.buffer = temp_scale_buffer;
            copy_config.in.pic_w = 720;
            copy_config.in.block_w = cur_mode_w;
            copy_config.in.pic_h = copy_config.in.block_h = in_lines;
            copy_config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

            copy_config.out.buffer = line_buffer;
            copy_config.out.buffer_size = DISPLAY_WIDTH * 2 * out_lines;
            copy_config.out.pic_w = DISPLAY_WIDTH;
            copy_config.out.pic_h = out_lines;
            copy_config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

            copy_config.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
            // scale is only accurate to 1/16th
            copy_config.scale_x = float(DISPLAY_WIDTH) / cur_mode_w;
            copy_config.scale_y = float(out_lines) / in_lines + (1.0f / 32.0f);
            copy_config.mode = PPA_TRANS_MODE_NON_BLOCKING;

            ppa_do_scale_rotate_mirror(ppa_client, &copy_config);
#else
            // fixed 2:1 scale
            auto in0 = temp_scale_buffer;
            auto in1 = temp_scale_buffer + 720;
            auto out = line_buffer;

            for(unsigned i = 0; i < DISPLAY_WIDTH; i++)
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

            esp_lcd_panel_draw_bitmap(panel_handle, 0, cur_copy_line, DISPLAY_WIDTH, cur_copy_line + 1, line_buffer);
            cur_copy_line++;
#endif
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
    bus_config.max_transfer_bytes = DISPLAY_WIDTH * 4 * sizeof(uint16_t);
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

#ifdef LCD_ILI9488
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);

    esp_lcd_panel_invert_color(panel_handle, false);
    esp_lcd_panel_set_gap(panel_handle, 0, 0);
    esp_lcd_panel_swap_xy(panel_handle, true);
    esp_lcd_panel_mirror(panel_handle, true, true);

    if(DISPLAY_WIDTH == 480 && DISPLAY_HEIGHT == 320)
    {
        // TODO: I don't even know if this is correct for the display I have.
        esp_lcd_panel_io_tx_param(io_handle, 0xC0/*PWCTRL1*/, (uint8_t[]) {
            0x17, 0x12
        }, 2);

        esp_lcd_panel_io_tx_param(io_handle, 0xC1/*PWCTRL2*/, (uint8_t[]) {
            0x41
        }, 1);

        esp_lcd_panel_io_tx_param(io_handle, 0xC5/*VMCTRL*/, (uint8_t[]) {
            0x00, 0x12, 0x80
        }, 3);

        esp_lcd_panel_io_tx_param(io_handle, 0xB1/*FRMCTR1*/, (uint8_t[]) {
            0xA0
        }, 1);

        esp_lcd_panel_io_tx_param(io_handle, 0xB7/*ETMOD*/, (uint8_t[]) {
            0x86
        }, 1);

        esp_lcd_panel_io_tx_param(io_handle, 0xE0/*PGAMCTRL*/, (uint8_t[]) {
            0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78, 0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F
        }, 15);

        esp_lcd_panel_io_tx_param(io_handle, 0xE1/*NGAMCTRL*/, (uint8_t[]) {
            0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F
        }, 15);
    }

#elif defined(LCD_ST7789)
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
    line_buffer = (uint16_t *)alloc_display_buffer(DISPLAY_WIDTH * 4 * 2);

  // pixel-processing accelerator
#if SOC_PPA_SUPPORTED
    ppa_client_config_t ppa_config = {};
    ppa_config.oper_type = PPA_OPERATION_SRM;
    ppa_config.max_pending_trans_num = 1;
    ppa_config.data_burst_length = PPA_DATA_BURST_LENGTH_128;
    ESP_ERROR_CHECK(ppa_register_client(&ppa_config, &ppa_client));

    ppa_event_callbacks_t ppa_callbacks = {};
    ppa_callbacks.on_trans_done = on_ppa_trans_done;
    ESP_ERROR_CHECK(ppa_client_register_event_callbacks(ppa_client, &ppa_callbacks));
#endif

    xTaskCreate(display_task, "display", 2048, nullptr, 5, nullptr);
}

void set_display_size(int w, int h)
{
    new_mode_h = h;
}