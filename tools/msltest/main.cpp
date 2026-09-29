#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "frameworks/Metal/msl.h"

namespace fs = std::filesystem;
using namespace orchard::metal;

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: orchard-msltest <dir> [entry]\n");
        return 2;
    }
    std::string entry = argc > 2 ? argv[2] : "xlatMtlMain";
    int ok = 0, failed = 0;
    for (auto& e : fs::directory_iterator(argv[1]))
    {
        if (e.path().extension() != ".metal") continue;
        std::ifstream f(e.path(), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        msl::Result r = msl::translate(ss.str(), entry, {});
        std::string name = e.path().filename().string();
        if (const char* show = std::getenv("MSLTEST_PRINT"); show && name == show) std::printf("%s\n", r.hlsl.c_str());
        if (!r.ok)
        {
            std::printf("%-20s TRANSLATE FAILED: %s\n", name.c_str(), r.error.c_str());
            ++failed;
            continue;
        }
        ID3DBlob *code = nullptr, *err = nullptr;
        HRESULT hr = D3DCompile(r.hlsl.data(), r.hlsl.size(), name.c_str(), nullptr, nullptr, "main", r.vertex ? "vs_5_0" : "ps_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
        if (FAILED(hr))
        {
            std::string msg = err ? std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()) : "?";
            std::printf("%-20s COMPILE FAILED:\n%s\n", name.c_str(), msg.substr(0, 600).c_str());
            std::ofstream(e.path().string() + ".hlsl") << r.hlsl;
            ++failed;
        }
        else
        {
            ++ok;
        }
        if (code) code->Release();
        if (err) err->Release();
    }
    std::printf("\n%d compiled, %d failed\n", ok, failed);
    return failed ? 1 : 0;
}
