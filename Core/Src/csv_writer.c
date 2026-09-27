#include "csv_writer.h"

int CSV_AppendChar(char *out, size_t out_size, size_t *off, char c)
{
    if (*off >= out_size) {
        return 0;
    }
    out[(*off)++] = c;
    return 1;
}

int CSV_AppendString(char *out, size_t out_size, size_t *off, const char *s)
{
    while (*s != '\0') {
        if (!CSV_AppendChar(out, out_size, off, *s++)) {
            return 0;
        }
    }
    return 1;
}

int CSV_AppendU32(char *out, size_t out_size, size_t *off, uint32_t value)
{
    char tmp[10];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && n < (int)sizeof(tmp));

    while (n > 0) {
        if (!CSV_AppendChar(out, out_size, off, tmp[--n])) {
            return 0;
        }
    }
    return 1;
}

int CSV_AppendI32(char *out, size_t out_size, size_t *off, int32_t value)
{
    uint32_t magnitude;

    if (value < 0) {
        if (!CSV_AppendChar(out, out_size, off, '-')) {
            return 0;
        }
        magnitude = (uint32_t)(-(value + 1)) + 1U;
    } else {
        magnitude = (uint32_t)value;
    }

    return CSV_AppendU32(out, out_size, off, magnitude);
}

int CSV_AppendHex8(char *out, size_t out_size, size_t *off, uint8_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    return CSV_AppendString(out, out_size, off, "0x") &&
           CSV_AppendChar(out, out_size, off, hex[(value >> 4) & 0x0F]) &&
           CSV_AppendChar(out, out_size, off, hex[value & 0x0F]);
}

int CSV_AppendHex32(char *out, size_t out_size, size_t *off, uint32_t value)
{
    static const char hex[] = "0123456789ABCDEF";

    if (!CSV_AppendString(out, out_size, off, "0x")) {
        return 0;
    }

    for (int shift = 28; shift >= 0; shift -= 4) {
        if (!CSV_AppendChar(out, out_size, off, hex[(value >> shift) & 0x0FU])) {
            return 0;
        }
    }
    return 1;
}

int CSV_AppendCRLF(char *out, size_t out_size, size_t *off)
{
    return CSV_AppendString(out, out_size, off, "\r\n");
}

int CSV_AppendPhase(char *out, size_t out_size, size_t *off,
                    const uint32_t phase_us[3])
{
    return CSV_AppendString(out, out_size, off, "PHASE,") &&
           CSV_AppendU32(out, out_size, off, phase_us[0]) &&
           CSV_AppendChar(out, out_size, off, ',') &&
           CSV_AppendU32(out, out_size, off, phase_us[1]) &&
           CSV_AppendChar(out, out_size, off, ',') &&
           CSV_AppendU32(out, out_size, off, phase_us[2]) &&
           CSV_AppendCRLF(out, out_size, off);
}
