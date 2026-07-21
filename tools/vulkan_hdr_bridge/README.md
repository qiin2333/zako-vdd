# Zako Vulkan HDR bridge

This is an enumeration-only Vulkan HDR WSI bridge. It is intentionally not
installed or registered globally by the VDD package. The streaming host owns
session-scoped registration and cleanup.

The bridge injects the HDR10 and scRGB format/color-space pairs only when:

- the Vulkan surface maps to a Win32 window;
- that window is on a display identified as ZakoVDD;
- Windows reports HDR as supported and enabled; and
- the GPU/driver/Windows capability cache contains a verified entry.

It compares complete `(format, colorSpace)` pairs and is otherwise a strict
pass-through layer. `DISABLE_ZAKO_VIRTUAL_HDR=1` disables it through the
standard implicit-layer manifest switch.

The bridge reads `%LOCALAPPDATA%\Sunshine\zako_vulkan_hdr_capabilities.bin`
by default, falling back to the DLL directory when `LOCALAPPDATA` is absent.
Override that location with `ZAKO_VHDR_CAPABILITY_CACHE`. Entries are keyed by
Vulkan vendor/device IDs, driver/API version, adapter LUID, and Windows build.

Development-only overrides:

- `ZAKO_VHDR_ALLOW_ANY_HDR=1` permits any active HDR display.
- `ZAKO_VHDR_FORCE_SURFACE=1` bypasses display checks but still requires a
  matching cache entry.
- `ZAKO_VHDR_FORCE=1` bypasses display identity and HDR-state checks.

Local closure on AMD Radeon 780M / Windows build 26200 established that:

- the raw ICD returned five SDR pairs;
- the bridge returned seven pairs including exact HDR10 and scRGB pairs;
- a scRGB swapchain presented 600 frames successfully; and
- the VDD RGBA16F channel read a requested `(0, 12.5, 0)` patch as
  `(0.0083, 12.4531, 0.0073)` on three consecutive frames.

Foundation Sunshine now packages all three artifacts, validates presentation,
uses session-scoped per-user registration, cleans normal and crashed sessions,
and exposes Automatic/Disabled plus live status in WebUI.

The minimal local Vulkan ABI should be replaced by pinned Khronos
Vulkan-Headers before broad distribution.
