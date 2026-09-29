#pragma once

struct I2CDriver
{
    void (*init)();
    void (*update)();
};