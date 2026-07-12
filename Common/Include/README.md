# Shared headers

This directory contains contracts shared across the driver and companion
tools:

- `vdd_control_ioctl.h`: ZakoVDD control and sealed frame-channel IOCTL ABI.
- `AdapterOption.h`: adapter-selection state shared by driver modules.
- `vulkan_hdr_policy.h`: strict HDR surface-format injection policy.
- `vulkan_hdr_capability_cache.h`: versioned GPU/driver/OS validation cache.

Keep only cross-component contracts here. Driver-internal implementation
details belong under `ZakoVDD`.
