#pragma once

#include <stddef.h>
#include <stdint.h>

int CSV_AppendChar(char *out, size_t out_size, size_t *off, char c);
int CSV_AppendString(char *out, size_t out_size, size_t *off, const char *s);
int CSV_AppendU32(char *out, size_t out_size, size_t *off, uint32_t value);
int CSV_AppendI32(char *out, size_t out_size, size_t *off, int32_t value);
int CSV_AppendHex8(char *out, size_t out_size, size_t *off, uint8_t value);
int CSV_AppendHex32(char *out, size_t out_size, size_t *off, uint32_t value);
int CSV_AppendCRLF(char *out, size_t out_size, size_t *off);
int CSV_AppendPhase(char *out, size_t out_size, size_t *off,
                    const uint32_t phase_us[3]);
