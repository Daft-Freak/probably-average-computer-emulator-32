#include <cstdlib>
#include <ctime>
#include <forward_list>
#include <string>
#include <string_view>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/i2c.h"
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
#include "wifi_nina.h"

#include "i2c/seesaw.h"

#include "ATAController.h"
#include "FloppyController.h"
#include "GamePort.h"
#include "QEMUConfig.h"
#include "Scancode.h"
#include "System.h"
#include "VGACard.h"

// if any of the LED pins are defined, all of them should be
#ifdef DISK_IO_FD0_LED_PIN
static const int diskLedPins[]
{
    DISK_IO_FD0_LED_PIN,
    DISK_IO_FD1_LED_PIN,
    DISK_IO_ATA_PRI0_LED_PIN,
    DISK_IO_ATA_PRI1_LED_PIN
};
#endif

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

static bool wifiConnected = false;

static void initWifi(const char *ssid, const char *pass);

static void ntpRequest(const char *addr);

static void speakerCallback(int8_t sample)
{
    int16_t sample16 = sample << 4;
    audio_queue_sample(sample16);
}

void update_raw_key_state(const uint8_t keys[6], uint8_t mods)
{
    
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

// very similar to above...
void update_i2c_joystick_state(uint16_t axis[4], uint8_t buttons)
{
    // HACK: the values seem a bit low, so scale them up
    for(int i = 0; i < 4; i++)
        gamePort.setAxis(i, axis[i] / 65535.0f * 3.0f);

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

    std::string wifiSSID;

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
        else if(key == "wifi-ssid")
            wifiSSID = value;
        else if(key == "wifi-pass")
        {
            // init wifi when we have ssid and password
            // this does rely on ssid being in the config first...
            initWifi(wifiSSID.c_str(), value.data());
        }
        else if(key == "ntp-ip")
        {
            // ip because we don't do DNS yet
            ntpRequest(value.data());
        }
        else
            printf("unhandled config line %s\n", buf);
    }

    f_close(&configFile);

    return true;
}

// controller is 0 for floppy, 1 for ata primary
// device is 0-1
static void setDiskLED(int controller, int device, bool on)
{
#ifdef DISK_IO_FD0_LED_PIN
    gpio_put(diskLedPins[controller * 2 + device], on == DISK_IO_LED_ACTIVE);
#endif
}

static void initPreBIOSVGA()
{
    // set up the emulated VGA card so we can output text before the BIOS has started
    auto vgaRAM = vga.getRAM();

    // find 16x16 font in VGA BIOS (assuming seavgabios)
    auto vgaBIOS = reinterpret_cast<const uint8_t *>(_binary_vgabios_bin_start);
    static const uint8_t char1[]{0x00, 0x00, 0x7e, 0x81, 0xa5, 0x81, 0x81, 0xbd, 0x99, 0x81, 0x81, 0x7e, 0x00, 0x00, 0x00, 0x00};

    int fontOffset = 0;
    for(int i = 0; i < 0x10000; i++)
    {
        if(memcmp(char1, vgaBIOS + i, sizeof(char1)) == 0)
        {
            fontOffset = i - 16;
            break;
        }
    }

    // copy to VGA RAM
    for(int c = 0; c < 256; c++)
    {
        for(int i = 0; i < 16; i++)
            vgaRAM[0x20000 + c * 32 + i] = vgaBIOS[fontOffset + c * 16 + i];
    }

    vga.write(0x3C4, 1); vga.write(0x3C5, 1); // 8px wide chars

    vga.write(0x3D4, 0x01); vga.write(0x03D5, 79); // horiz chars = 80
    vga.write(0x3D4, 0x09); vga.write(0x03D5, 0xF); // char height = 16
    vga.write(0x3D4, 0x0C); vga.write(0x03D5, 0); // start addr
    vga.write(0x3D4, 0x0D); vga.write(0x03D5, 0); // ...
    vga.write(0x3D4, 0x13); vga.write(0x03D5, 40); // offset

    // palettes
    vga.read(0x3DA); // reset attrib addr/data
    for(int i = 0; i < 16; i++)
    {
        // identity mapping
        vga.write(0x3C0, i);
        vga.write(0x3C0, i);
    }

    // two colours
    vga.write(0x3C8, 0);

    vga.write(0x3C9, 0);
    vga.write(0x3C9, 0);
    vga.write(0x3C9, 0);

    vga.write(0x3C9, 0x1F);
    vga.write(0x3C9, 0x1F);
    vga.write(0x3C9, 0x1F);
}

