# ZakoVDD Logging

ZakoVDD uses TraceLogging ETW as the only driver logging backend.

- Provider name: `ZakoTech.VDD`
- Provider GUID: `{B254994F-46E6-4719-80A0-0A3AA50D6CE5}`
- Event name: `Log`
- Event fields: `Level`, `Category`, `Message`, `SourceFile`, `Function`, `Line`

Capture a trace:

```powershell
$traceDir = Join-Path $env:TEMP "ZakoVDD"
New-Item -ItemType Directory -Force -Path $traceDir | Out-Null
$etlPath = Join-Path $traceDir "ZakoVDDTrace.etl"
$csvPath = Join-Path $traceDir "ZakoVDDTrace.csv"

logman start ZakoVDDTrace -ets -p "{B254994F-46E6-4719-80A0-0A3AA50D6CE5}" 0xFFFFFFFF 0x5 -o $etlPath
# Reproduce the issue.
logman stop ZakoVDDTrace -ets
tracerpt $etlPath -of CSV -o $csvPath
```

ETW levels follow the Windows convention: error `2`, warning `3`, info `4`, verbose/debug `5`.
The driver does not select a storage location, write log files, or stream logs through the legacy control pipe.
The `.etl` path belongs to the capture session. Keep traces in a non-shared support directory and delete them after use, because logs can contain device names, configuration values, and timing information.
