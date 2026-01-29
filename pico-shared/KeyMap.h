#pragma once

#include <cstdint>

#include "Scancode.h"

ATScancode map_hid_key(uint8_t key);

ATScancode map_hid_mod(unsigned bit);