#include "driver/gptimer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include "BIOS.h"
#include "Display.h"

#include "ATAController.h"
#include "FloppyController.h"
#include "QEMUConfig.h"
#include "Scancode.h"
#include "System.h"
#include "VGACard.h"

gptimer_handle_t sysTimer = nullptr;

static System sys;

static ATAController ataPrimary(sys);
static FloppyController fdc(sys);
static QEMUConfig qemuCfg(sys);
static VGACard vga(sys);

// static FileATAIO ataPrimaryIO;
// static FileFloppyIO floppyIO;

void display_draw_line(void *, int line, uint16_t *buf)
{
    // may need to be more careful here as this is coming from an interrupt...
    vga.drawScanline(line, reinterpret_cast<uint8_t *>(buf));
}

static void runEmulator(void * arg)
{
    while(true)
    {
        sys.getCPU().run(10);
        sys.getChipset().updateForDisplay();
        vTaskDelay(1); // hmm
    }
}

extern "C" void app_main()
{
    // create and start timer
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,                // count up
        .resolution_hz = System::getClockSpeed() / 4, // at emulated system freq (/4)
        .intr_priority = 0,
        .flags = {}
    };
    // on ESP32-P4 this gets a frequency of 3.636363MHz instead of 3.579545 (~14.54 instead of ~14.31)

    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &sysTimer));
    ESP_ERROR_CHECK(gptimer_enable(sysTimer));
    ESP_ERROR_CHECK(gptimer_start(sysTimer));

    // display/fs init
    init_display();

    // emulator init
    auto ramSize = 8 * 1024 * 1024; // can go up to 16 (core limit)
    auto ram = new uint8_t[ramSize];
    sys.addMemory(0, ramSize, ram);

    // load BIOS
    auto bios = _binary_bios_bin_start;
    auto biosSize = _binary_bios_bin_end - _binary_bios_bin_start;
    auto biosBase = 0x100000 - biosSize;
    memcpy(ram + biosBase, bios, biosBase);

    qemuCfg.setVGABIOS(reinterpret_cast<const uint8_t *>(_binary_vgabios_bin_start));

    sys.reset();

    xTaskCreatePinnedToCore(runEmulator, "emu_cpu", 4096, xTaskGetCurrentTaskHandle(), 1, nullptr, 1);

    while(true)
    {
        vTaskDelay(1);
    }
}