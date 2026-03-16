#include "bn_shaders.hpp"

#include <cstdio>

static IDxcCompiler3 *s_compiler = nullptr;
static IDxcUtils *s_utils = nullptr;

static bool ensure_dxc() {
    if (s_compiler) return true;
    HRESULT hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&s_compiler));
    if (FAILED(hr)) { printf("[ERR] DxcCreateInstance(compiler) (0x%08X)\n", (unsigned)hr); return false; }
    hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&s_utils));
    if (FAILED(hr)) { printf("[ERR] DxcCreateInstance(utils) (0x%08X)\n", (unsigned)hr); s_compiler->Release(); s_compiler = nullptr; return false; }
    return true;
}

IDxcBlob *shaders_compile_file(const wchar_t *path, const wchar_t *profile) {
    return shaders_compile_file(path, nullptr, profile);
}

IDxcBlob *shaders_compile_file(const wchar_t *path, const wchar_t *entry, const wchar_t *profile) {
    if (!ensure_dxc()) return nullptr;

    HRESULT hr;
    IDxcBlobEncoding *source = nullptr;
    hr = s_utils->LoadFile(path, nullptr, &source);
    if (FAILED(hr)) { printf("[ERR] Failed to load shader file (0x%08X)\n", (unsigned)hr); return nullptr; }

    const wchar_t *args[] = {
        L"-T", profile,
        L"-Zi",
        L"-E", entry,
    };
    const UINT32 arg_count = (entry) ? _countof(args) : _countof(args) - 2;

    DxcBuffer src_buf = {};
    src_buf.Ptr = source->GetBufferPointer();
    src_buf.Size = source->GetBufferSize();
    src_buf.Encoding = DXC_CP_ACP;

    IDxcResult *result = nullptr;
    hr = s_compiler->Compile(&src_buf, args, arg_count, nullptr, IID_PPV_ARGS(&result));

    IDxcBlobUtf8 *errors = nullptr;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (errors && errors->GetStringLength() > 0) {
        printf("[SHADER] %s\n", errors->GetStringPointer());
    }

    HRESULT compile_status;
    result->GetStatus(&compile_status);

    IDxcBlob *shader = nullptr;
    if (SUCCEEDED(compile_status)) {
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shader), nullptr);
    }

    if (errors) errors->Release();
    if (result) result->Release();
    source->Release();

    return shader;
}

void shaders_release() {
    if (s_utils) { s_utils->Release(); s_utils = nullptr; }
    if (s_compiler) { s_compiler->Release(); s_compiler = nullptr; }
}
