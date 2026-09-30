# Orchard

Orchard is an hobby project, please do not expect updates too frequently

Orchard is an iOS emulator for Windows written in C++ that runs 64-bit iOS apps by recompiling their ARM64 code with dynarmic and implementing the parts of iOS they call into, similar to how Wine runs Windows programs on Linux, and since it is in early development its behavior can change a lot between builds

Orchard is heavily AI assisted, but it is tested by humans and built under human direction and cooperation

> [!IMPORTANT]
> Orchard is not affiliated with Apple and does not distribute apps or Apple system software, so only use apps and firmware files you have obtained legally

## Current status

- Crossy Road 7.13.2 is playable with touch and saves, and it can go online
- Subway Surfers 3.69.1 is playable too, though it can freeze for a moment when a run starts
- Sound works in Unity games, while the other iOS audio APIs are in but not tested in a game yet
- Apps that need iOS 17 or newer don't run yet, since Orchard uses iOS 16.7 system files
- Graphics go through a Metal to Direct3D 11 layer, so games that render with OpenGL ES or depend on compute shaders will not display correctly
- The first launch of a game stutters while its shaders compile, but they are cached on disk so later launches are smooth

## Bugs and issues

Expect crashes, and when a game stops Orchard shows the function or message it failed on, which you should include along with the game version when reporting an issue

## How it works

Orchard reserves a 64 GB block of address space and loads the app's Mach-O binaries into it with Apple's `dyld_shared_cache` mapped alongside, where libraries that only compute, such as libc++ and the Swift runtime, run as Apple's own code while libraries that talk to the operating system have their entry points patched to trap into Orchard, which implements them on Windows

From there the app starts the way it would on a phone (initializers, `main()`, `UIApplicationMain`, then a run loop with a 60 Hz display link that the game renders from), with mouse clicks and drags in the window passed to the game as touches and network requests going through WinHTTP once you allow them

Important areas of the codebase:

- `src/cpu`: dynarmic JIT setup and guest threads
- `src/loader`: Mach-O loading and the dyld shared cache
- `src/objc`: the Objective-C runtime
- `src/frameworks`: Foundation, CoreFoundation, libSystem, UIKit, audio, networking
- `src/frameworks/Metal`: Metal on Direct3D 11, including the MSL to HLSL shader translator and an ASTC decoder
- `src/host`: the window, IPA launching, the network prompt and diagnostics

## Building

### Requirements

- Windows 10 or later, with a Direct3D 11 capable GPU
- Visual Studio 2022 with the Desktop development with C++ workload

### Dependencies

Place these in `external/`:

- dynarmic (Vita3K fork) in `external/dynarmic`
- Boost 1.86 headers in `external/boost/boost`

Then run:

```
build.cmd
```

The binaries are placed in `build/`

## iOS system files

Orchard needs the dyld shared cache from iOS 16.7.16 for the iPhone X (build 20H392), so extract `dyld_shared_cache_arm64` and its numbered subcache files from the firmware, for example with [ipsw](https://github.com/blacktop/ipsw), into a `cache` folder next to `orchard.exe`:

```
cache\dyld_shared_cache_arm64
```

For a source build `external/ios-16.7.16/cache/` also works, and if the extracted files are still LZBITMAP compressed you can decompress them with `orchard-unzbm`

## Running

Open a decrypted IPA with Orchard:

```
orchard.exe CrossyRoad.ipa
```

To open IPAs by double clicking them, add Orchard to the Open with list once:

```
orchard.exe --register
```

`--unregister` removes it again

A window opens with the game's launch screen and the game takes over once it has loaded, and an unpacked `.app` folder works in place of an IPA too

Save data goes in `userdata\<bundle id>` next to the `cache` folder (the repository root for a source build) along with `userdata\network.txt` for your network choices, while unpacked IPAs and compiled shaders are kept in `%LOCALAPPDATA%\Orchard`

Options:

- `--network allow|deny`: answer the network prompt in advance
- `--cache <file>`: path to `dyld_shared_cache_arm64`, if it is not in the default location
- `--screenshot <file.png>`: save the last frame when Orchard exits
- `--quit-after <seconds>`: exit after the given time
- `--lenient`: continue past unimplemented functions instead of stopping

To test a whole folder of IPAs at once, `orchard.exe --compat <folder> [seconds]` runs each one and writes `compat.md` into that folder, showing how far every game got and which missing functions it hit most

For debugging:

- `--hitches`: when a frame takes longer than 0.3 seconds, print what each thread was doing
- `--memstats`: print a memory breakdown every 10 seconds
- `--profile <from>:<to>`: sample all threads between two times and print where the time went
- `--tap <seconds>:<x>,<y>` and `--swipe <seconds>:<x1>,<y1>,<x2>,<y2>`: scripted input, in iPhone points

## Tools

- `orchard-msltest`: translates and compiles a folder of Metal shaders, for testing the shader translator
- `orchard-dsc`: finds which library and export an address in the cache belongs to
- `orchard-imports`: lists the system symbols an app imports
- `orchard-unzbm`: decompresses LZBITMAP compressed cache files
- `orchard-cputest`: JIT smoke test
- `orchard-audiotest`: decodes a sound file and prints its format and level, `--play` plays it
