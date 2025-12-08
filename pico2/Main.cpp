#include <cstdlib>
#include <forward_list>
#include <string_view>

#include "hardware/clocks.h"
#include "hardware/dma.h"
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

// need to include this after config.h
#ifdef PIO_USB_HOST
#include "pio_usb_configuration.h"
#endif

#include "clock.pio.h"

#include "Audio.h"
#include "BIOS.h"
#include "DiskIO.h"
#include "Display.h"

#include "ATAController.h"
#include "FloppyController.h"
#include "GamePort.h"
#include "QEMUConfig.h"
#include "Scancode.h"
#include "System.h"
#include "VGACard.h"

static FATFS fs;

static System sys;

static ATAController ataPrimary(sys);
static FloppyController fdc(sys);
static GamePort gamePort(sys);
static QEMUConfig qemuCfg(sys);
static VGACard vga(sys);

static FileATAIO ataPrimaryIO;
static FileFloppyIO floppyIO;

static int rtcSeconds = 0;

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

void update_gamepad_state(uint8_t axis[2], uint8_t hat, uint32_t buttons)
{
    // TODO: multiple gamepads/the other two axes
    // HACK: the values seem a bit low, so scale them up
    gamePort.setAxis(0, axis[0] / 255.0f * 3.0f);
    gamePort.setAxis(1, axis[1] / 255.0f * 3.0f);

    // TODO: button mapping
    for(int i = 0; i < 4; i++)
        gamePort.setButton(i, buttons & (1 << i));
}

// the first argument serves no purpose other than making this function not shuffle regs around
void __not_in_flash_func(display_draw_line)(void *, int line, uint16_t *buf)
{
    // may need to be more careful here as this is coming from an interrupt...
    vga.drawScanline(line, reinterpret_cast<uint8_t *>(buf));
}

static void vgaResolutionCallback(int w, int h)
{
    set_display_size(w, h);
}

static void core1FIFOHandler()
{
    switch(multicore_fifo_pop_blocking())
    {
        case 1: // floppy IO
            floppyIO.ioComplete();
            break;
        case 2: // ATA IO
            ataPrimaryIO.ioComplete();
            break;
    }

    multicore_fifo_clear_irq();
}

static bool rtcTimerCallback(repeating_timer *t)
{
    rtcSeconds++;
    return true;
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

    // setup timer for RTC
    repeating_timer timer;
    add_repeating_timer_ms(1000, rtcTimerCallback, nullptr, &timer);

    // configure PIO USB host here
#ifdef PIO_USB_HOST

#ifdef PICO_DEFAULT_PIO_USB_VBUSEN_PIN
    gpio_init(PICO_DEFAULT_PIO_USB_VBUSEN_PIN);
    gpio_set_dir(PICO_DEFAULT_PIO_USB_VBUSEN_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_PIO_USB_VBUSEN_PIN, PICO_DEFAULT_PIO_USB_VBUSEN_STATE);
#endif

    pio_usb_configuration_t pioHostConfig = PIO_USB_DEFAULT_CONFIG;
    // find an unused channel, then unclaim it again so PIO USB can claim it...
    pioHostConfig.tx_ch = dma_claim_unused_channel(true);
    dma_channel_unclaim(pioHostConfig.tx_ch);

    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pioHostConfig);

    tusb_rhport_init_t hostInit = {
        .role = TUSB_ROLE_HOST,
        .speed = TUSB_SPEED_AUTO
    };
    tusb_init(BOARD_TUH_RHPORT, &hostInit);
#endif

    // run
    while(true)
    {
        sys.getCPU().run(10);
        sys.getChipset().updateForDisplay();

        if(rtcSeconds)
        {
            rtcSeconds--; // probably overkill as we shouldn't get stuck in the CPU for a second...
            sys.getChipset().updateRTC();
        }
    }
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

    f_close(&configFile);

    return true;
}

static void setDiskLED(bool on)
{
#ifdef DISK_IO_LED_PIN
    gpio_put(DISK_IO_LED_PIN, on == DISK_IO_LED_ACTIVE);
#endif
}

int main()
{
#ifdef OVERCLOCK_500
    // this is tested on two of my boards
    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_60);
    sleep_ms(10);
    set_sys_clock_khz(504000, true);
#else
    // PIO USB wants a multiple of 12, HSTX wants a multiple of ~125 (25.175 * 10 / 2)
    set_sys_clock_khz(252000, false);
#endif

// with all the display handling here, there isn't enough CPU time on core0 for PIO USB
// also adjust the clock a bit
#ifndef PIO_USB_HOST
    tusb_rhport_init_t hostInit = {
        .role = TUSB_ROLE_HOST,
        .speed = TUSB_SPEED_AUTO
    };
    tusb_init(BOARD_TUH_RHPORT, &hostInit);
#endif

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

    // init blinky disk led
#ifdef DISK_IO_LED_PIN
    gpio_set_dir(DISK_IO_LED_PIN, true);
    gpio_put(DISK_IO_LED_PIN, !DISK_IO_LED_ACTIVE);
    gpio_set_function(DISK_IO_LED_PIN, GPIO_FUNC_SIO);
#endif

    // emulator init

    auto psram = reinterpret_cast<uint8_t *>(PSRAM_LOCATION);
    sys.addMemory(0, 8 * 1024 * 1024, psram);

    sys.getChipset().setSpeakerAudioCallback(speakerCallback);

    auto bios = _binary_bios_bin_start;
    auto biosSize = _binary_bios_bin_end - _binary_bios_bin_start;
    auto biosBase = 0x100000 - biosSize;
    memcpy(psram + biosBase, bios, biosBase);

    qemuCfg.setVGABIOS(reinterpret_cast<const uint8_t *>(_binary_vgabios_bin_start));

    vga.setTextWidthHack(true); // none of the display drivers handle 720 wide modes yet
    vga.setResolutionChangeCallback(vgaResolutionCallback);

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

    // set an initial time
    sys.getChipset().setRTC(28, 21, 14, 11, 9, 2025);

    // FIXME: mode changes
    set_display_size(640, 480);
    update_display();

    multicore_launch_core1(core1Main);

    while(true)
    {
        update_display();

        // check fifo for any commands from the emulator core
        uint32_t data;
        if(multicore_fifo_pop_timeout_us(1000, &data))
        {
            switch(data)
            {
                case 1: // floppy IO
                    setDiskLED(true);
                    floppyIO.doCore0IO();
                    setDiskLED(false);
                    break;
                case 2: // ATA IO
                    setDiskLED(true);
                    ataPrimaryIO.doCore0IO();
                    setDiskLED(false);
                    break;
            }
        }

        tuh_task();
    }

    return 0;
}