// Headless host: load .aex, verify PiPL resource + EffectMain dispatch.
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include "AEConfig.h"
#include "A.h"
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"

typedef PF_Err (*EntryFn)(PF_Cmd, PF_InData*, PF_OutData*, PF_ParamDef**, PF_LayerDef*, void*);

int main() {
    HMODULE hm = LoadLibraryW(L"D:/AEplugin/build/plugin/NwRemover.aex");
    if (!hm) { std::printf("LoadLibrary FAILED err=%lu\n", GetLastError()); return 1; }
    std::printf("LoadLibrary OK\n");

    EntryFn fn = (EntryFn)GetProcAddress(hm, "EffectMain");
    if (!fn) { std::printf("GetProcAddress EffectMain FAILED\n"); return 2; }
    std::printf("EffectMain @ %p\n", (void*)fn);

    HRSRC hr = FindResourceW(hm, MAKEINTRESOURCEW(16000), L"PiPL");
    if (!hr) { std::printf("PiPL resource NOT FOUND err=%lu\n", GetLastError()); return 3; }
    DWORD sz = SizeofResource(hm, hr);
    HGLOBAL hg = LoadResource(hm, hr);
    const uint8_t* p = (const uint8_t*)LockResource(hg);
    uint16_t sig = *(uint16_t*)p;
    uint32_t ver = *(uint32_t*)(p + 2);
    uint32_t cnt = *(uint32_t*)(p + 6);
    std::printf("PiPL size=%lu sig=0x%04x version=%lu count=%lu\n", sz, sig, ver, cnt);
    std::printf("first prop vendor=%.4s key=%.4s\n", p + 10, p + 14);
    if (sig != 0x0001 || cnt != 12) { std::printf("PiPL HEADER BAD\n"); return 4; }

    PF_InData in; PF_OutData out;
    memset(&in, 0, sizeof(in)); memset(&out, 0, sizeof(out));
    PF_Err err = fn(PF_Cmd_GLOBAL_SETUP, &in, &out, nullptr, nullptr, nullptr);
    std::printf("GLOBAL_SETUP err=%d out_flags=0x%08lx out_flags2=0x%08lx my_version=%lu\n",
                (int)err, (unsigned long)out.out_flags, (unsigned long)out.out_flags2,
                (unsigned long)out.my_version);
    if (err != 0 || out.out_flags != 0x9C000000UL) { std::printf("GLOBAL_SETUP BAD\n"); return 5; }

    std::printf("HOST TEST PASS\n");
    FreeLibrary(hm);
    return 0;
}