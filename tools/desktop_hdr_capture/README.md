# Desktop HDR capture diagnostic

`desktop_hdr_capture` tests whether DXGI Desktop Duplication can acquire an
FP16 desktop image from an active HDR display. It is a diagnostic fallback,
not the VDD production capture path.

```cmd
build\desktop_hdr_capture.exe \\.\DISPLAY7 100 100 5000
```

IddCx outputs commonly return `DXGI_ERROR_UNSUPPORTED` from
`DuplicateOutput1`. Use `vdd_capture_test` and the driver's shared-frame
channel for the authoritative VDD pixel check.
