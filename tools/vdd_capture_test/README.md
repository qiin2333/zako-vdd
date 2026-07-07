# vdd_capture_test

`vdd_capture_test` 是独立 consumer，用于验证 ZakoVDD 的
`SharedFrameExporter` 可以把 IddCx 帧导出给外部进程，不经过 DXGI Desktop
Duplication 或 Windows Graphics Capture。

## 工作原理

驱动会为每个虚拟显示器发布以下共享对象：

| 资源 | 命名 |
| --- | --- |
| 带 keyed mutex 的 D3D11 共享纹理 | `Global\ZakoVDD_Frame_<idx>` |
| 帧就绪事件 | `Global\ZakoVDD_FrameReady_<idx>` |
| 元数据 file mapping | `Global\ZakoVDD_Meta_<idx>` |

每帧从 IddCx 收到的 surface 会被复制到共享纹理。本工具打开这些共享对象，
通过 keyed mutex 同步，然后把帧 dump 成 PPM 文件做视觉验证。

## 编译

打开 **x64 Native Tools Command Prompt for VS 2022**，然后运行：

```cmd
build.bat
```

输出文件为 `build\vdd_capture_test.exe`。

## 使用

运行前需要满足：

1. 已安装包含 `SharedFrameExporter` 的 ZakoVDD 构建。
2. 至少创建了一个虚拟显示器。
3. 虚拟显示器处于活动状态，确保 DWM 会持续推帧。

```cmd
build\vdd_capture_test.exe --monitor 0 --frames 5 --out .
```

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--monitor N` | `0` | ZakoVDD 内部 monitor index。 |
| `--frames N` | `5` | 要捕获的帧数。 |
| `--out DIR` | `.` | PPM 帧文件输出目录。 |
| `--timeout MS` | `2000` | 每帧等待超时。 |

## 期望输出

```text
[vdd_capture_test] monitor=0 frames=5 out=. timeout=2000ms
[meta] 1920x1080 fmt=BGRA8(87) hdr=0 maxNits=0.0 frameCounter=42
[shared] tex 1920x1080 fmt=BGRA8(87) bind=0x28 misc=0x802
[frame   0] dumped=OK path=./vdd_frame_000.ppm frameCounter=43 (+1)
[frame   1] dumped=OK path=./vdd_frame_001.ppm frameCounter=44 (+1)
[done] captured=5 in 0.083s (60.0 fps)
```

## 故障排查

- `OpenFileMappingW failed: 2`：驱动未运行、未安装 exporter 构建，或选中的
  monitor 未处于活动状态。
- `OpenFileMappingW failed: 5`：当前进程没有访问权限。检查共享对象 SDDL；
  默认授权 system、administrators 和 interactive users。
- `OpenSharedResourceByName failed: 0x80070057`：工具使用的 D3D11 设备 LUID
  和驱动 render adapter LUID 不一致。
- 持续 timeout 通常表示虚拟显示器没有收到帧。把可见窗口移动到该显示器上，
  或把它设为扩展桌面。
