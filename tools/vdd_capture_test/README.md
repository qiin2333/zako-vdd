# vdd_capture_test

独立 consumer，用于验证 ZakoVDD 的 SharedFrameExporter 可以把 IddCx 帧导出给外部进程，**不经过 DXGI Desktop Duplication / WGC**。

## 工作原理

ZakoVDD 驱动里的 `SharedFrameExporter` 为每个虚拟显示器创建：

| 资源 | 命名 |
|---|---|
| D3D11 共享纹理（NT shared handle + keyed mutex） | `Global\ZakoVDD_Frame_<idx>` |
| 帧就绪事件 | `Global\ZakoVDD_FrameReady_<idx>` |
| 元数据 file mapping | `Global\ZakoVDD_Meta_<idx>` |

每帧 IddCx 拿到的 surface 经过 `CopyResource` 推到共享纹理；本工具打开它们，用 keyed mutex 同步，然后 dump 出 PPM 文件做视觉验证。

## 编译

打开 **x64 Native Tools Command Prompt for VS 2022**，进入本目录：

```cmd
build.bat
```

输出：`build\vdd_capture_test.exe`

## 使用

前置：
1. ZakoVDD 驱动已经更新到带 SharedFrameExporter 的版本并部署
2. VDD 创建了至少一个虚拟显示器
3. 该虚拟显示器**正在被使用**（被 OS 拉去显示画面，否则 IddCx 不会推帧）—— 可以把它设为扩展屏，鼠标拖个窗口过去

```cmd
build\vdd_capture_test.exe --monitor 0 --frames 5 --out .
```

参数：

| 参数 | 默认 | 说明 |
|---|---|---|
| `--monitor N` | 0 | VDD 内部 monitor index |
| `--frames N` | 5 | 抓多少帧 |
| `--out DIR` | `.` | 帧文件输出目录 |
| `--timeout MS` | 2000 | 等帧超时 |

## 期望输出

```
[vdd_capture_test] monitor=0 frames=5 out=. timeout=2000ms
[meta] 1920x1080 fmt=BGRA8(87) hdr=0 maxNits=0.0 frameCounter=42
[shared] tex 1920x1080 fmt=BGRA8(87) bind=0x28 misc=0x802
[frame   0] dumped=OK path=./vdd_frame_000.ppm frameCounter=43 (+1)
[frame   1] dumped=OK path=./vdd_frame_001.ppm frameCounter=44 (+1)
...
[done] captured=5 in 0.083s (60.0 fps)
```

## 故障排查

- `OpenFileMappingW failed: 2 (driver running? monitor active?)`  
  → 驱动没跑、未更新到带 exporter 的版本，或 monitor 没在工作（没在显示画面）

- `OpenFileMappingW failed: 5 (Access denied)`  
  → 当前用户既不是 admin 也不是 interactive user。检查 SDDL ACL（默认 `D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)`）

- `OpenSharedResourceByName failed: 0x80070057 (E_INVALIDARG)`  
  → 工具用的 D3D11 设备 LUID 和 VDD RenderAdapter LUID 不一致（笔记本 hybrid GPU）。后续 Sunshine 集成时会做 LUID 协商。

- 抓不到帧（一直 timeout）  
  → 虚拟显示器没在被推帧。把鼠标拖一个窗口到那块虚拟屏上让 DWM 持续合成。
