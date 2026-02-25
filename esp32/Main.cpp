#include <string_view>

#include "driver/gptimer.h"
#include "driver/ledc.h"

#include "soc/interrupts.h"
#include "soc/ledc_struct.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include "BIOS.h"
#include "DiskIO.h"
#include "Display.h"
#include "Storage.h"
#include "USB.h"

#include "ATAController.h"
#include "FloppyController.h"
#include "QEMUConfig.h"
#include "Scancode.h"
#include "System.h"
#include "VGACard.h"

gptimer_handle_t sysTimer = nullptr;
volatile uint32_t timerHigh = 0;

static System sys;

static ATAController ataPrimary(sys);
static FloppyController fdc(sys);
static QEMUConfig qemuCfg(sys);
static VGACard vga(sys);

static FileATAIO ataPrimaryIO;
static FileFloppyIO floppyIO;

// called from "drivers"
void display_draw_line(void *, int line, uint16_t *buf)
{
    // may need to be more careful here as this is coming from an interrupt...
    vga.drawScanline(line, reinterpret_cast<uint8_t *>(buf));
}

void update_key_state(ATScancode code, bool state)
{
    sys.getChipset().sendKey(code, state);
}

void update_mouse_state(int8_t x, int8_t y, bool left, bool right)
{
    auto &chipset = sys.getChipset();
    chipset.addMouseMotion(x, y);
    chipset.setMouseButton(0, left);
    chipset.setMouseButton(1, right);
    chipset.syncMouse();
}

static bool readConfigFile()
{
    char buf[100];

    FIL *configFile = new FIL;

    FRESULT res = f_open(configFile, "config.txt", FA_READ | FA_OPEN_EXISTING);

    if(res != FR_OK)
    {
        printf("Failed to open config file! %i\n", res);
        delete configFile;
        return false;
    }

    size_t off = 0;
    UINT read;

    while(!f_eof(configFile))
    {
        // get line
        for(off = 0; off < sizeof(buf) - 1; off++)
        {
            if(f_read(configFile, buf + off, 1, &read) != FR_OK || !read)
                break;

            if(buf[off] == '\n')
                break;
        }
        buf[off] = 0;

        // parse key=value
        std::string_view line(buf);

        auto equalsPos = line.find_first_of('=');

        if(equalsPos == std::string_view::npos)
        {
            printf("invalid config line %s\n", buf);
            continue;
        }

        auto key = line.substr(0, equalsPos);
        auto value = line.substr(equalsPos + 1);

        // ata drive (yes, 0-9 is a little optimistic)
        if(key.compare(0, 3, "ata") == 0 && key.length() == 4 && key[3] >= '0' && key[3] <= '9')
        {
            int index = key[3] - '0';
            // TODO: secondary controller?
            // using value as a c string is fine as it's the end of the original string
            ataPrimaryIO.openDisk(index, value.data());
            sys.getChipset().setFixedDiskPresent(index, ataPrimaryIO.getNumSectors(index) && !ataPrimaryIO.isATAPI(index));
        }
        else if(key.compare(0, 11, "ata-sectors") == 0 && key.length() == 12 && key[11] >= '0' && key[11] <= '9')
        {
            int index = key[11] - '0';
            int sectors = atoi(value.data());
            ataPrimary.overrideSectorsPerTrack(index, sectors);
        }
        else if(key.compare(0, 6, "floppy") == 0 && key.length() == 7 && key[6] >= '0' && key[6] <= '9')
        {
            int index = key[6] - '0';
            floppyIO.openDisk(index, value.data());
        }
        else
            printf("unhandled config line %s\n", buf);
    }

    f_close(configFile);
    delete configFile;

    return true;
}

static void vgaResolutionCallback(int w, int h)
{
    set_display_size(w, h);
}

#ifdef LEDC_TIMER
static void IRAM_ATTR ledc_overflow_intr(void *arg)
{
    LEDC.int_clr.timer0_ovf_int_clr = 1;
    timerHigh += 1 << 20;
}

static void initLEDCTimer()
{
    ledc_timer_config_t ledcConfig = {};
    ledcConfig.speed_mode = LEDC_LOW_SPEED_MODE;
    ledcConfig.duty_resolution = LEDC_TIMER_1_BIT;
    ledcConfig.timer_num = LEDC_TIMER_0;
    ledcConfig.freq_hz = System::getClockSpeed() / 2; // we're only using the timer so we need half the freq
    ledcConfig.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&ledcConfig));

    // hack timer resolution to max (making freq actually ~13.6Hz)
    LEDC.timer_group[0].timer[0].conf.duty_res = 20;
    LEDC.timer_group[0].timer[0].conf.para_up = 1;

    // overflow interrupt
    intr_handle_t handle;
    esp_intr_alloc(ETS_LEDC_INTR_SOURCE, ESP_INTR_FLAG_IRAM, ledc_overflow_intr, nullptr, &handle);
    LEDC.int_ena.timer0_ovf_int_ena = 1;
}
#endif

static void runEmulator(void * arg)
{
#ifdef LEDC_TIMER
    initLEDCTimer();
#endif

    while(true)
    {
        for(int i = 0; i < 100; i++)
        {
            sys.getCPU().run(10);
            sys.getChipset().updateForDisplay();
        }
        vTaskDelay(1); // hmm
    }
}

extern "C" void app_main()
{
#ifndef LEDC_TIMER
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
#endif

    // hw init
    init_display();
    init_storage();
    init_usb();

    // emulator init
    auto ramSize = 16 * 1024 * 1024; // can go up to 16 (core limit)
    auto ram = new uint8_t[ramSize];
    sys.addMemory(0, ramSize, ram);
    sys.getChipset().setTotalMemory(ramSize);

    // load BIOS
    auto bios = _binary_bios_bin_start;
    auto biosSize = _binary_bios_bin_end - _binary_bios_bin_start;
    auto biosBase = 0x100000 - biosSize;
    memcpy(ram + biosBase, bios, biosBase);

    qemuCfg.setVGABIOS(reinterpret_cast<const uint8_t *>(_binary_vgabios_bin_start));

    vga.setResolutionChangeCallback(vgaResolutionCallback);
    vga.setTextWidthHack(true); 

    // disk setup
    ataPrimary.setIOInterface(&ataPrimaryIO);
    fdc.setIOInterface(&floppyIO);

    if(!readConfigFile())
    {
        // load a default image
        ataPrimaryIO.openDisk(0, "hd0.img");
        sys.getChipset().setFixedDiskPresent(0, ataPrimaryIO.getNumSectors(0) && !ataPrimaryIO.isATAPI(0));
    }

    sys.reset();

    xTaskCreatePinnedToCore(runEmulator, "emu_cpu", 4096, xTaskGetCurrentTaskHandle(), 1, nullptr, 1);

    while(true)
    {
        vTaskDelay(1);
    }
}