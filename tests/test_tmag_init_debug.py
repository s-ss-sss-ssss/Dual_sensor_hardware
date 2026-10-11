"""Exercise the actual TMAG driver and main.c printf-to-USB routing on host."""
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class TmagInitDebugTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = ROOT / "build/verify-tmag-debug-host"
        cls.directory.mkdir(parents=True, exist_ok=True)
        cls.drivers = {}
        for debug in (0, 1):
            exe = cls.directory / f"driver-{debug}.exe"
            subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "tests/mock"), "-I" + str(ROOT / "Core/Inc"),
                            f"-DTMAG_INIT_DEBUG={debug}", str(ROOT / "tests/test_tmag_init_debug.c"),
                            str(ROOT / "Core/Src/tmag3001.c"), "-o", str(exe)], check=True)
            cls.drivers[debug] = exe

        source = (ROOT / "Core/Src/main.c").read_text(encoding="utf-8")
        write = source.split("int _write(int file, char *ptr, int len)", 1)[1].split(
            "/* USER CODE END 0 */", 1)[0]
        routing = cls.directory / "routing.c"
        routing.write_text('''
#include <stdint.h>
#include <stdlib.h>
#include "binary_writer.h"
volatile uint8_t g_tmag_init_debug;
static int huart1, usb_calls, uart_calls;
static void USB_Send_PrintBuffer(char *ptr, int len) {
    (void)ptr; (void)len; usb_calls++;
}
static int HAL_UART_Transmit(int *uart, uint8_t *ptr, int len, int timeout) {
    (void)uart; (void)ptr; (void)len; (void)timeout; uart_calls++; return 0;
}
int _write(int file, char *ptr, int len)
''' + write + '''
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    g_tmag_init_debug = (uint8_t)atoi(argv[1]);
    char text[] = "TMAG INIT ASCII\\r\\n";
    int len = (int)sizeof(text) - 1;
    int result = _write(1, text, len);
    return !(result == len && usb_calls == atoi(argv[2]) && uart_calls == 1);
}
''', encoding="utf-8")
        cls.routes = {}
        for output_format in (0, 1):
            exe = cls.directory / f"routing-{output_format}.exe"
            subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "Core/Inc"),
                            f"-DSENSOR_OUTPUT_FORMAT={output_format}", str(routing),
                            "-o", str(exe)], check=True)
            cls.routes[output_format] = exe

    def run_driver(self, mode=0, debug=-1, build_debug=1):
        return subprocess.run([str(self.drivers[build_debug]), str(mode), str(debug)],
                              check=True, capture_output=True, text=True).stdout

    def test_default_off_and_runtime_switch(self):
        self.assertEqual(self.run_driver(build_debug=0), "RESULT=0\n")
        self.assertEqual(self.run_driver(debug=0), "RESULT=0\n")
        self.assertIn("VERIFY OK", self.run_driver(debug=1, build_debug=0))

    def test_success_steps(self):
        output = self.run_driver()
        self.assertIn("BOOT WRITE reg=0x00 wr=0x00 hal=0", output)
        self.assertIn("MFG try=1 hal=0 err=0x00000000 id=0x5449", output)
        self.assertIn("VERIFY OK reg=0x02 wr=0x70 rd=0x70", output)
        self.assertIn("VERIFY OK reg=0x01 wr=0x02 rd=0x02", output)
        self.assertIn("READY OK status=0x11", output)
        self.assertTrue(output.endswith("RESULT=0\n"))

    def test_failure_steps_and_original_errors(self):
        cases = {
            1: "MFG try=15 hal=1 err=0x00000004 id=0xFFFF",
            2: "VERIFY FAIL reg=0x02 wr=0x70 rd=0x00",
            3: "WRITE FAIL reg=0x02 wr=0x70 hal=1 err=0x00000004",
            4: "READBACK FAIL reg=0x02 wr=0x70 hal=1 err=0x00000020",
            5: "READY READ FAIL hal=1 err=0x00000001",
            6: "READY TIMEOUT status=0x10 timeout_ms=5",
        }
        for mode, expected in cases.items():
            with self.subTest(mode=mode):
                output = self.run_driver(mode=mode)
                self.assertIn(expected, output)
                self.assertTrue(output.endswith("RESULT=1\n"))
                self.assertEqual(self.run_driver(mode=mode, debug=0), "RESULT=1\n")

    def test_boot_write_logged_without_changing_retry_flow(self):
        output = self.run_driver(mode=7)
        self.assertIn("BOOT WRITE reg=0x00 wr=0x00 hal=1 err=0x00000004", output)
        self.assertTrue(output.endswith("RESULT=0\n"))

    def test_printf_usb_routing(self):
        for output_format in (0, 1):
            for debug in (0, 1):
                expected_usb = int(output_format == 0 or debug == 1)
                with self.subTest(output_format=output_format, debug=debug):
                    subprocess.run([str(self.routes[output_format]), str(debug),
                                    str(expected_usb)], check=True)


if __name__ == "__main__":
    unittest.main()
