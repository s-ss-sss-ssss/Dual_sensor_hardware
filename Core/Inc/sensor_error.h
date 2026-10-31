#pragma once
#include <stdint.h>
extern uint16_t g_sensor_errormap;
static inline unsigned SensorError_AK(unsigned bus, unsigned mask) {
 unsigned ch=0; while(mask>1){mask>>=1;ch++;} return (bus-1)*6+ch-1;
}
static inline unsigned SensorError_TMAG(unsigned mask, unsigned addr) {
 unsigned ch=0; while(mask>1){mask>>=1;ch++;} return (ch-1)*3+addr-0x34;
}
static inline int SensorError_Has(unsigned bit){return (g_sensor_errormap & (1U<<bit))!=0;}
static inline void SensorError_Set(unsigned bit){g_sensor_errormap |= (uint16_t)(1U<<bit);}
static inline unsigned SensorError_Count(void){unsigned n=0; for(unsigned i=0;i<12;i++) n+=SensorError_Has(i); return n;}

/* Runtime failures must be consecutive; initialization uses Set directly. */
#ifndef SENSOR_ERROR_FAILURE_LIMIT
#define SENSOR_ERROR_FAILURE_LIMIT 3U
#endif
#if SENSOR_ERROR_FAILURE_LIMIT < 1 || SENSOR_ERROR_FAILURE_LIMIT > 255
#error SENSOR_ERROR_FAILURE_LIMIT must be between 1 and 255
#endif
extern uint8_t g_sensor_failure_streak[12];
static inline void SensorError_Failure(unsigned bit) {
 if (SensorError_Has(bit)) return;
 if (g_sensor_failure_streak[bit] < SENSOR_ERROR_FAILURE_LIMIT)
  ++g_sensor_failure_streak[bit];
 if (g_sensor_failure_streak[bit] >= SENSOR_ERROR_FAILURE_LIMIT) SensorError_Set(bit);
}
static inline void SensorError_Success(unsigned bit) {g_sensor_failure_streak[bit] = 0;}
