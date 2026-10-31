#include "binary_writer.h"
#include "tim.h"

enum { ROUND_HEADER_LEN = 14, ROUND_CRC_LEN = 2 };
static uint32_t round_start_us;
static int round_failed;

int BinaryWriter_BeginRound(uint8_t *out, size_t out_size, size_t *off,
                            uint32_t round_seq, uint32_t tick_us,
                            uint32_t start_us)
{
    round_failed = 1;
    if (out == NULL || off == NULL || out_size < ROUND_HEADER_LEN + ROUND_CRC_LEN) {
        return 0;
    }
    *off = 0;
    out[(*off)++] = BINARY_FRAME_SYNC0;
    out[(*off)++] = BINARY_FRAME_SYNC1;
    out[(*off)++] = BINARY_FRAME_VERSION;
    out[(*off)++] = BINARY_FRAME_TYPE_ROUND;
    BinaryWriter_PutU32(out, off, round_seq);
    BinaryWriter_PutU32(out, off, tick_us);
    BinaryWriter_PutU16(out, off, 0U);
    round_start_us = start_us;
    round_failed = 0;
    return 1;
}

static uint16_t crc16_ccitt_false(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFU;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

void BinaryWriter_PutU8(uint8_t *payload, size_t *off, uint8_t value)
{
    payload[(*off)++] = value;
}

void BinaryWriter_PutI16(uint8_t *payload, size_t *off, int16_t value)
{
    BinaryWriter_PutU16(payload, off, (uint16_t)value);
}

void BinaryWriter_PutU16(uint8_t *payload, size_t *off, uint16_t value)
{
    payload[(*off)++] = (uint8_t)(value & 0xFFU);
    payload[(*off)++] = (uint8_t)((value >> 8) & 0xFFU);
}

void BinaryWriter_PutU32(uint8_t *payload, size_t *off, uint32_t value)
{
    payload[(*off)++] = (uint8_t)(value & 0xFFU);
    payload[(*off)++] = (uint8_t)((value >> 8) & 0xFFU);
    payload[(*off)++] = (uint8_t)((value >> 16) & 0xFFU);
    payload[(*off)++] = (uint8_t)((value >> 24) & 0xFFU);
}

int BinaryWriter_AppendFrame(uint8_t *out, size_t out_size, size_t *off,
                             uint8_t type, uint32_t seq, uint32_t tick_ms,
                             const uint8_t *payload, uint16_t payload_len)
{
    const int sensor = type == BINARY_FRAME_TYPE_AK ||
                       type == BINARY_FRAME_TYPE_TMAG ||
                       type == BINARY_FRAME_TYPE_ICM;
    const size_t record_len = 1U + 2U + (sensor ? 4U : 0U) + payload_len;

    (void)seq;
    (void)tick_ms;
    if (out == NULL || off == NULL || payload == NULL || round_failed ||
        record_len > UINT16_MAX) {
        round_failed = 1;
        return 0;
    }
    if (*off > out_size || record_len + ROUND_CRC_LEN > out_size - *off) {
        round_failed = 1;
        return 0;
    }
    out[(*off)++] = type;
    BinaryWriter_PutU16(out, off, (uint16_t)(record_len - 3U));
    if (sensor) {
        BinaryWriter_PutU32(out, off,
                            __HAL_TIM_GET_COUNTER(&htim2) - round_start_us);
    }

    for (uint16_t i = 0; i < payload_len; i++) {
        out[(*off)++] = payload[i];
    }

    return 1;
}

int BinaryWriter_EndRound(uint8_t *out, size_t out_size, size_t *off)
{
    if (round_failed || out == NULL || off == NULL || *off < ROUND_HEADER_LEN ||
        *off > out_size || out_size - *off < ROUND_CRC_LEN ||
        *off - ROUND_HEADER_LEN > UINT16_MAX) {
        round_failed = 1;
        return 0;
    }
    size_t len_off = 12U;
    BinaryWriter_PutU16(out, &len_off, (uint16_t)(*off - ROUND_HEADER_LEN));
    BinaryWriter_PutU16(out, off, crc16_ccitt_false(out + 2U, *off - 2U));
    return 1;
}

int BinaryWriter_AppendError(uint8_t *out, size_t out_size, size_t *off,
                             uint32_t seq, uint32_t tick_ms,
                             uint8_t source, uint16_t code, uint32_t detail)
{
    uint8_t payload[7];
    size_t poff = 0;

    BinaryWriter_PutU8(payload, &poff, source);
    BinaryWriter_PutU16(payload, &poff, code);
    BinaryWriter_PutU32(payload, &poff, detail);
    return BinaryWriter_AppendFrame(out, out_size, off, BINARY_FRAME_TYPE_ERR,
                                    seq, tick_ms, payload, (uint16_t)poff);
}

int BinaryWriter_AppendStats(uint8_t *out, size_t out_size, size_t *off,
                             uint32_t seq, uint32_t tick_ms,
                             uint32_t ak_frames, uint32_t tmag_frames,
                             uint32_t icm_frames, uint32_t skipped,
                             uint32_t errors, uint32_t device_id, uint16_t errormap, const uint32_t timing_us[3])
{
    uint8_t payload[38];
    size_t poff = 0;

    BinaryWriter_PutU32(payload, &poff, ak_frames);
    BinaryWriter_PutU32(payload, &poff, tmag_frames);
    BinaryWriter_PutU32(payload, &poff, icm_frames);
    BinaryWriter_PutU32(payload, &poff, skipped);
    BinaryWriter_PutU32(payload, &poff, errors);
    BinaryWriter_PutU32(payload, &poff, device_id);
    BinaryWriter_PutU16(payload, &poff, errormap);
    for (unsigned i = 0; i < 3; ++i) BinaryWriter_PutU32(payload, &poff, timing_us[i]);
    return BinaryWriter_AppendFrame(out, out_size, off, BINARY_FRAME_TYPE_STATS,
                                    seq, tick_ms, payload, (uint16_t)poff);
}

int BinaryWriter_AppendPhase(uint8_t *out, size_t out_size, size_t *off,
                             uint32_t seq, uint32_t tick_ms,
                             const uint32_t phase_us[3])
{
    uint8_t payload[12];
    size_t poff = 0;

    BinaryWriter_PutU32(payload, &poff, phase_us[0]);
    BinaryWriter_PutU32(payload, &poff, phase_us[1]);
    BinaryWriter_PutU32(payload, &poff, phase_us[2]);
    return BinaryWriter_AppendFrame(out, out_size, off, BINARY_FRAME_TYPE_PHASE,
                                    seq, tick_ms, payload, (uint16_t)poff);
}
