#pragma once
#ifdef PICO_BUILD
#include "pico.h"
#define RAM_FUNC(x) __not_in_flash_func(x)
#elif defined(ESP_BUILD)
#include "esp_attr.h"
#define RAM_FUNC(x) IRAM_ATTR x
#else
#define RAM_FUNC(x) x
#endif
