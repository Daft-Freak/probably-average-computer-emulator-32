#include <forward_list>

#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/timer.h"
#include "hardware/riscv_platform_timer.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "tusb.h"

#include "fatfs/ff.h"

#include "config.h"
#include "psram.h"

#include "Audio.h"
#include "BIOS.h"
//#include "DiskIO.h"
#include "Display.h"


#include "ATAController.h"
#include "FloppyController.h"
#include "QEMUConfig.h"
#include "Scancode.h"
#include "System.h"
#include "VGACard.h"

static FATFS fs;

static System sys;

static FloppyController fdc(sys);

static QEMUConfig qemuCfg(sys);

static VGACard vga(sys);

//static FileFixedIO fixedIO;

static uint32_t emu_time = 0, real_time = 0, sync_time = 0;

static void speakerCallback(int8_t sample)
{
    int16_t sample16 = sample << 4;
    audio_queue_sample(sample16);
}

void update_key_state(ATScancode code, bool state)
{
    sys.getChipset().sendKey(code, state);
}

void update_mouse_state(int8_t x, int8_t y, bool left, bool right)
{
    auto chipset = sys.getChipset();
    chipset.addMouseMotion(x, y);
    chipset.setMouseButton(0, left);
    chipset.setMouseButton(1, right);
    chipset.syncMouse();
}

static void runEmulator(absolute_time_t &time)
{
    auto now = get_absolute_time();
    auto elapsed = absolute_time_diff_us(time, now) / 1000;

    if(elapsed)
    {
        if(elapsed > 10)
            elapsed = 10;

        auto start = get_absolute_time();

        sys.getCPU().run(elapsed);
        time = delayed_by_ms(time, elapsed);

        sys.getChipset().updateForDisplay();

        // get "real" time taken
        auto update_time = absolute_time_diff_us(start, get_absolute_time());

        emu_time += elapsed * 1000;
        real_time += update_time;

        // every 10s calculate speed
        if(emu_time >= 10000000) {
            int speed = uint64_t(emu_time) * 1000 / real_time;
            printf("speed %i.%i%% (%ims in %ims, sync %ims)\n", speed / 10, speed % 10, emu_time / 1000, real_time / 1000, sync_time / 1000);
            emu_time = real_time = sync_time = 0;
        }
    }
}

static void core1Main()
{
    // configure the riscv timer to count cycles so we can use it for the "system" clock
    // (which is really just the PIT)
    riscv_timer_set_fullspeed(true);
    riscv_timer_set_enabled(true);

    auto time = get_absolute_time();
    while(true)
        runEmulator(time);
}

int main()
{
    set_sys_clock_khz(250000, false);

    tusb_init();

    stdio_init_all();

    init_display();
    set_display_size(320, 200);

    size_t psramSize = psram_init(PSRAM_CS_PIN);

    printf("detected %i bytes PSRAM\n", psramSize);

    // init storage/filesystem
    auto res = f_mount(&fs, "", 1);

    if(res != FR_OK)
    {
        printf("Failed to mount filesystem! (%i)\n", res);
        while(true);
    }

    init_audio();

    // emulator init

    auto psram = reinterpret_cast<uint8_t *>(PSRAM_LOCATION);
    sys.addMemory(0, 8 * 1024 * 1024, psram);

    //sys.setSpeakerAudioCallback(speakerCallback);

    auto bios = _binary_bios_bin_start;
    auto biosSize = _binary_bios_bin_end - _binary_bios_bin_start;
    auto biosBase = 0x100000 - biosSize;
    memcpy(psram + biosBase, bios, biosBase);

    qemuCfg.setVGABIOS(reinterpret_cast<const uint8_t *>(_binary_vgabios_bin_start));

    sys.reset();

    multicore_launch_core1(core1Main);

    while(true)
    {
        tuh_task();
    }

    return 0;
}