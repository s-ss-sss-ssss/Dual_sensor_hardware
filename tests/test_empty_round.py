"""Host-run the actual binary main-loop body with mocked sensors/time/USB.

Run: python -m unittest discover -s tests -v
Requires a native gcc on PATH. The real binary writer supplies the wire bytes.
"""
import re
import struct
import subprocess
import unittest
from pathlib import Path

from test_usb_read import reader

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "binary_writer.h"
#define PHASE_CAPTURE_CHANNEL_COUNT 3U
#define SENSOR_OUTPUT_FORMAT SENSOR_OUTPUT_FORMAT_BINARY
#define SENSOR_OUTPUT_ICM 1
#define SENSOR_OUTPUT_AK 1
#define SENSOR_OUTPUT_TMAG 1
uint32_t test_tim2_count, g_device_id = 0x12345678U;
static uint32_t tick, read_cost, usb_cost, phase_cost, delay_total;
static int mode, send_ok, icm, sends;
static FILE *wire;
static uint32_t HAL_GetTick(void) { return tick; }
static void HAL_Delay(uint32_t ms) { delay_total += ms; tick += ms; }
static void PhaseCapture_Sample(uint32_t *phase, uint32_t *start) {
    tick += phase_cost;
    phase[0] = phase[1] = phase[2] = UINT32_MAX;
    test_tim2_count = tick * 1000U;
    *start = test_tim2_count;
}
static int sample(uint8_t type, uint8_t *out, size_t cap, uint32_t *seq,
                  uint32_t *frames, uint32_t *skipped, uint32_t *errors) {
    size_t off = 0;
    (void)skipped;
    if (type == BINARY_FRAME_TYPE_TMAG) tick += read_cost;
    if (mode == -1 && type == BINARY_FRAME_TYPE_TMAG) {
        (*errors)++;
        BinaryWriter_AppendError(out, cap, &off, (*seq)++, tick, 2, 1, 3);
    } else if (mode == -2 && type == BINARY_FRAME_TYPE_TMAG) {
        uint8_t payload = 0;
        BinaryWriter_AppendFrame(out, 0, &off, type, (*seq)++, tick, &payload, 1);
    } else if (mode == type) {
        /* TMAG's unchanged 10-byte payload: ch, addr, XYZ, status, overflow. */
        uint8_t payload[14] = {2, 0x34, 6, 0, 7, 0, 8, 0, 1, 0};
        uint16_t len = type == 1 ? 11 : type == 2 ? 10 : 14;
        if (BinaryWriter_AppendFrame(out, cap, &off, type, (*seq)++, tick, payload, len))
            (*frames)++;
    }
    return (int)off;
}
static int ICM42670_ReadToBinary(int *dev, uint8_t *out, size_t cap,
    uint32_t *seq, uint32_t *frames, uint32_t *skipped, uint32_t *errors) {
    (void)dev;
    return sample(3, out, cap, seq, frames, skipped, errors);
}
static int Sensor_AK09973D_ReadToBinary(uint8_t *out, size_t cap,
    uint32_t *seq, uint32_t *frames, uint32_t *skipped, uint32_t *errors) {
    return sample(1, out, cap, seq, frames, skipped, errors);
}
static int Sensor_TMAG3001_ReadAllToBinary(uint8_t *out, size_t cap,
    uint32_t *seq, uint32_t *frames, uint32_t *skipped, uint32_t *errors) {
    return sample(2, out, cap, seq, frames, skipped, errors);
}
static int USB_Send_Raw(uint8_t *buf, uint16_t len) {
    fwrite(buf, 1, len, wire);
    sends++;
    tick += usb_cost;
    return send_ok;
}
static void run_round(void) {
@LOOP@
}
int main(int argc, char **argv) {
    if (argc != 9) return 2;
    tick = (uint32_t)strtoul(argv[1], NULL, 0);
    mode = atoi(argv[2]); read_cost = atoi(argv[3]); usb_cost = atoi(argv[4]);
    phase_cost = atoi(argv[5]); send_ok = atoi(argv[6]);
    int rounds = atoi(argv[7]);
    wire = fopen(argv[8], "wb");
    if (!wire) return 3;
    for (int i = 0; i < rounds; i++) run_round();
    fclose(wire);
    printf("%u %u %d\n", tick, delay_total, sends);
    return 0;
}
'''


class EmptyRoundTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = ROOT / "build/verify-host"
        cls.directory.mkdir(parents=True, exist_ok=True)
        main = (ROOT / "Core/Src/main.c").read_text(encoding="utf-8")
        loop = main.split("/* USER CODE BEGIN 3 */", 1)[1].split("// LED 状态指示", 1)[0]
        period = re.search(r"#ifndef SENSOR_EMPTY_ROUND_PERIOD_MS\n.*?\n#endif", main, re.S).group()
        generated = cls.directory / "round.c"
        generated.write_text(period + "\n" + HARNESS.replace("@LOOP@", loop), encoding="utf-8")
        cls.executables = {}
        for value in (10, 25):
            exe = cls.directory / f"round-{value}.exe"
            command = ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-I" + str(ROOT / "tests/mock"), "-I" + str(ROOT / "Core/Inc"),
                       str(generated), str(ROOT / "Core/Src/binary_writer.c"), "-o", str(exe)]
            if value != 10:
                command.append(f"-DSENSOR_EMPTY_ROUND_PERIOD_MS={value}U")
            subprocess.run(command, check=True)
            cls.executables[value] = exe

    def run_round(self, mode=0, start=0, read=0, usb=0, phase=0, ok=1, rounds=1, period=10):
        wire = self.directory / "wire.bin"
        result = subprocess.run([str(self.executables[period]), str(start), str(mode),
                                 str(read), str(usb), str(phase), str(ok), str(rounds),
                                 str(wire)], check=True, capture_output=True, text=True)
        end, delay, sends = map(int, result.stdout.split())
        parser = reader.RoundParser()
        frames = parser.feed(wire.read_bytes())
        self.assertEqual(parser.crc_errors, 0)
        self.assertEqual(len(frames), sends)
        return end, delay, frames

    def test_no_devices_and_non_sensor_records(self):
        for mode, start in [(0, 0), (-1, 0), (0, 1000)]:
            with self.subTest(mode=mode, start=start):
                end, delay, frames = self.run_round(mode=mode, start=start)
                self.assertEqual((end, delay), (start + 10, 10))
                kinds = [r[0] for r in frames[0][2]]
                self.assertIn(4, kinds)
                if mode == -1:
                    self.assertIn(0xE0, kinds)
                if start == 1000:
                    self.assertIn(0xF0, kinds)

    def test_any_sensor_record_disables_delay(self):
        for mode in (1, 2, 3):
            with self.subTest(mode=mode):
                end, delay, frames = self.run_round(mode=mode, read=2, usb=1)
                self.assertEqual((end, delay), (3, 0))
                self.assertIn(mode, [r[0] for r in frames[0][2]])
                if mode == 2:
                    tmag = next(r[1] for r in frames[0][2] if r[0] == 2)
                    self.assertEqual(tmag[4:], bytes([2, 0x34, 6, 0, 7, 0, 8, 0, 1, 0]))
                    self.assertIn("TMAG", reader.decode_frame((2, 0, 0, tmag))[0])

    def test_elapsed_includes_phase_reads_and_usb(self):
        end, delay, _ = self.run_round(read=3, usb=2, phase=1)
        self.assertEqual((end, delay), (10, 4))
        for cost in (10, 11, 50):
            end, delay, _ = self.run_round(usb=cost)
            self.assertEqual((end, delay), (cost, 0))

    def test_usb_failure_and_next_round_error(self):
        end, delay, frames = self.run_round(start=990, usb=2, ok=0, rounds=2)
        self.assertEqual((end, delay), (1010, 16))
        self.assertEqual([f[0] for f in frames], [0, 1])
        self.assertIn(0xE0, [r[0] for r in frames[1][2]])
        stats = next(r[1] for r in frames[1][2] if r[0] == 0xF0)
        self.assertEqual(struct.unpack("<IIIIII", stats), (0, 0, 0, 0, 1, 0x12345678))
        end, delay, _ = self.run_round(mode=2, usb=2, ok=0)
        self.assertEqual((end, delay), (2, 0))

    def test_stats_remain_once_per_second(self):
        end, delay, frames = self.run_round(start=1000, rounds=102)
        self.assertEqual((end, delay), (2020, 1020))
        self.assertEqual([f[0] for f in frames], list(range(102)))
        self.assertEqual([f[1] for f in frames if any(r[0] == 0xF0 for r in f[2])],
                         [1000000, 2000000])

    def test_build_failure_still_limits_empty_round(self):
        end, delay, frames = self.run_round(mode=-2, read=3)
        self.assertEqual((end, delay, frames), (10, 7, []))

    def test_tick_wrap_and_period_override(self):
        end, delay, _ = self.run_round(start=0xFFFFFFFC, read=3, usb=2)
        self.assertEqual((end, delay), (6, 5))
        end, delay, _ = self.run_round(read=3, usb=2, period=25)
        self.assertEqual((end, delay), (25, 20))


if __name__ == "__main__":
    unittest.main()
