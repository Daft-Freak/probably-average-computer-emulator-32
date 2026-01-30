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

#include "clock.pio.h"

#include "Audio.h"
#include "BIOS.h"
#include "RemoteIO.h"

#include "QEMUConfig.h"
#include "System.h"

static FATFS fs;

static System sys;

static QEMUConfig qemuCfg(sys);
static RemoteIO remoteIO(sys);

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
}

// very similar to above...
void update_i2c_joystick_state(uint16_t axis[4], uint8_t buttons)
{
}

// the first argument serves no purpose other than making this function not shuffle regs around
void __not_in_flash_func(display_draw_line)(void *, int line, uint16_t *buf)
{
}

static bool rtcTimerCallback(repeating_timer *t)
{
    rtcSeconds++;
    return true;
}

static void remoteStatusCallback(uint gpio, uint32_t event)
{
    remoteIO.syncStatus();
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

    // setup timer for RTC
    repeating_timer timer;
    add_repeating_timer_ms(1000, rtcTimerCallback, nullptr, &timer);

    // setup io for status notifications
    int statusPin = 36;

    gpio_init(statusPin);
    gpio_set_irq_enabled_with_callback(statusPin, GPIO_IRQ_LEVEL_HIGH, true, remoteStatusCallback);

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

        if(key == "wifi-ssid")
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

    // also log it
    puts(buf);

    delete[] buf;

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

    tusb_rhport_init_t hostInit = {
        .role = TUSB_ROLE_HOST,
        .speed = TUSB_SPEED_AUTO
    };
    tusb_init(BOARD_TUH_RHPORT, &hostInit);

    stdio_init_all();

    // before we init anything, reserve PIO1 SM0 for the clock program
    pio_claim_sm_mask(pio1, 1 << 0);

    size_t psramSize = psram_init(PSRAM_CS_PIN);

    vgaPrintf(0, 0, "Detected %i bytes PSRAM", psramSize);

    // init storage/filesystem
    auto res = f_mount(&fs, "", 1);

    if(res != FR_OK)
    {
        vgaPrintf(0, 1, "Failed to mount filesystem! (error %i)", res);

        if(res == FR_NOT_READY)
            vgaPrintf(0, 2, "Check that the SD card is inserted correctly.", res);

        // storage is optional here, it would only contain the config file
        //while(true);
    }

    init_audio();
}

static void initWifi(const char *ssid, const char *pass)
{
    // TODO: also regular pico w stuff
}

/*static void setRTCFromNTP(uint32_t ntpTime)
{
    time_t time = ntpTime - 2208988800;  // 1900 -> 1970

    tm *utc = gmtime(&time);

    sys.getChipset().setRTC(utc->tm_sec, utc->tm_min, utc->tm_hour, utc->tm_mday, utc->tm_mon + 1, utc->tm_year + 1900);
}*/

static void ntpRequest(const char *addr)
{
    if(!wifiConnected)
        return;

    //static const int ntpPort = 123;
    //static const int ntpMessageLen = 48;
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


    remoteIO.init(); // needs to be after memory init

    sys.reset();

    // set an initial time
    sys.getChipset().setRTC(28, 21, 14, 11, 9, 2025);

    readConfigFile();

    multicore_launch_core1(core1Main);
}

int main()
{
    initHardware();

    // emulator init
    initEmulator();

    while(true)
    {
        remoteIO.syncInputs();
        tuh_task();
    }

    return 0;
}