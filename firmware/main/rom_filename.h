#pragma once
#include <stdbool.h>
#include <stddef.h>
bool rom_filename_valid(const char *name);
bool rom_filename_decode_hex(const char *hex, char *name, size_t capacity);
void rom_filename_display(const char *name, char *output, size_t capacity);
