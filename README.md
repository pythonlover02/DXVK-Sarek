> [!NOTE]
> Bug reports and pull requests are welcome, but please understand that development happens in my free time and progress may be slow at times. The project is still maintained even if the last commit was made a while ago.

# DXVK-Sarek

**Vulkan 1.1/1.2 based implementation of D3D3, 5, 6, 7, 8, 9, 10 and 11 for Linux/Wine/Proton.**

This repository exists to support users with Vulkan capable GPUs that don't meet the requirements of current upstream builds. The goal: make sure everyone benefits from the performance of DXVK, even on slightly older hardware. That means creating or backporting QoL patches, fixes, and per-game configurations from the latest versions to the 1.10.x branch and a little more on top.

The project is officially supported on [proton-cachyos](https://github.com/CachyOS/proton-cachyos). To enable it there, add this to your launch options:

```
PROTON_DXVK_SAREK=1
```

Full credit goes to doitsujin / ドイツ人 (Philip Rebohle) and everyone who has worked on the DXVK project. The original DXVK repository is [here](https://github.com/doitsujin/dxvk).

A huge thank-you to the following contributors for their invaluable help:

- [Blisto91](https://github.com/Blisto91)
- [AmerXz](https://github.com/AmerXz)
- [Gcenx](https://github.com/Gcenx)
- [WinterSnowfall](https://github.com/WinterSnowfall)

## Table of Contents

- [Proton-Sarek: discontinued](#proton-sarek-discontinued)
- [ARM Emulation & Mobile GPUs](#arm-emulation--mobile-gpus-box64-fex-mali-adreno)
- [How to Use](#how-to-use)
- [Build Instructions](#build-instructions)
- [Logs](#logs)
- [HUD](#hud)
- [Frame Rate Limit](#frame-rate-limit)
- [Device Filter](#device-filter)
- [State Cache](#state-cache)
- [Shader Compilation (dyasync)](#shader-compilation-dyasync)
- [Frame Pacing (low-latency mode)](#frame-pacing-low-latency-mode)
- [Debugging](#debugging)
- [Troubleshooting](#troubleshooting)

## Proton-Sarek: discontinued

Proton-Sarek is officially discontinued. To be upfront about the reasoning so there are no doubts: I currently work, go to university, and take care of my little brothers from time to time, so I do not have a lot of free time, let alone time to dedicate to open-source projects (which I still really enjoy as a hobby). Keeping up with Proton upstream while also working on DXVK-Sarek and handling issues for both projects simply isn't sustainable for me anymore.

Thankfully, I'm on really good terms with the Proton-CachyOS developer. He's a good guy, and a while ago he already added DXVK-Sarek support since he has a laptop that needs it, so the integration was already done by the time I decided to drop Proton-Sarek. From now on, he will handle the Proton side of things and I will focus solely on DXVK-Sarek and my other small projects.

CachyOS is also one of the few distros that officially still supports some of the older drivers DXVK-Sarek targets, like `nvidia-470` (available on the official servers). So it's genuinely a great fit.

Every change I made here had a reason, and dropping Proton-Sarek frees me up to keep improving DXVK-Sarek itself, as I'm already doing with dyasync.

That said, this is not a blanket "no" to other Proton builds. If Valve, GE, or anyone else wants to ship DXVK-Sarek in their releases, they'll have my full support on the DXVK-Sarek side of any issues that come up. I just wanted to drop the Proton part personally. For now, **Proton-CachyOS is the only officially supported way to use DXVK-Sarek with Proton.**

## ARM Emulation & Mobile GPUs (Box64, FEX, Mali, Adreno)

DXVK-Sarek includes fixes that allow the project to run on mobile and ARM translation layers (Box64, FEX, Android PC emulators, etc.).

- Certain Vulkan extensions have been made optional so the project can run on Mali GPUs.
- @zeyadadev fixed a Mali GPU black screen caused by unbound texture optimization (#36) thanks pal.

> [!WARNING]
> **Important Note for Mobile GPU Users.** I am not against marking some Vulkan extensions as optional to make certain GPUs run DXVK-Sarek. But please understand that even if it runs now, the experience will most likely not be ideal. You might encounter visual bugs or stuttering. If you run into issues on a GPU that was allowed to run it this way (Adreno and Mali), check on other Vulkan-compatible hardware before reporting the issue, as it's more likely related to your Vulkan drivers.
>
> **If you are using a Mali GPU, do not report issues related to performance or visual artifacts.** These problems are almost certainly caused by the GPU's lack of support for critical Vulkan extensions. While I have made efforts to allow Mali GPUs to run the project, I cannot provide fixes or support for these devices.

**For developers and contributors:** my only rule is that Mali / other-device fixes don't affect other devices. That way you don't have to worry too much about what upstream does, and I don't have to manually cherry-pick patches from your fork. If you're alright with it, just open PRs against the repo once you think things are ready. If not, that's fine too I wanted to extend the offer, since contributing upstream means the fixes reach more users through official releases and the work gets properly credited there.

## How to Use

Follow the official guide from the [upstream DXVK README](https://github.com/doitsujin/dxvk?tab=readme-ov-file#how-to-use).

Keep in mind this is a manual installation method, which isn't the most convenient. An easier approach is to use a Linux game launcher such as Lutris or Heroic. There you can simply select DXVK-Sarek as the DXVK version (for Wine) or Proton-CachyOS with `PROTON_DXVK_SAREK=1` (for Proton).

If they're not available by default, you can install them via [ProtonPlus](https://flathub.org/apps/com.vysp3r.ProtonPlus) or [ProtonUpQT](https://flathub.org/apps/net.davidotek.pupgui2).

## Build Instructions

To pull in all submodules needed for building, clone the repository with:

```
git clone --branch main --recurse https://github.com/pythonlover02/DXVK-Sarek.git DXVK
```

### Requirements

- [wine 7.1](https://www.winehq.org/) or newer
- [Meson](https://mesonbuild.com/) build system (at least version 0.49)
- [Mingw-w64](https://www.mingw-w64.org) compiler and headers (at least version 10.0)
- [glslang](https://github.com/KhronosGroup/glslang) compiler

### Building DLLs

The simple way, inside the DXVK directory, run:

```
./package-release.sh master /your/target/directory --no-package
```

This creates a folder `dxvk-master` in `/your/target/directory` containing both 32-bit and 64-bit versions of DXVK, which can be set up the same way as the release versions.

To preserve the build directories for development, pass `--dev-build` to the script. This option implies `--no-package`. After making changes to the source, rebuild with:

```
# change to build.32 for 32-bit
cd /your/target/directory/build.64
ninja install
```

### Compiling manually

```
# 64-bit build. For 32-bit builds, replace
# build-win64.txt with build-win32.txt
meson setup --cross-file build-win64.txt --buildtype release --prefix /your/dxvk/directory build.w64
cd build.w64
ninja install
```

The D3D9, D3D10, D3D11 and DXGI DLLs are located in `/your/dxvk/directory/bin`. Setup has to be done manually in this case.

## Logs

When used with Wine, DXVK prints log messages to `stderr`. Standalone log files can optionally be generated by setting the `DXVK_LOG_PATH` variable; log files in the given directory will be called `app_d3d11.log`, `app_dxgi.log` etc., where `app` is the name of the game executable.

On Windows, log files are created in the game's working directory by default, which is usually next to the game executable.

## HUD

The `DXVK_HUD` environment variable controls a HUD that can display the framerate and some stat counters. It accepts a comma-separated list of the following options:

- `devinfo` displays the name of the GPU and the driver version
- `fps` shows the current frame rate
- `frametimes` shows a frame time graph
- `submissions` shows the number of command buffers submitted per frame
- `drawcalls` shows the number of draw calls and render passes per frame
- `pipelines` shows the total number of graphics and compute pipelines
- `memory` shows the amount of device memory allocated and used
- `gpuload` shows estimated GPU load (may be inaccurate)
- `version` shows DXVK version
- `api` shows the D3D feature level used by the application
- `cs` shows worker thread statistics
- `compiler` shows shader compiler activity
- `samplers` shows the current number of sampler pairs used *(D3D9 only)*
- `scale=x` scales the HUD by a factor of `x` (e.g. `1.5`)
- `opacity=y` adjusts HUD opacity by a factor of `y` (e.g. `0.5`, `1.0` being fully opaque)

`DXVK_HUD=1` is equivalent to `DXVK_HUD=devinfo,fps`, and `DXVK_HUD=full` enables all available HUD elements.

## Frame Rate Limit

The `DXVK_FRAME_RATE` environment variable can be used to limit the frame rate. A value of `0` uncaps the frame rate; any positive value limits rendering to that number of frames per second. The configuration file can be used as an alternative.

## Device Filter

Some applications do not provide a method to select a different GPU. In that case, DXVK can be forced to use a given device:

- `DXVK_FILTER_DEVICE_NAME="Device Name"` selects devices with a matching Vulkan device name, which can be retrieved with tools such as `vulkaninfo`. Matches on substrings, so "VEGA" or "AMD RADV VEGA10" is supported if the full device name is "AMD RADV VEGA10 (LLVM 9.0.0)", for example. If the substring matches more than one device, the first device matched is used.

> [!NOTE]
> If the device filter is configured incorrectly, it may filter out all devices and applications will be unable to create a D3D device.

## State Cache

DXVK caches pipeline state by default, so shaders can be recompiled ahead of time on subsequent runs of an application, even if the driver's own shader cache got invalidated in the meantime. This cache is enabled by default and generally reduces stuttering.

- `DXVK_STATE_CACHE=0` disables the state cache
- `DXVK_STATE_CACHE_PATH=/some/directory` specifies a directory for the cache files. Defaults to the current working directory of the application.

## Shader Compilation (dyasync)

DXVK-Sarek includes **dyasync (Dynamic Asynchronous Pipeline Compilation)**, enabled by default.

When a shader is encountered for the very first time, it must be compiled synchronously this is unavoidable and may cause a brief stutter. Every variant after that is handled differently. A variant is created whenever the game uses the same shaders with a different combination of fixed-function state (blend mode, depth test, cull mode, render pass, etc.); each unique combination counts as a new variant.

When a new variant is needed, dyasync does not stall the game to compile it. Instead, it grabs the closest already-compiled pipeline for those same shaders (perhaps one compiled with different blend settings) and uses it as a placeholder while the correct variant builds in a background thread. Once the background compilation finishes, it silently swaps in the correct pipeline. This reduces stuttering and improves frametimes.

This approach is safer than the traditional async patch because something valid is always being rendered on screen there are no invisible or missing objects. That said, during the brief placeholder period, minor visual inaccuracies are possible (e.g. slightly wrong blending). **Use in multiplayer games at your own discretion.**

Dyasync can be disabled by setting `dxvk.enableDyasync = False` in `dxvk.conf`, in the `DXVK_CONFIG` environment variable, or by using the environment variable `DXVK_DISABLE_DYASYNC=1`.

`DXVK_ALL_CORES=1` overrides the default way cores are assigned for shader compilation. By default, DXVK-Sarek uses roughly half the available CPU cores for background compilation, leaving the rest free for the game. On CPUs with weak per-core performance that rely on all cores for good throughput, this may cause longer loading times. With `DXVK_ALL_CORES=1`, all available cores are used for both the game and shader compilation. This may cause brief unresponsiveness while compiling shaders but can improve the overall experience on such hardware.

## Frame Pacing (low-latency mode)

DXVK-Sarek includes an optional frame pacing mode adapted from and inspired by [netborg-afps/dxvk-low-latency](https://github.com/netborg-afps/dxvk-low-latency). It is not a direct port: that project relies on per submission GPU timing and an asynchronous presenter that upstream DXVK has but Sarek Vulkan 1.1/1.2-targeted presenter does not, so the mechanism here has been adapted to what Sarek can actually observe.

The `dxvk.framePace` option in `dxvk.conf` (or the `DXVK_FRAME_PACE` environment variable) controls it:

- `"max-frame-latency"` (default) is Sarek's existing, unchanged behaviour. Frame `i` wont start until frame `(i-1)-x` has finished, where `x` is `dxgi.maxFrameLatency` / `d3d9.maxFrameLatency`.
- `"low-latency"` forces the effective frame latency down to the minimum needed for forward progress, and makes a best-effort prediction of when the previous frame will finish (based on a rolling average of recent frame durations) to reduce unnecessary CPU wake-up jitter. The prediction is never load-bearing for correctness; a wrong guess costs microseconds, not a broken frame.
- `"min-latency"` forces the same minimal frame latency without the predictive sleep. Lowest possible latency, usually at a noticeable fps cost.

`dxvk.lowLatencyOffset` fine-tunes `"low-latency"` mode: positive values (in microseconds) delay a frame's predicted start slightly, negative values start it earlier. Clamped to `-10000`..`10000`, defaults to `0`.

> [!NOTE]
> Because Sarek presenter has no per-submission GPU progress telemetry the way upstream DXVK's does, `"low-latency"` and `"min-latency"` currently share the same underlying latency-reduction mechanism rather than being two fully distinct algorithms. There's also no VRR-aware pacing mode and no HUD latency display yet, both of which the upstream project has.

## Debugging

The following environment variables can be used for debugging:

- `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` enables Vulkan debug layers. Highly recommended for troubleshooting rendering issues and driver crashes. Requires the Vulkan SDK to be installed on the host system.
- `DXVK_LOG_LEVEL=none|error|warn|info|debug` controls message logging
- `DXVK_LOG_PATH=/some/directory` changes the path where log files are stored. Set to `none` to disable log file creation entirely, without disabling logging.
- `DXVK_CONFIG_FILE=/xxx/dxvk.conf` sets the path to the configuration file
- `DXVK_CONFIG="dxgi.hideAmdGpu = True; dxgi.syncInterval = 0"` sets config variables through the environment instead of a configuration file, using the same syntax (`;` is the separator)
- `DXVK_PERF_EVENTS=1` enables use of the `VK_EXT_debug_utils` extension for translating performance event markers

## Troubleshooting

DXVK requires threading support from your mingw-w64 build environment. If this is missing, you may see `error: 'std::cv_status' has not been declared` or similar threading-related errors.

On Debian and Ubuntu this can be resolved by using the posix alternate, which supports threading. Choose the posix alternate from these commands:

```
update-alternatives --config x86_64-w64-mingw32-gcc
update-alternatives --config x86_64-w64-mingw32-g++
update-alternatives --config i686-w64-mingw32-gcc
update-alternatives --config i686-w64-mingw32-g++
```

For non-Debian-based distros, make sure your mingw-w64-gcc cross compiler has `--enable-threads=posix` enabled during configure. If your distro ships its mingw-w64-gcc binary with `--enable-threads=win32`, you may have to recompile locally or open a bug at your distro's bug tracker to ask for it.
