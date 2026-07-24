# VDD command utility

`vdd_command` sends UTF-16 commands through the ZakoVDD IOCTL device
interface.

```cmd
build\vdd_command.exe CREATEMONITOR
build\vdd_command.exe DESTROYMONITOR
build\vdd_command.exe HDR-ON \\.\DISPLAY7
build\vdd_command.exe HDR-OFF \\.\DISPLAY7
build\vdd_command.exe DISPLAY-LIST-ALL
build\vdd_command.exe DISPLAY-ACTIVATE-ZAKO
```

The HDR commands use Windows DisplayConfig directly and verify the effective
state after setting it. They do not send an HDR toggle to the driver.

`DISPLAY-LIST-ALL` prints active and inactive DisplayConfig paths, including
adapter/source/target IDs and monitor identity. `DISPLAY-ACTIVATE-ZAKO` keeps
the current active paths and adds the first connected `ZAK2333` target. The
latter can require elevation when Windows persists the topology.
