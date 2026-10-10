#pragma once
#include <stdint.h>
extern uint32_t test_tim2_count;
#define htim2 test_tim2_count
#define __HAL_TIM_GET_COUNTER(timer) (*(timer))