static void vgaPrint(int x, int y, const char *text)
{
    auto vgaRAM = vga.getRAM();
    int offset = x + y * 80;

    size_t len = strlen(text);

    for(size_t i = 0; i < len; i++)
    {
        vgaRAM[(offset + i) * 2] = text[i]; // char
        vgaRAM[0x10000 + (offset + i) * 2] = 1; // attrib (bg=0, fg=1)
    }
}

static int vgaPrintf(int x, int y, const char *format, ...)
{
    va_list args;
    va_start(args, format);

    // get length
    va_list tmp_args;
    va_copy(tmp_args, args);
    int len = vsnprintf(nullptr, 0, format, tmp_args) + 1;
    va_end(tmp_args);

    auto buf = new char[len];
    int ret = vsnprintf(buf, len, format, args);
    va_end(args);

    vgaPrint(x, y, buf);
    // also log it
    puts(buf);

    delete[] buf;

    update_display();

    return ret;
}

static void initHardware()
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

    // reset fruit jam peripherals
#ifdef ADAFRUIT_FRUIT_JAM
    // wifi cs
    gpio_set_dir(ADAFRUIT_FRUIT_JAM_WIFI_CS_PIN, GPIO_OUT);
    gpio_put(ADAFRUIT_FRUIT_JAM_WIFI_CS_PIN, true);
    gpio_set_function(ADAFRUIT_FRUIT_JAM_WIFI_CS_PIN, GPIO_FUNC_SIO);

    gpio_set_dir(ADAFRUIT_FRUIT_JAM_PERIPH_RESET_PIN, GPIO_OUT);
    gpio_put(ADAFRUIT_FRUIT_JAM_PERIPH_RESET_PIN, 0);
    gpio_set_function(ADAFRUIT_FRUIT_JAM_PERIPH_RESET_PIN, GPIO_FUNC_SIO);
    sleep_ms(10);
    gpio_put(ADAFRUIT_FRUIT_JAM_PERIPH_RESET_PIN, 1);
#endif

    // init i2c_default if requested by board config
#ifdef DEFAULT_I2C_CLOCK
    i2c_init(i2c_default, DEFAULT_I2C_CLOCK);
    gpio_set_function(PICO_DEFAULT_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(PICO_DEFAULT_I2C_SCL_PIN, GPIO_FUNC_I2C);

    // TODO: put these in a list (when there's more than one driver)
    seesaw_init();
#endif

    init_display();
    set_display_size(640, 480);

    initPreBIOSVGA();

    size_t psramSize = psram_init(PSRAM_CS_PIN);

    vgaPrintf(0, 0, "Detected %i bytes PSRAM", psramSize);

    // init storage/filesystem
    auto res = f_mount(&fs, "", 1);

    if(res != FR_OK)
    {
        vgaPrintf(0, 1, "Failed to mount filesystem! (error %i)", res);

        if(res == FR_NOT_READY)
            vgaPrintf(0, 2, "Check that the SD card is inserted correctly.", res);

        while(true);
    }

    init_audio();

    // init blinky disk leds
#ifdef DISK_IO_FD0_LED_PIN
    for(auto &pin : diskLedPins)
    {
        gpio_set_dir(pin, true);
        gpio_put(pin, !DISK_IO_LED_ACTIVE);
        gpio_set_function(pin, GPIO_FUNC_SIO);
    }
#endif
}

