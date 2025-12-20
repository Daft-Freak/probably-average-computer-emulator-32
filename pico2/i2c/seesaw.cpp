#include <cstdint>
#include <cstdio>

#include "hardware/i2c.h"

#include "seesaw.h"

#include "config.h"

#ifndef SEESAW_I2C
#define SEESAW_I2C i2c_default
#endif

#ifndef SEESAW_ADDR
#define SEESAW_ADDR 0x50
#endif

enum class Module : uint8_t {
  Status = 0,
  GPIO = 1,
  // ...
  ADC = 9,
  // ...
};

enum class Function : uint8_t {
  Status_HW_ID   = 0x01,
  Status_VERSION = 0x02,
  Status_OPTIONS = 0x03,
  Status_TEMP    = 0x04,
  Status_SWRST   = 0x7F,

  GPIO_DIRSET    = 0x02,
  GPIO_DIRCLR    = 0x03,
  GPIO_GPIO      = 0x04,
  GPIO_SET       = 0x05,
  GPIO_CLR       = 0x06,
  GPIO_TOGGLE    = 0x07,
  GPIO_INTENSET  = 0x08,
  GPIO_INTENCLR  = 0x09,
  GPIO_INTFLAG   = 0x0A,
  GPIO_PULLENSET = 0x0B,
  GPIO_PULLENCLR = 0x0C,

  ADC_STATUS     = 0x00,
  ADC_INTENSET   = 0x02,
  ADC_INTENCLR   = 0x03,
  ADC_WINMODE    = 0x04,
  ADC_WINTHRESH  = 0x05,
  ADC_CHANNEL0   = 0x07,
  ADC_CHANNEL1   = 0x08,
  ADC_CHANNEL2   = 0x09,
  ADC_CHANNEL3   = 0x0A,
  ADC_CHANNEL4   = 0x0B,
  ADC_CHANNEL5   = 0x0C,
  ADC_CHANNEL6   = 0x0D,
  ADC_CHANNEL7   = 0x0E,
  ADC_CHANNEL8   = 0x0F,
  ADC_CHANNEL9   = 0x10,
  ADC_CHANNEL10  = 0x11,
  ADC_CHANNEL11  = 0x12,
  ADC_CHANNEL12  = 0x13,
  ADC_CHANNEL13  = 0x14,
  ADC_CHANNEL14  = 0x15,
  ADC_CHANNEL15  = 0x16,
  ADC_CHANNEL16  = 0x17,
  ADC_CHANNEL17  = 0x18,
  ADC_CHANNEL18  = 0x19,
  ADC_CHANNEL19  = 0x1A,
  ADC_CHANNEL20  = 0x1B,
};

enum class SeesawState : uint8_t {
    GPIORequest = 0,
    GPIORead,

    AnalogRequest,
    AnalogRead,

    Done,
};

static SeesawState state = SeesawState::GPIORead;
static uint8_t analogIndex = 0;
static int alarmNum;

static uint32_t gpioState;
static uint16_t analogState[4];

// IO mapping
static int8_t buttonIO[4]{-1, -1, -1, -1};
static int8_t analogIO[4]{-1, -1, -1, -1};
static int analogMax = 1;
static int analogInvert = 0;

// TODO: declare this somewhere shared
void update_i2c_joystick_state(uint16_t axis[4], uint8_t buttons);

static bool seesaw_read(uint8_t i2c_addr, Module module, Function function, uint8_t *data, int len, int delay_us)
{
    uint8_t cmd[]{uint8_t(module), uint8_t(function)};
    if(i2c_write_blocking_until(SEESAW_I2C, i2c_addr, cmd, 2, true, make_timeout_time_ms(1)) != 2)
        return false;

    sleep_us(delay_us);

    return i2c_read_blocking(SEESAW_I2C, i2c_addr, data, len, false) == len;
}

static bool seesaw_write(uint8_t i2c_addr, Module module, Function function, uint8_t *data, int len)
{
    uint8_t cmd[]{uint8_t(module), uint8_t(function)};

    if(i2c_write_blocking_until(SEESAW_I2C, i2c_addr, cmd, 2, true, make_timeout_time_ms(1)) != 2)
        return false;

    // write the rest manually
    // (we don't want a RESTART and write_raw doesn't do a STOP)
    for(int i = 0; i < len; i++)
    {
        while (!i2c_get_write_available(SEESAW_I2C));

        i2c_get_hw(SEESAW_I2C)->data_cmd = data[i] | (i == len - 1 ? I2C_IC_DATA_CMD_STOP_BITS : 0);
    }

    while(!(i2c_get_hw(SEESAW_I2C)->raw_intr_stat & I2C_IC_RAW_INTR_STAT_STOP_DET_BITS));

    SEESAW_I2C->restart_on_next = false;

    return true;
}

