#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "BIOS.h"

#include "ATAController.h"
#include "FloppyController.h"
#include "QEMUConfig.h"
#include "Scancode.h"
#include "System.h"
#include "VGACard.h"

static System sys;

static ATAController ataPrimary(sys);
static FloppyController fdc(sys);
static QEMUConfig qemuCfg(sys);
static VGACard vga(sys);

// static FileATAIO ataPrimaryIO;
// static FileFloppyIO floppyIO;

static void runEmulator()
{
    sys.getCPU().run(10);
    sys.getChipset().updateForDisplay();
}

extern "C" void app_main()
{
    // display/fs init

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

    while(true)
    {
        runEmulator();
        vTaskDelay(1); // let idle task run
    }
}