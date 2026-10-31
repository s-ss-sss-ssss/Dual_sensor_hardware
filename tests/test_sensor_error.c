#include "sensor_error.h"
#include <assert.h>
uint16_t g_sensor_errormap;
uint8_t g_sensor_failure_streak[12];
int main(void){
 SensorError_Failure(0); SensorError_Failure(0); assert(!SensorError_Has(0));
 SensorError_Success(0); SensorError_Failure(0); assert(!SensorError_Has(0));
 SensorError_Failure(0); SensorError_Failure(0); assert(SensorError_Has(0));
 SensorError_Success(0); assert(SensorError_Has(0));
 assert(!SensorError_Has(1)); return 0;
}
