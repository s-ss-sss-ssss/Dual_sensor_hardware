#include "binary_writer.h"

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
    const size_t header_len = 2U + 1U + 1U + 4U + 4U + 2U;
    const size_t total_len = header_len + payload_len + 2U;
    size_t start = *off;

    if (out == NULL || off == NULL || payload == NULL) {
        return 0;
    }
    if ((out_size - start) < total_len) {
        return 0;
    }

    out[(*off)++] = BINARY_FRAME_SYNC0;
    out[(*off)++] = BINARY_FRAME_SYNC1;
    out[(*off)++] = BINARY_FRAME_VERSION;
    out[(*off)++] = type;
    BinaryWriter_PutU32(out, off, seq);
    BinaryWriter_PutU32(out, off, tick_ms);
    BinaryWriter_PutU16(out, off, payload_len);

    for (uint16_t i = 0; i < payload_len; i++) {
        out[(*off)++] = payload[i];
    }

    uint16_t crc = crc16_ccitt_false(&out[start + 2U], total_len - 4U);
    BinaryWriter_PutU16(out, off, crc);
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
                             uint32_t errors)
{
    uint8_t payload[20];
    size_t poff = 0;

    BinaryWriter_PutU32(payload, &poff, ak_frames);
    BinaryWriter_PutU32(payload, &poff, tmag_frames);
    BinaryWriter_PutU32(payload, &poff, icm_frames);
    BinaryWriter_PutU32(payload, &poff, skipped);
    BinaryWriter_PutU32(payload, &poff, errors);
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