static void initWifi(const char *ssid, const char *pass)
{
    // TODO: also regular pico w stuff
#if defined(WIFI_ESP32_NINA)
    vgaPrintf(0, 1, "Connecting to wifi...");

    nina_spi_init();

    if(!nina_connect_timeout(ssid, pass, 10000))
    {
        vgaPrintf(0, 1, "Failed to connect to wifi!");
        return;
    }

    vgaPrintf(0, 1, "Connected to wifi.   ");

    wifiConnected = true;
#endif
}

static void setRTCFromNTP(uint32_t ntpTime)
{
    time_t time = ntpTime - 2208988800;  // 1900 -> 1970

    tm *utc = gmtime(&time);

    sys.getChipset().setRTC(utc->tm_sec, utc->tm_min, utc->tm_hour, utc->tm_mday, utc->tm_mon + 1, utc->tm_year + 1900);
}

static void ntpRequest(const char *addr)
{
    if(!wifiConnected)
        return;

    static const int ntpPort = 123;
    static const int ntpMessageLen = 48;

#if defined(WIFI_ESP32_NINA)
    nina_sockaddr saddr;

    // TODO: DNS (or even less basic ip parsing)
    int a, b, c, d;
    sscanf(addr, "%i.%i.%i.%i", &a, &b, &c, &d);

    saddr.addr = a | b << 8 | c << 16 | d << 24;
    saddr.port = __builtin_bswap16(ntpPort);

    // get socket
    int fd = nina_socket(2 /*AF_INET*/, 2 /*SOCK_DGRAM*/, 0);

    // this is UDP, so try a few times
    bool done = false;
    for(int retry = 0; retry < 10; retry++)
    {
        // send message
        uint8_t buf[ntpMessageLen]{};
        buf[0] = 0x1B;

        nina_sendto(fd, buf, sizeof(buf), &saddr);

        // wait for reply
        // TODO: async?
        auto timeout = make_timeout_time_ms(1000);

        while(!time_reached(timeout) && !done)
        {
            if(nina_poll(fd) & 1/*read*/)
            {
                nina_sockaddr recvAddr;
                if(nina_recvfrom(fd, buf, ntpMessageLen, &recvAddr) == ntpMessageLen)
                {
                    int mode = buf[0] & 7;
                    int stratum = buf[1];

                    // check message
                    if(recvAddr.addr == saddr.addr && mode == 4 && stratum != 0)
                    {
                        setRTCFromNTP(buf[40] << 24 | buf[41] << 16 | buf[42] << 8 | buf[43]);
                        done = true;
                    }
                }
            }
        }

        if(done)
            break;
    }

    nina_close(fd);
#endif
}

static void initEmulator()
{
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

    sys.reset();

    // set an initial time
    sys.getChipset().setRTC(28, 21, 14, 11, 9, 2025);

    if(!readConfigFile())
    {
        // load a default image
        ataPrimaryIO.openDisk(0, "hd0.img");
        sys.getChipset().setFixedDiskPresent(0, ataPrimaryIO.getNumSectors(0) && !ataPrimaryIO.isATAPI(0));
    }

    multicore_launch_core1(core1Main);
}

int main()
{
    initHardware();

    // emulator init
    initEmulator();

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
                {
                    int dev = floppyIO.getCurAccessDevice();
                    setDiskLED(0, dev, true);
                    floppyIO.doCore0IO();
                    setDiskLED(0, dev, false);
                    break;
                }
                case 2: // ATA IO
                {
                    int dev = ataPrimaryIO.getCurAccessDevice();
                    setDiskLED(1, dev, true);
                    ataPrimaryIO.doCore0IO();
                    setDiskLED(1, dev, false);
                    break;
                }
            }
        }

#ifdef DEFAULT_I2C_CLOCK
        // TODO: put these in a list (when there's more than one driver)
        seesaw_update();
#endif

        tuh_task();
    }

    return 0;
}