# GPU bugs

Minimal repros of GPU driver bugs, for the driver developers. All of them are currently in
AMD's proprietary Vulkan shader compiler.

Small C17 + Vulkan 1.3 programs, one per issue. Each one runs a compute or graphics shader on
data it generates, checks the result against a CPU reference, and prints `RESULT: PASS` or the
failure. Every SPIR-V module passes `spirv-val`, and the Khronos validation layer reports
nothing for any of them (05 `bad` crashes the driver before the layer can report). The
same programs pass on an NVIDIA GTX 1660 SUPER.

The bugs were found on a Radeon RX 6800 under Windows. Every case also reproduces with AMD's
proprietary Linux Vulkan driver on a Radeon Vega 11 (GCN 5): 02, 03 and 04 with the same
failure counts, while 01 crashes the compiler there instead of giving wrong values. The same
modules pass on that GPU with AMDVLK (LLPC) and with Mesa RADV. So the problems follow AMD's
proprietary shader compiler, across GPU generations and operating systems.

## Issues

| Case | Problem on the RX 6800 | Run |
|---|---|---|
| [01.amd_struct_pointer_spill](01.amd_struct_pointer_spill) | Function-storage struct of `PhysicalStorageBuffer` pointers: wrong results, ~1000x slower | `amd_struct_pointer_spill.exe --variant bad` |
| [02.amd_divergent_group_fence](02.amd_divergent_group_fence) | Workgroup-scope `OpMemoryBarrier` reached by some subgroups, then a workgroup control barrier: every value wrong | `amd_divergent_group_fence.exe --variant bad` |
| [03.amd_u16_push_pointer](03.amd_u16_push_pointer) | 16-bit load through a `uint16_t` pointer held in push constants returns 0 | `amd_u16_push_pointer.exe --variant bad` |
| [03.amd_u16_push_pointer](03.amd_u16_push_pointer#kernel-crashes-while-reducing) | while reducing 03, Windows crashed twice: bugchecks 0xD1 in amdkmdag.sys and 0x13A | not reproducible from this repo |
| [04.amd_bitcast_u16_pointer](04.amd_bitcast_u16_pointer) | 16-bit load through a pointer made by `OpBitcast` from a 64-bit integer reads wrong data | `amd_bitcast_u16_pointer.exe --variant bad` |
| [05.amd_pointer_cast_pipeline](05.amd_pointer_cast_pipeline) | `OpBitcast` of a `uint16_t` pointer from push constants to a `uint` pointer crashes `vkCreateComputePipelines` | `amd_pointer_cast_pipeline.exe --variant bad` |

Each case README has the results of every variant on every driver below, and, for 02, 03 and
04, the cause as it shows in the proprietary compiler's ISA (`--dump-isa`), with the dumps in
the case's `isa/` folder. 01 and 05 crash inside that compiler, so they have no ISA.

## Drivers tested

All on 2026-11-19.

| GPU | Driver | OS | Role |
|---|---|---|---|
| Radeon RX 6800 (RDNA2) | AMD Software 26.10.1 and 26.11.2 (same results), driverVersion 0x00800161 on both | Windows 11 build 26300 | where the bugs were found |
| Radeon Vega 11 (Raven Ridge, GCN 5) | AMD proprietary 23.30 (`vulkan-amdgpu-pro_23.30-1697785.22.04_amd64.deb`, amdgpu 5.7.3), driverVersion 0x0080011b, "AMD proprietary shader compiler" | Linux 7.3 | same compiler: every case reproduces |
| Radeon Vega 11 | AMDVLK 2023.Q3.3 (LLPC), driverVersion 2.0.285 | Linux 7.3 | control: every variant passes |
| Radeon Vega 11 | Mesa 26.2.6 RADV (ACO) | Linux 7.3 | control: every variant passes |
| GeForce GTX 1660 SUPER | NVIDIA 621.01 | Windows 11 build 26300 | control: every variant passes |

23.30 and AMDVLK 2023.Q3.3 are the last of each with GFX9 support. With the validation layer
(Vulkan SDK 1.4.357.0 on Linux) there are no messages on any of these. The modules built
with DXC and Slang on Linux are byte-identical to the checked-in ones.

## Build

Needs the Vulkan SDK (headers, loader, `dxc`, `slangc`, `spirv-val`), CMake 3.28+ and Ninja.
`build.bat` finds Visual Studio, sets up its environment and builds with MSVC; any C17
compiler works (for example `-DCMAKE_C_COMPILER=clang-cl`).

```
build.bat
```

Executables go to `.builds\default\`, SPIR-V to `.builds\default\shaders\`.

Without the shader compilers (any OS, Linux included), build only the programs and run them
on the checked-in modules:

```
cmake -S . -B .builds/linux -G Ninja -DREPRO_SHADERS=OFF
cmake --build .builds/linux
.builds/linux/amd_u16_push_pointer --variant bad --spv-dir 03.amd_u16_push_pointer/spirv
```

The `spirv/` folder of each case holds the exact modules behind its results (`.spv` plus
`spirv-dis` text).

To run on AMD's proprietary Linux driver without installing it, unpack `amdvlk64.so` from the
`vulkan-amdgpu-pro` package, write an ICD JSON whose `library_path` points at it, and run
with `VK_DRIVER_FILES=<that json>`.

## Run

```
.builds\default\amd_u16_push_pointer.exe --variant bad
```

| Argument | Meaning |
|---|---|
| `--device <index or name>` | GPU; default the first AMD GPU (env `REPRO_DEVICE` too) |
| `--variant <name>` | shader variant; each case's README lists them |
| `--spv-dir <dir>` | load SPIR-V from another directory |
| `--validation` | enable `VK_LAYER_KHRONOS_validation` |
| `--timeout <s>` | fence wait before reporting TIMEOUT, default 10 |
| `--repeat <n>` | submit the work n times |
| `--dump-isa <file>` | write each pipeline's statistics, IR and ISA to file (`VK_KHR_pipeline_executable_properties`) |

Output: OS build, GPU, driver name, info and version, then
`RESULT: PASS | WRONG | DEVICE_LOST | TIMEOUT | SETUP_FAILED | PIPELINE_FAILED`
(exit codes 0-5). On device loss it prints the `VK_EXT_device_fault` report.
