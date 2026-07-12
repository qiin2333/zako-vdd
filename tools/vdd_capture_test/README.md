# VDD shared-frame capture test

`vdd_capture_test` consumes the frame ring exported by ZakoVDD without DXGI
Desktop Duplication or Windows Graphics Capture. It opens the metadata mapping,
frame-ready event, and keyed-mutex D3D11 textures for one VDD monitor.

Build from an x64 Visual Studio 2022 developer prompt:

```cmd
build.bat
```

Capture five frames:

```cmd
build\vdd_capture_test.exe --monitor 0 --frames 5 --out build\capture
```

Verify a known scRGB pixel without writing large raw frame files:

```cmd
build\vdd_capture_test.exe --monitor 0 --frames 3 --sample 100 100 ^
  --expect-rgb 0 12.5 0 --tolerance 0.5 --no-dump
```

Important options:

- `--monitor N`: internal ZakoVDD monitor index, default `0`.
- `--frames N`: number of frames, default `5`.
- `--timeout MS`: per-frame timeout, default `2000`.
- `--sample X Y`: decode and print one pixel from an RGBA16F frame.
- `--expect-scrgb V`: expect the same scRGB value in R, G, and B.
- `--expect-rgb R G B`: expect independent scRGB channel values.
- `--tolerance V`: absolute per-channel tolerance, default `0.25`.
- `--no-dump`: validate metadata/pixels without writing PPM/raw frames.

The tool exits nonzero if no frame is captured or no sampled frame matches the
requested value. Pixel verification is intended to run while
`vulkan_hdr_probe --present scrgb --clear ...` holds a visible patch on the
active HDR VDD.

The compatibility named-object channel may require an elevated consumer. The
production Sunshine path should use the sealed IOCTL frame channel instead.
