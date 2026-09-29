cmake_minimum_required(VERSION 3.25)
project(orchard LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
string(REPLACE "/Ob1" "/Ob2" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
string(REPLACE "/Ob1" "/Ob2" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")

set(Boost_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/external/boost)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A64" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/dynarmic EXCLUDE_FROM_ALL)

set(ORCHARD_FLAGS $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8 /wd4100 /D_CRT_SECURE_NO_WARNINGS>)

add_library(orchard_loader STATIC
    src/loader/macho.cpp
)
target_include_directories(orchard_loader PUBLIC src)
target_compile_options(orchard_loader PRIVATE ${ORCHARD_FLAGS})

add_library(orchard_core STATIC
    src/core/memory.cpp
    src/core/runtime.cpp
    src/core/threads.cpp
    src/core/vfs.cpp
    src/core/plist.cpp
    src/core/heap.cpp
    src/cpu/cpu.cpp
    src/hle/hle.cpp
    src/hle/guard.cpp
    src/host/window.cpp
    src/host/permission.cpp
    src/host/profiler.cpp
    src/host/memstats.cpp
    src/host/launcher.cpp
    src/host/compat.cpp
    src/loader/linker.cpp
    src/loader/dyld_cache.cpp
    src/loader/cache_images.cpp
    src/frameworks/libSystem/libsystem.cpp
    src/frameworks/libSystem/format.cpp
    src/frameworks/libSystem/pthread.cpp
    src/frameworks/libSystem/posix.cpp
    src/frameworks/libSystem/dyld.cpp
    src/frameworks/libSystem/mach.cpp
    src/frameworks/CoreFoundation/cf.cpp
    src/frameworks/libSystem/dispatch.cpp
    src/frameworks/libSystem/blocks.cpp
    src/frameworks/Foundation/foundation.cpp
    src/frameworks/Foundation/json.cpp
    src/frameworks/Foundation/nsstring.cpp
    src/frameworks/Foundation/nsstring_extra.cpp
    src/frameworks/Foundation/regex.cpp
    src/frameworks/Foundation/maptable.cpp
    src/frameworks/Foundation/collections.cpp
    src/frameworks/Foundation/plist_bridge.cpp
    src/frameworks/Foundation/system.cpp
    src/frameworks/Foundation/urlsession.cpp
    src/frameworks/Foundation/runtime_classes.cpp
    src/objc/runtime.cpp
    src/frameworks/UIKit/application.cpp
    src/frameworks/UIKit/views.cpp
    src/frameworks/UIKit/quartz.cpp
    src/frameworks/UIKit/runloop.cpp
    src/frameworks/UIKit/touch.cpp
    src/frameworks/UIKit/cg.cpp
    src/frameworks/Metal/astc.cpp
    src/frameworks/Metal/descriptors.cpp
    src/frameworks/Metal/metal.cpp
    src/frameworks/Metal/msl.cpp
    src/frameworks/Metal/render.cpp
    src/frameworks/SDKs/firebase.cpp
    src/frameworks/SystemConfiguration/reachability.cpp
    src/frameworks/Audio/audio.cpp
    src/objc/libobjc.cpp
    src/objc/nsobject.cpp
)
target_include_directories(orchard_core PUBLIC src)
target_link_libraries(orchard_core PUBLIC orchard_loader dynarmic d3d11 dxgi d3dcompiler windowscodecs ole32 winhttp comctl32 dbghelp psapi)
target_compile_options(orchard_core PRIVATE ${ORCHARD_FLAGS})

add_executable(orchard src/main.cpp)
target_link_libraries(orchard PRIVATE orchard_core)
target_compile_options(orchard PRIVATE ${ORCHARD_FLAGS})

add_executable(orchard-imports tools/imports/main.cpp)
target_link_libraries(orchard-imports PRIVATE orchard_loader)
target_compile_options(orchard-imports PRIVATE ${ORCHARD_FLAGS})

add_executable(orchard-cputest tools/cputest/main.cpp)
target_link_libraries(orchard-cputest PRIVATE orchard_core)
target_compile_options(orchard-cputest PRIVATE ${ORCHARD_FLAGS})

add_executable(orchard-dsc tools/dsc/main.cpp)
target_link_libraries(orchard-dsc PRIVATE orchard_core)
target_compile_options(orchard-dsc PRIVATE ${ORCHARD_FLAGS})

add_executable(orchard-unzbm tools/unzbm/main.cpp)
target_link_libraries(orchard-unzbm PRIVATE orchard_core)
target_compile_options(orchard-unzbm PRIVATE ${ORCHARD_FLAGS})

add_executable(orchard-msltest tools/msltest/main.cpp)
target_link_libraries(orchard-msltest PRIVATE orchard_core)
target_compile_options(orchard-msltest PRIVATE ${ORCHARD_FLAGS})
