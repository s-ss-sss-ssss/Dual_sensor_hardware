#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "binary_writer.h"

uint32_t test_tim2_count;

int main(void)
{
    uint8_t out[128] = {0};
    uint8_t sensor[11] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    uint32_t phase[3] = {1, 2, UINT32_MAX};
    size_t off = 0;
    assert(BinaryWriter_BeginRound(out, sizeof(out), &off, 42, UINT32_MAX - 2, UINT32_MAX - 2));
    assert(off == 14);
    assert(out[2] == 2);
    assert(out[8] == 0xFD && out[9] == 0xFF &&
           out[10] == 0xFF && out[11] == 0xFF);
    test_tim2_count = 7;
    size_t record_off = 0;
    assert(BinaryWriter_AppendFrame(out + off, sizeof(out) - off, &record_off,
                                    BINARY_FRAME_TYPE_AK, 99, 999, sensor, 11));
    off += record_off;
    assert(out[14] == BINARY_FRAME_TYPE_AK && out[15] == 15 && out[16] == 0);
    assert(out[17] == 10 && out[18] == 0 && out[19] == 0 && out[20] == 0);
    size_t second = off;
    assert(BinaryWriter_AppendFrameAt(out, sizeof(out), &off, BINARY_FRAME_TYPE_AK,
                                     0, 0, sensor, 11, UINT32_MAX));
    assert(out[second + 3] == 2 && out[second + 4] == 0 &&
           out[second + 5] == 0 && out[second + 6] == 0);
    assert(BinaryWriter_AppendPhase(out, sizeof(out), &off, 0, 0, phase));
    assert(BinaryWriter_AppendError(out, sizeof(out), &off, 0, 0, 2, 1, 3));
    size_t stats_off = off;
    assert(BinaryWriter_AppendStats(out, sizeof(out), &off, 0, 0, 1, 2, 3, 4, 5,
                                    0x12345678U, 0x0801U, (const uint32_t[3]){100, 200, 300}));
    assert(off - stats_off == 41);
    assert(out[stats_off] == BINARY_FRAME_TYPE_STATS);
    assert(out[stats_off + 1] == 38 && out[stats_off + 2] == 0);
    assert(out[stats_off + 23] == 0x78 && out[stats_off + 24] == 0x56 &&
           out[stats_off + 25] == 0x34 && out[stats_off + 26] == 0x12);
    assert(out[stats_off + 27] == 1 && out[stats_off + 28] == 8);
    assert(BinaryWriter_EndRound(out, sizeof(out), &off));
    assert(out[12] == off - 16 && out[13] == 0);

    assert(BinaryWriter_BeginRound(out, 20, &off, 43, 124, 0));
    record_off = 0;
    assert(!BinaryWriter_AppendFrame(out + off, 20 - off, &record_off,
                                     BINARY_FRAME_TYPE_AK, 0, 0, sensor, 11));
    assert(!BinaryWriter_EndRound(out, 20, &off));
    assert(BinaryWriter_BeginRound(out, sizeof(out), &off, 44, 125, 0));
    assert(BinaryWriter_AppendPhase(out, sizeof(out), &off, 0, 0, phase));
    assert(BinaryWriter_EndRound(out, sizeof(out), &off));
    return 0;
}
