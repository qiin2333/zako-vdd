# Configured native resolution in the built-in EDID

Each cached client EDID retains its built-in/custom source independently of the
currently loaded template. Reloading settings after a failed monitor removal
does not turn a cached custom EDID into a built-in one or overwrite its timing.

`EdidNativeResolution` is an explicit compatibility preference, independent of
the legacy/modern colour profile and the session's actual desktop resolution.
It is not an automatic selection of the largest mode or the current client size.

```xml
<EdidNativeResolution>default</EdidNativeResolution>
```

`default` or a missing value preserves the original 1920x1080 at 60 Hz first
detailed timing, even if that size is absent from the mode list. Alternatively,
set a size such as `3440x1440`, `2037x1109` or `3840x2160`. The selected size must
already be in the driver's current mode list. No extra modes are added.

For an explicit size, choose an encodable supported refresh rate nearest 60 Hz,
preferring the lower rate on a tie. Preserve the verified 1080p60 and 4K60 timing
presets; other sizes/rates use the same four-pixel blanking as
[DisplayModeHelpers::dispinfo](../ZakoVDD/Device/DisplayModeHelpers.h), with
one-pixel sync offsets/widths and the clock rounded to an EDID 10 kHz unit.
These are virtual-display timings, not physical-monitor CVT timings.

Active dimensions must be between 1 and 4095; the clock must fit the 16-bit
10 kHz field. Unavailable sizes, invalid values and unencodable rates are logged
and fall back to the original timing, including for cached clients. For example,
5120x1440 cannot fit this base-block descriptor; it must not be truncated into a
different size. Supporting such native dimensions would need a separate extended
descriptor design.

Only the first timing's first 12 bytes and the base-block checksum change.
Physical size, identity, borders, sync flags, other descriptors and every CTA,
HDR, colour and Dolby byte are preserved. A valid loaded custom EDID takes
precedence. Missing/invalid custom files use the configured built-in fallback.
The existing `EDIDNATIVERESOLUTION` registry lookup can override the XML value.

The setting is read when creating a monitor. Recreate the virtual display after
saving; existing live monitors are not modified in place. The driver mode list,
IddCx preferred index and session-resolution logic are unchanged.

If adding a new size or refresh rate as well, apply/reload the host's mode-list
configuration first. A host managing SETMODES can otherwise overwrite the live
driver list with its previous values on reconnect. In Foundation Sunshine, use
Apply after editing advertised resolutions/FPS, then reconnect; an EDID-only
change needs Save and monitor recreation.

The equivalent 4K60 custom EDID unlocked fullscreen 4K in local Windows 11 tests
of Oblivion Remastered and Sackboy: A Big Adventure with driver 100.0.18.1.
A local test build of this change on Windows 11 preserved the exact original
EDID by default and generated the equivalent 4K60 EDID with CustomEdid=false.
Sackboy offered and retained fullscreen 4K. A newly added 2736x1824 size was
applied through the companion Sunshine UI, exposed by Windows at 60/90/120/144 Hz
and advertised in the EDID at approximately 60 Hz. Sackboy's fullscreen menu then
offered only 2736x1824. Windows 10 remains untested in live use.
