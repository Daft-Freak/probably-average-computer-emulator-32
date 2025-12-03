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

    //return false;

    // config-95 config-386bench
    if(f_open(&configFile, "config-games.txt", FA_READ | FA_OPEN_EXISTING) != FR_OK)
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

#include <cmath>
#include "hardware/structs/coresight_trace.h"

void enableCPUSample()
{
    int uartBaud = 3000000;
    gpio_set_function(0, GPIO_FUNC_UART);
    uart_init(uart0, uartBaud);

    // configure TPIU for 8 bit port
    *(uint32_t *)(CORESIGHT_TPIU_BASE + 4) = 1 << 7; // CSPSR

    *(uint32_t *)(CORESIGHT_TPIU_BASE + 0x304) = 1 << 8; // FFCR = TrigIn

    // disable all funnel inputs
    hw_clear_bits((uint32_t *)(CORESIGHT_ATB_FUNNEL_BASE + 0), 0xFF);

    // enable DWT+ITM
    m33_hw->demcr = M33_DEMCR_TRCENA_BITS;

    // enable ITM and forwarding from DWT
    m33_hw->itm_tcr = 1 << M33_ITM_TCR_TRACEBUSID_LSB | M33_ITM_TCR_TXENA_BITS | M33_ITM_TCR_ITMENA_BITS;

    // enable DMA access
    hw_set_bits(&accessctrl_hw->coresight_trace, 0xACCE0000 | ACCESSCTRL_CORESIGHT_TRACE_DMA_BITS);

    // flush/hold FIFO
    hw_set_bits(&coresight_trace_hw->ctrl_status, CORESIGHT_TRACE_CTRL_STATUS_TRACE_CAPTURE_FIFO_FLUSH_BITS);

    // dma from FIFO
    int dmaChannel = dma_claim_unused_channel(true);
    auto config = dma_channel_get_default_config(dmaChannel);
    channel_config_set_dreq(&config, DREQ_CORESIGHT);
    channel_config_set_read_increment(&config, false);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_8);

    dma_channel_configure(dmaChannel, &config, &uart0_hw->dr, &coresight_trace_hw->trace_capture_fifo, 0xF0000001, false);
    dma_channel_start(dmaChannel);

    // enable FIFO
    hw_clear_bits(&coresight_trace_hw->ctrl_status, CORESIGHT_TRACE_CTRL_STATUS_TRACE_CAPTURE_FIFO_FLUSH_BITS);

    // enable cycle counter and pc sampling
    int dataRate = ceil((clock_get_hz(clk_sys) / 1024.0f/* CYCTAP=1 */) * 5 /*event is 5 bytes*/); 
    int uartRate = uartBaud / 10;
    int div = std::ceil(float(dataRate) / uartRate);
    m33_hw->dwt_ctrl = M33_DWT_CTRL_PCSAMPLENA_BITS | M33_DWT_CTRL_CYCTAP_BITS | div << M33_DWT_CTRL_POSTPRESET_LSB | M33_DWT_CTRL_CYCCNTENA_BITS;

    // enable receiver 0 in FUNNELCONTROL
    *(uint32_t *)(CORESIGHT_ATB_FUNNEL_BASE + 0) |= 0x1;
}

//
#include "hardware/spi.h"

class SPIATAIO final : public ATADiskIO
{
public:

    void init()
    {
        spi_init(spi1, 2000000);
        gpio_set_function(28, GPIO_FUNC_SPI);
        gpio_set_function(30, GPIO_FUNC_SPI);
        gpio_set_function(31, GPIO_FUNC_SPI);
        gpio_set_function(45, GPIO_FUNC_SPI);

        for(int i = 0; i < maxDrives; i++)
        {
            // get info
            uint8_t cmd = 0x00 | i << 7;
            spi_write_blocking(spi1, &cmd, 1);

            while(true)
            {
                uint8_t res;
                spi_read_blocking(spi1, 0xFF, &res, 1);

                if(res == 0xEE)
                {
                    // failed
                    numSectors[i] = 0;
                    isCD[i] = false;
                    break;
                }
                else if(res == 0xAA)
                {
                    // success
                    uint8_t buf[5];
                    spi_read_blocking(spi1, 0xFF, buf, 5);

                    isCD[i] = buf[0];
                    numSectors[i] = buf[1] | buf[2] << 8 | buf[3] << 16 | buf[4] << 24;

                    printf("SPI ATA %i %lu sectors\n", i, numSectors[i]);
                    break;
                }
            }
        }
    }

    uint32_t getNumSectors(int device) override
    {
        if(device >= maxDrives)
            return 0;
        return numSectors[device];
    }

    bool isATAPI(int device) override
    {
        if(device >= maxDrives)
            return 0;
        return isCD[device];
    }

    bool read(ATAController *controller, int device, uint8_t *buf, uint32_t lba) override
    {
        if(device >= maxDrives)
            return false;

        uint8_t cmdBuf[5];

        cmdBuf[0] = 0x01 | device << 7, // read
        cmdBuf[1] = (lba >>  0) & 0xFF;
        cmdBuf[2] = (lba >>  8) & 0xFF;
        cmdBuf[3] = (lba >> 16) & 0xFF;
        cmdBuf[4] = (lba >> 24) & 0xFF;

        spi_write_blocking(spi1, cmdBuf, sizeof(cmdBuf));

        while(true)
        {
            uint8_t res;
            spi_read_blocking(spi1, 0xFF, &res, 1);

            if(res == 0xEE)
            {
                // failed
                return false;
            }
            else if(res == 0xAA)
            {
                spi_read_blocking(spi1, 0xFF, buf, 512);
                controller->ioComplete(device, true, false);

                return true;
            }
        }

        return false;
    }

    bool write(ATAController *controller, int device, const uint8_t *buf, uint32_t lba) override
    {
        if(device >= maxDrives)
            return false;

        uint8_t cmdBuf[5];

        cmdBuf[0] = 0x02 | device << 7, // write
        cmdBuf[1] = (lba >>  0) & 0xFF;
        cmdBuf[2] = (lba >>  8) & 0xFF;
        cmdBuf[3] = (lba >> 16) & 0xFF;
        cmdBuf[4] = (lba >> 24) & 0xFF;

        spi_write_blocking(spi1, cmdBuf, sizeof(cmdBuf));

        while(true)
        {
            uint8_t res;
            spi_read_blocking(spi1, 0xFF, &res, 1);

            if(res == 0xEE)
            {
                // failed
                return false;
            }
            else if(res == 0xAA)
                break;
        }

        //
        sleep_us(10);

        // do write
        spi_write_blocking(spi1, buf, 512);

        while(true)
        {
            uint8_t res;
            spi_read_blocking(spi1, 0xFF, &res, 1);

            if(res == 0xEE)
            {
                // failed
                return false;
            }
            else if(res == 0xAA)
            {
                controller->ioComplete(device, true, true);
                return true;
            }
        }


        return false;
    }

    static const int maxDrives = 2;

private:
    uint32_t numSectors[maxDrives]{};
    bool isCD[maxDrives]{};
};

static SPIATAIO spiIO;
//

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

    //
    //enableCPUSample();
    //

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
    //ataPrimary.setIOInterface(&spiIO);
    fdc.setIOInterface(&floppyIO);

    if(!readConfigFile())
    {
        // load a default image
        ataPrimaryIO.openDisk(0, "hd-win3.0-vga.img");
        sys.getChipset().setFixedDiskPresent(0, ataPrimaryIO.getNumSectors(0) && !ataPrimaryIO.isATAPI(0));
    }

    //spiIO.init();

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