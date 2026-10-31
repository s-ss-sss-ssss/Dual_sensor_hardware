#pragma once
#include "ak09973d.h"
#include "i2c.h"
#include <stddef.h>

#define AK09973D_STATIC_CONFIG \
    {1, 0x02, 0x10}, {1, 0x04, 0x10}, {1, 0x08, 0x10}, \
    {1, 0x10, 0x10}, {1, 0x20, 0x10}, {1, 0x40, 0x10}, \
    {2, 0x02, 0x10}, {2, 0x04, 0x10}, {2, 0x08, 0x10}, \
    {2, 0x10, 0x10}, {2, 0x20, 0x10}, {2, 0x40, 0x10}

#define AK09973D_TCA_ADDR_7B  0x70
#define AK09973D_COUNT         12
#define AK09973D_ACTIVE_CNTL2  AK09973D_MODE_500HZ
#define AK09973D_DESELECT_AFTER_READ  0
#define AK09973D_TCA_SETTLE_MS        0
#define AK09973D_POST_READ_MS         0

void Sensor_AK09973D_Init_All(void);
int Sensor_AK09973D_ReadToCSV(char *out, size_t out_size);
int Sensor_AK09973D_ReadToBinary(uint8_t *out, size_t out_size, uint32_t *seq,
                                 uint32_t *frames, uint32_t *skipped,
                                 uint32_t *errors);
void Sensor_AK09973D_ReadAll(void);
int Sensor_AK09973D_GetCount(void);

/* Last binary scan: TCA select + AK read wall time, microseconds. */
extern uint32_t g_ak_bus_read_us[2];
