#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hle/hle.h"

namespace orchard
{
namespace
{
int access_violation_filter(unsigned code, EXCEPTION_POINTERS* info, uintptr_t* fault_addr)
{
    if (code != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    *fault_addr = uintptr_t(info->ExceptionRecord->ExceptionInformation[1]);
    return EXCEPTION_EXECUTE_HANDLER;
}
}

// no locals with destructors in here: __try refuses them (C2712)
bool call_guarded(HleFn fn, Cpu& cpu, uintptr_t* fault_addr)
{
    __try
    {
        fn(cpu);
        return true;
    }
    __except (access_violation_filter(GetExceptionCode(), GetExceptionInformation(), fault_addr))
    {
        return false;
    }
}

}
