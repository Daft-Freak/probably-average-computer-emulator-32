#pragma once
#include <cstdint>

#include "hardware/pio.h"

inline uint32_t getTimer()
{
    return pio1->rxf_putget[0][0];
}