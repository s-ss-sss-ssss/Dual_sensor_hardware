#pragma once

#include <stddef.h>
#include <stdint.h>

#define SENSOR_OUTPUT_FORMAT_CSV     0
#define SENSOR_OUTPUT_FORMAT_BINARY  1

#define BINARY_FRAME_SYNC0  0xA5U
#define BINARY_FRAME_SYNC1  0x5AU
#define BINARY_FRAME_VERSION 2U
#define BINARY_FRAME_TYPE_ROUND  0x10U

#define BINARY_FRAME_TYPE_AK     0x01U
#define BINARY_FRAME_TYPE_TMAG   0x02U
#define BINARY_FRAME_TYPE_ICM    0x03U
#define BINARY_FRAME_TYPE_PHASE  0x04U
#define BINARY_FRAME_TYPE_ERR    0xE0U
#define BINARY_FRAME_TYPE_STATS  0xF0U

#define BINARY_ERR_SOURCE_AK     0x01U
#define BINARY_ERR_SOURCE_TMAG   0x02U
#define BINARY_ERR_SOURCE_ICM    0x03U
#define BINARY_ERR_SOURCE_USB    0x04U
#define BINARY_ERR_SOURCE_USB    0x04U

/* tick_us is the round-start TIM2 counter (1 us/tick); start_us is the same
 * sample used as the sensor read-done offset baseline. Both wrap at 2^32.
 * Begin/End bracket one ROUND frame. AppendFrame writes one length-prefixed
 * record into the supplied slice; any append failure invalidates the round. */
int BinaryWriter_BeginRound(uint8_t *out, size_t out_size, size_t *off,
                            uint32_t round_seq, uint32_t tick_us,
                            uint32_t start_us);
int BinaryWriter_EndRound(uint8_t *out, size_t out_size, size_t *off);

int BinaryWriter_AppendFrame(uint8_t *out, size_t out_size, size_t *off,
                             uint8_t type, uint32_t seq, uint32_t tick_ms,
                             const uint8_t *payload, uint16_t payload_len);
int BinaryWriter_AppendError(uint8_t *out, size_t out_size, size_t *off,
                             uint32_t seq, uint32_t tick_ms,
                             uint8_t source, uint16_t code, uint32_t detail);
int BinaryWriter_AppendStats(uint8_t *out, size_t out_size, size_t *off,
                             uint32_t seq, uint32_t tick_ms,
                             uint32_t ak_frames, uint32_t tmag_frames,
                             uint32_t icm_frames, uint32_t skipped,
                             uint32_t errors, uint32_t device_id);
int BinaryWriter_AppendPhase(uint8_t *out, size_t out_size, size_t *off,
                             uint32_t seq, uint32_t tick_ms,
                             const uint32_t phase_us[3]);
void BinaryWriter_PutU8(uint8_t *payload, size_t *off, uint8_t value);
void BinaryWriter_PutI16(uint8_t *payload, size_t *off, int16_t value);
void BinaryWriter_PutU16(uint8_t *payload, size_t *off, uint16_t value);
void BinaryWriter_PutU32(uint8_t *payload, size_t *off, uint32_t value);