static void seesaw_alarm_callback(uint alarm_num)
{
    timer_hw->intr = 1 << alarm_num;

    switch(state)
    {
        case SeesawState::GPIORequest:
        {
            uint8_t cmd[]{uint8_t(Module::GPIO), uint8_t(Function::GPIO_GPIO)};
            auto timeout = make_timeout_time_us(500);

            if(i2c_write_blocking_until(SEESAW_I2C, SEESAW_ADDR, cmd, 2, false, timeout) == 2)
                state = SeesawState::GPIORead;

            hardware_alarm_set_target(alarm_num, make_timeout_time_us(250));
            break;
        }

        case SeesawState::GPIORead:
        {
            auto timeout = make_timeout_time_us(1000);
            i2c_read_blocking_until(SEESAW_I2C, SEESAW_ADDR, (uint8_t *)&gpioState, 4, false, timeout);

            state = SeesawState::AnalogRequest;
            hardware_alarm_set_target(alarm_num, make_timeout_time_us(100));
            break;
        }

        case SeesawState::AnalogRequest:
        {
            uint8_t func = int(Function::ADC_CHANNEL0) + analogIO[analogIndex];
            uint8_t cmd[]{uint8_t(Module::ADC), func};
            auto timeout = make_timeout_time_us(500);

            if(i2c_write_blocking_until(SEESAW_I2C, SEESAW_ADDR, cmd, 2, false, timeout) == 2)
                state = SeesawState::AnalogRead;

            hardware_alarm_set_target(alarm_num, make_timeout_time_us(500));
            break;
        }

        case SeesawState::AnalogRead:
        {
            auto timeout = make_timeout_time_us(1000);
            i2c_read_blocking_until(SEESAW_I2C, SEESAW_ADDR, (uint8_t *)&analogState[analogIndex], 2, false, timeout);

            // stop if last axis, otherwise move to the next one
            if(++analogIndex == 4 || analogIO[analogIndex] == -1)
            {
                analogIndex = 0;
                state = SeesawState::Done;
            }
            else
            {
                state = SeesawState::AnalogRequest;
                hardware_alarm_set_target(alarm_num, make_timeout_time_us(100));
            }
            break;
        }

        case SeesawState::Done:
            break; // shouldn't happen
    }
}

void seesaw_init()
{
    // state
    gpioState = ~0;

    for(auto &x : analogState)
        x = 0xFF01;

    // get product id
    uint8_t version[4]{};

    if(!seesaw_read(SEESAW_ADDR, Module::Status, Function::Status_VERSION, version, 4, 5))
        return;

    if(version[1] == 0xFF && version[2] == 0xFF && version[3] == 0xFF)
        return;

    int product = version[0] << 8 | version[1]; // should check that this is 5743
    int day = version[2] >> 3;
    int month = (version[2] & 7) << 1 | version[3] >> 7;
    int year = version[3] & 0x7F;

    printf("Seesaw addr %02X product %i date 20%02i-%02i-%02i\n", SEESAW_ADDR, product, year, month, day);

    if(product == 5743) // gamepad
    {
        // ABXY order (copied from 32blit)
        buttonIO[0] = 5;
        buttonIO[1] = 1;
        buttonIO[2] = 6;
        buttonIO[3] = 2;

        // only 2 axes
        analogIO[0] = 14;
        analogIO[1] = 15;

        // X axis is inverted
        analogInvert = 1 << 0;

        analogMax = 1023;
    }
    else if(product == 5753) // pc joystick
    {
        // this is untested, but should be the right IOs
        // will need a SEESAW_ADDR override due to different default address
        buttonIO[0] = 3;
        buttonIO[1] = 13;
        buttonIO[2] = 2;
        buttonIO[3] = 14;

        analogIO[0] = 1;
        analogIO[1] = 15;
        analogIO[2] = 0;
        analogIO[3] = 16;

        // TODO: likely will need adjusting
        analogMax = 1023;
    }
    else
        return;

    // init
    // TODO: skip any -1s? (currently never happens)
    uint32_t ioMask = 1 << buttonIO[0] | 1 << buttonIO[1] | 1 << buttonIO[2] | 1 << buttonIO[3];
    ioMask = __builtin_bswap32(ioMask); // seesaw is msb first

    // set inputs
    seesaw_write(SEESAW_ADDR, Module::GPIO, Function::GPIO_DIRCLR, (uint8_t *)&ioMask, 4);

    // enable pullups
    seesaw_write(SEESAW_ADDR, Module::GPIO, Function::GPIO_PULLENSET, (uint8_t *)&ioMask, 4);
    seesaw_write(SEESAW_ADDR, Module::GPIO, Function::GPIO_SET, (uint8_t *)&ioMask, 4);

    // setup an alarm for async polling
    alarmNum = hardware_alarm_claim_unused(true);
    hardware_alarm_set_callback(alarmNum, seesaw_alarm_callback);

    seesaw_alarm_callback(alarmNum);
}

void seesaw_update()
{
    // dont do anything while we're still updating
    if(state != SeesawState::Done)
        return;

    // map buttons
    uint8_t buttons = 0;
    uint32_t gpio = __builtin_bswap32(gpioState);

    for(int i = 0; i < 4; i++)
    {
        if(!(gpio & (1 << buttonIO[i])))
            buttons |= (1 << i);
    }

    // scale axes
    uint16_t scaledAxes[4];

    for(int i = 0; i < 4; i++)
    {
        uint32_t val = __builtin_bswap16(analogState[i]);
        val = val * 65535 / analogMax;

        // ... and invert
        if(analogInvert & (1 << i))
            val = 65535 - val;

        scaledAxes[i] = val;
    }

    update_i2c_joystick_state(scaledAxes, buttons);

    // start new read cycle
    state = SeesawState::GPIORequest;
    seesaw_alarm_callback(alarmNum);
}