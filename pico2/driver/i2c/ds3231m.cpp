#include <cstdint>

#include "hardware/i2c.h"

#include "ds3231m.h"

#include "config.h"

#ifndef DS3231M_I2C
#define DS3231M_I2C i2c_default
#endif

#define DS3231M_ADDR 0x68

static bool ds3231mPresent = false;

inline int bcdDecode(uint8_t v)
{
    return (v & 0xF) + (v >> 4) * 10;
}

inline uint8_t bcdEncode(int v)
{
    return (v % 10) | (v / 10) << 4;
}

static void ds3231m_init()
{
    // try to read date/time
    uint8_t addr = 0;
    if(i2c_write_blocking_until(DS3231M_I2C, DS3231M_ADDR, &addr, 1, true, make_timeout_time_ms(1)) != 1)
        return;

    uint8_t data[7];

    if(i2c_read_blocking_until(DS3231M_I2C, DS3231M_ADDR, data, sizeof(data), false, make_timeout_time_ms(1)) != sizeof(data))
        return;

    // it worked, we have an RTC!
    ds3231mPresent = true;

    // store initial time value
    rtcSyncTime.seconds = bcdDecode(data[0]);
    rtcSyncTime.minutes = bcdDecode(data[1]);
    
    if(data[2] & (1 << 6)) // 12 hour
        rtcSyncTime.hours = bcdDecode(data[2] & 0x1F) + ((data[2] & (1 << 5)) ? 12 : 0);
    else // 24 hour
        rtcSyncTime.hours = bcdDecode(data[2]);

    rtcSyncTime.days = bcdDecode(data[4]);
    rtcSyncTime.month = bcdDecode(data[5] & 0x1F);
    rtcSyncTime.year = 2000 + bcdDecode(data[6]) + ((data[5] & (1 << 7)) ? 100 : 0) /*century*/;
}

static void ds3231m_update()
{
    if(!ds3231mPresent)
        return;

    if(rtcSyncTime.year)
    {
        uint8_t century = rtcSyncTime.year >= 2100 ? (1 << 7) : 0;
        uint8_t data[]
        {
            0x00, // addr
            bcdEncode(rtcSyncTime.seconds),
            bcdEncode(rtcSyncTime.minutes),
            bcdEncode(rtcSyncTime.hours), // as 24 hour
            0, // day of week
            bcdEncode(rtcSyncTime.days),
            static_cast<uint8_t>(bcdEncode(rtcSyncTime.month) | century),
            bcdEncode(rtcSyncTime.year % 100)
        };

        i2c_write_blocking_until(DS3231M_I2C, DS3231M_ADDR, data, sizeof(data), true, make_timeout_time_ms(1));
    }
}

const I2CDriver ds3231mDriver
{
    ds3231m_init, ds3231m_update
};