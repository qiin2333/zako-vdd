# 社区脚本

这里保留一组小型 PowerShell 脚本，用于调整当前活动的 Zako 虚拟显示器。脚本通过驱动的 EDID 硬件标识 `DISPLAY\ZAK2333` 定位显示器，不再依赖旧的显示器名称。

## 准备工作

脚本要求 Windows PowerShell 5.1 或更高版本，以及 `DisplayConfig` 1.1.1 或更高版本。依赖需要由用户显式安装，脚本不会自行修改 PowerShell 仓库信任设置或安装模块：

```powershell
Install-Module DisplayConfig -Scope CurrentUser
```

进入脚本目录：

```powershell
Set-Location '.\Community Scripts'
```

## 脚本

| 脚本 | 用途 | 示例 |
| --- | --- | --- |
| `list-VDD.ps1` | 列出所有活动的 Zako 显示器及其 ID | `.\list-VDD.ps1` |
| `changeres-VDD.ps1` | 设置分辨率 | `.\changeres-VDD.ps1 2560 1440` |
| `refreshrate-VDD.ps1` | 设置刷新率，支持小数 | `.\refreshrate-VDD.ps1 120` |
| `rotate-VDD.ps1` | 设置 0、90、180 或 270 度旋转 | `.\rotate-VDD.ps1 90` |
| `scale-VDD.ps1` | 设置缩放比例或恢复推荐值 | `.\scale-VDD.ps1 150` |
| `primary-VDD.ps1` | 设为主显示器 | `.\primary-VDD.ps1` |
| `HDRswitch-VDD.ps1` | 开启、关闭或切换 HDR | `.\HDRswitch-VDD.ps1 -State Toggle` |

除 `list-VDD.ps1` 外，脚本默认操作 Display ID 最小的活动 Zako 显示器。如果同时启用了多个 Zako 显示器，先运行列表脚本，再通过 `-DisplayId` 指定目标：

```powershell
.\list-VDD.ps1
.\changeres-VDD.ps1 1920 1080 -DisplayId 2
.\HDRswitch-VDD.ps1 -State On -DisplayId 2
.\scale-VDD.ps1 -Reset -DisplayId 2
```

这些脚本只调整 Windows 中当前可用的显示模式。分辨率和刷新率必须已经由驱动配置提供，否则 Windows 会拒绝切换。一般显示设置不需要管理员权限。

## 设计边界

- 不在运行时安装依赖或更改 PSGallery 的信任状态。
- 不通过禁用 PnP 设备来开关虚拟显示器。
- 不提供会隐式改变所有显示器拓扑的全局“复制/扩展”切换。
- `common.ps1` 是其余脚本共享的内部辅助文件，不需要直接运行。
