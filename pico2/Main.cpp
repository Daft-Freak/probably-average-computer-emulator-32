#include <forward_list>
#include <string_view>

#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/timer.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "tusb.h"

#include "fatfs/ff.h"

#include "config.h"
#include "psram.h"

#include "clock.pio.h"

#include "Audio.h"
#include "BIOS.h"
#include "DiskIO.h"
#include "Display.h"


#include "ATAController.h"
#include "FloppyController.h"
#include "QEMUConfig.h"
#include "Scancode.h"
#include "System.h"
#include "VGACard.h"

static FATFS fs;

static System sys;

static ATAController ataPrimary(sys);
static FloppyController fdc(sys);
static QEMUConfig qemuCfg(sys);
static VGACard vga(sys);

static FileATAIO ataPrimaryIO;
static FileFloppyIO floppyIO;

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
    auto &chipset = sys.getChipset();
    chipset.addMouseMotion(x, y);
    chipset.setMouseButton(0, left);
    chipset.setMouseButton(1, right);
    chipset.syncMouse();
}

// the first argument serves no purpose other than making this function not shuffle regs around
void __not_in_flash_func(display_draw_line)(void *, int line, uint16_t *buf)
{
    // may need to be more careful here as this is coming from an interrupt...
    vga.drawScanline(line, reinterpret_cast<uint8_t *>(buf));
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

static void core1FIFOHandler()
{
    switch(multicore_fifo_pop_blocking())
    {
        case 2: // ATA IO
            ataPrimaryIO.ioComplete();
    }

    multicore_fifo_clear_irq();
}

static void core1Main()
{
    // configure clock PIO program
    int offset = pio_add_program(pio1, &clock_program);
    auto config = clock_program_get_default_config(offset);

    // 3 cycles for program
    float clkdiv = float(clock_get_hz(clk_sys)) / (System::getClockSpeed() * 3);
    sm_config_set_clkdiv(&config, clkdiv);

    pio_sm_init(pio1, 0, offset, &config);
    pio_sm_set_enabled(pio1, 0, true);

    // setup FIFO irq
    multicore_fifo_clear_irq();
    irq_set_exclusive_handler(SIO_FIFO_IRQ_NUM(1), core1FIFOHandler);
    irq_set_enabled(SIO_FIFO_IRQ_NUM(1), true);

    // run
    auto time = get_absolute_time();
    while(true)
        runEmulator(time);
}

static bool readConfigFile()
{
    char buf[100];

    FIL configFile;

    if(f_open(&configFile, "config.txt", FA_READ | FA_OPEN_EXISTING) != FR_OK)
        return false;

    size_t off = 0;
    UINT read;

    while(!f_eof(&configFile))
    {
        // get line
        for(off = 0; off < sizeof(buf) - 1; off++)
        {
            if(f_read(&configFile, buf + off, 1, &read) != FR_OK || !read)
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
        else if(key.compare(0, 6, "floppy") == 0 && key.length() == 7 && key[6] >= '0' && key[6] <= '9')
        {
            int index = key[6] - '0';
            floppyIO.openDisk(index, value.data());
        }
        else
            printf("unhandled config line %s\n", buf);
    }

    f_close(&configFile);

    return true;
}

int main()
{
    set_sys_clock_khz(250000, false);

    tusb_init();

    stdio_init_all();

    // before we init anything, reserve PIO1 SM0 for the clock program
    pio_claim_sm_mask(pio1, 1 << 0);

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

    // FIXME: mode changes
    set_display_size(640, 480);
    update_display();

    multicore_launch_core1(core1Main);

    while(true)
    {
        // TODO: callback from VGA? kinda hard to know when a mode change is done
        auto [width, height] = vga.getOutputResolution();
        set_display_size(width, height);
        update_display();

        // check fifo for any commands from the emulator core
        uint32_t data;
        if(multicore_fifo_pop_timeout_us(1000, &data))
        {
            switch(data)
            {
                case 2: // ATA IO
                    ataPrimaryIO.doCore0IO();
                    break;
            }
        }

        tuh_task();
    }

    return 0;
}