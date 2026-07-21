# Vulkan HDR probe

`vulkan_hdr_probe` diagnoses the Vulkan WSI gap seen on HDR-capable IddCx
displays. It only modifies system state for explicit cache-writing or
per-user implicit-layer registration commands.

Build from an x64 Visual Studio 2022 developer prompt:

```cmd
build.bat
```

List active displays and their HDR state:

```cmd
build\vulkan_hdr_probe.exe --list
```

Test whether the ICD accepts missing HDR10/scRGB pairs and write the keyed
capability cache:

```cmd
build\vulkan_hdr_probe.exe --display \\.\DISPLAY2 --gpu 0 --force-create ^
  --cache ..\vulkan_hdr_bridge\build\zako_vulkan_hdr_capabilities.bin
```

Perform real acquire/clear/submit/present work and hold a known scRGB patch for
the VDD shared-frame consumer:

```cmd
build\vulkan_hdr_probe.exe --display \\.\DISPLAY2 --present scrgb ^
  --frames 600 --hold-ms 30000 --clear 0 12.5 0 ^
  --cache ..\vulkan_hdr_bridge\build\zako_vulkan_hdr_capabilities.bin
```

Inspect or mark eligible entries after external pixel verification:

```cmd
build\vulkan_hdr_probe.exe --show-cache ^
  ..\vulkan_hdr_bridge\build\zako_vulkan_hdr_capabilities.bin

build\vulkan_hdr_probe.exe --mark-pixels-verified ^
  ..\vulkan_hdr_bridge\build\zako_vulkan_hdr_capabilities.bin
```

Register or remove the bridge for the current Windows user. Removal matches
the manifest filename, so it also cleans paths left by an older installation:

```cmd
build\vulkan_hdr_probe.exe --register-implicit-layer C:\path\VkLayer_zako_virtual_hdr.json
build\vulkan_hdr_probe.exe --unregister-implicit-layer VkLayer_zako_virtual_hdr.json
```

HDR10 present submits BT.2020 mastering metadata. The default HDR10 patch is
the PQ code value for 1000 nits; the default scRGB patch is `12.5`, the Windows
linear scRGB value corresponding to 1000 nits.

The cache key includes Vulkan vendor/device IDs, driver/API versions, adapter
LUID, and Windows build. `--mark-pixels-verified` only marks entries that
already contain both HDR-active validation and a successful present result.
