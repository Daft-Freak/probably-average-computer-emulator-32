#pragma once

#include <cstdint>

#define LL_TIMER // hack to significantly reduce overhead
#include "driver/gptimer.h"

#ifdef LL_TIMER
#define _Atomic
#include "../src/gptimer_priv.h" // gptimer_t
#include "hal/timer_ll.h"
#endif

extern gptimer_handle_t sysTimer;

inline uint32_t getTimer()
{
    uint64_t count;
#ifdef LL_TIMER
    auto context = &sysTimer->hal;
    timer_ll_trigger_soft_capture(context->dev, context->timer_id);
    count = timer_ll_get_counter_value(context->dev, context->timer_id);
#else
    gptimer_get_raw_count(sysTimer, &count);
#endif
    return count << 2;
}