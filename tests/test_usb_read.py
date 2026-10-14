import importlib.util
import struct
import sys
import types
import unittest
from pathlib import Path

serial = types.ModuleType("serial")
serial.tools = types.ModuleType("serial.tools")
serial.tools.list_ports = types.ModuleType("serial.tools.list_ports")
sys.modules.update({"serial": serial, "serial.tools": serial.tools,
                    "serial.tools.list_ports": serial.tools.list_ports})
spec = importlib.util.spec_from_file_location("usb_read", Path(__file__).resolve().parents[1] / "tools" / "usb_read.py")
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)


def record(kind, body):
    return struct.pack("<BH", kind, len(body)) + body


def frame(seq, records):
    payload = b"".join(records)
    content = struct.pack("<BBIIH", 2, 0x10, seq, 123, len(payload)) + payload
    return reader.SYNC + content + struct.pack("<H", reader.crc16_ccitt_false(content))


class ProtocolTests(unittest.TestCase):
    def test_full_and_sparse_rounds(self):
        ak = record(1, struct.pack("<IBBhhhBBB", 10, 1, 2, 3, 4, 5, 1, 0, 0))
        tmag = record(2, struct.pack("<IBBhhhBB", 20, 2, 0x34, 6, 7, 8, 1, 0))
        icm = record(3, struct.pack("<Ihhhhhhh", 30, *range(7)))
        phase = record(4, struct.pack("<III", 1, 2, 0xFFFFFFFF))
        err = record(0xE0, struct.pack("<BHI", 2, 1, 3))
        stats = record(0xF0, struct.pack("<IIIIII", 1, 2, 3, 4, 5, 0x12345678))
        full = frame(1, [ak, tmag, icm, phase, err, stats])
        sparse = frame(2, [phase])
        for split in range(len(full) + 1):
            parser = reader.RoundParser()
            rounds = parser.feed(full[:split]) + parser.feed(full[split:] + sparse)
            self.assertEqual([r[0] for r in rounds], [1, 2])
            self.assertEqual(len(rounds[0][2]), 6)
        self.assertIn("t_us=123", reader.decode_frame((1, 1, 123, ak[3:]))[2])
        self.assertIn("done_us=10", reader.decode_frame((1, 1, 123, ak[3:]))[2])
        self.assertIn("device_id=0x12345678", reader.decode_frame((0xF0, 1, 123, stats[3:]))[2])
        legacy = reader.decode_frame((0xF0, 1, 123, struct.pack("<IIIII", 1, 2, 3, 4, 5)))
        self.assertEqual(legacy[0], "STATS")
        self.assertIn("errors=5", legacy[2])

    def test_crc_resync_unknown_and_boundary(self):
        # The firmware's 4096-byte frame limit permits 4080 payload bytes.
        max_frame = frame(3, [record(0x77, b"x" * 4077)])
        self.assertEqual(len(max_frame), 4096)
        bad = bytearray(frame(1, [record(4, b"\0" * 12)]))
        bad[-1] ^= 1
        parser = reader.RoundParser()
        result = []
        for byte in b"ASCII" + bad + max_frame:
            result.extend(parser.feed(bytes([byte])))
        self.assertEqual([r[0] for r in result], [3])
        self.assertEqual(result[0][2][0][0], 0x77)
        self.assertEqual(parser.crc_errors, 1)

    def test_bad_record_length(self):
        malformed = frame(1, [struct.pack("<BH", 1, 9) + b"x"])
        self.assertEqual(reader.RoundParser().feed(malformed), [])


if __name__ == "__main__":
    unittest.main()
