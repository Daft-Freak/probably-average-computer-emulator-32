#pragma once

#include <cstdint>

#define LL_TIMER // hacks to significantly reduce overhead

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
    // assume that because we created this timer as early as possible, it is the first one
    timer_ll_trigger_soft_capture(context->dev, 0);
    //count = timer_ll_get_counter_value(context->dev, context->timer_id);
    // avoid reading high half
    count = context->dev->hw_timer[0].lo.tx_lo;
#else
    gptimer_get_raw_count(sysTimer, &count);
#endif
    return count << 2;
}