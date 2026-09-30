#pragma once

#include <cstdint>

struct I2CDriver
{
    void (*init)();
    void (*update)();
};

// for RTCs

struct RTCDateTime
{
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t days;
    uint8_t month;
    uint16_t year;
};

extern RTCDateTime rtcSyncTime;