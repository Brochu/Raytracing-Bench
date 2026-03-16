#pragma once

#include <windows.h>
#include <dxcapi.h>

IDxcBlob *shaders_compile_file(const wchar_t *path, const wchar_t *profile);
IDxcBlob *shaders_compile_file(const wchar_t *path, const wchar_t *entry, const wchar_t *profile);
void shaders_release();
