#include <cstdio>
#include <d3dcompiler.h>

int main() {
    printf("[RTBench] Starting...\n");

    ID3DBlob *data;
    HRESULT r = D3DCreateBlob(1024, &data);
    if (r != S_OK) {
        printf("ERR: Could not access D3DCompiler's function!\n");
        return -1;
    }
    r = D3DReadFileToBlob(L"./build.bat", &data);
    if (r != S_OK) {
        printf("ERR: Could not read file into reserved blob\n");
        return -1;
    }

    printf("read file :\n");
    printf("%s", (char*)data->GetBufferPointer());
    return 0;
}
