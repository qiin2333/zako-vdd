# Zako VDD

专为 [Foundation Sunshine](https://github.com/qiin2333/foundation-sunshine) 设计的 Windows 虚拟显示器驱动，提供完整的 HDR 支持和灵活的虚拟显示器管理功能。

## 🌟 特性

- **HDR 支持** - 完整的 HDR (High Dynamic Range) 支持，提供真正的 HDR 游戏串流体验
- **虚拟显示器管理** - 动态创建和管理多个虚拟显示器，无需物理显示器
- **灵活配置** - 支持自定义分辨率、刷新率、颜色格式等
- **EDID 支持** - 支持自定义 EDID，可模拟特定显示器的特性
- **硬件加速** - 利用 GPU 硬件加速进行渲染
- **低延迟** - 优化的渲染管线，提供低延迟的虚拟显示体验
- **多显示器支持** - 支持同时创建和管理多个虚拟显示器
- **社区脚本** - 提供范围明确的 PowerShell 脚本用于调整活动的虚拟显示器

## 📋 系统要求

- **操作系统**: Windows 10 22H2 或更高版本
- **架构**: x64
- **GPU**: 支持 DirectX 11 的显卡
- **权限**: 管理员权限（用于安装驱动）

## 🚀 快速开始

### 安装步骤

1. **下载驱动**
   - 从 [Releases](https://github.com/qiin2333/zako-vdd/releases) 下载最新版本的驱动包

2. **安装证书**
   - 以管理员身份运行 `*.bat` 文件，将驱动证书添加为受信任的根证书

3. **安装驱动**
   - 打开设备管理器
   - 点击任意设备，然后点击 "操作" 菜单 → "添加过时硬件"
   - 选择 "从列表中选择硬件（高级）"
   - 选择 "显示适配器"
   - 点击 "从磁盘安装..." → "浏览..."
   - 导航到解压的文件并选择 `ZakoVDD.inf` 文件
   - 完成安装

4. **配置虚拟显示器**
   - 编辑 `vdd_settings.xml` 文件来自定义虚拟显示器的设置
   - 使用社区 PowerShell 脚本调整当前活动显示器的分辨率、刷新率、缩放、旋转和 HDR 状态

### 配置文件说明

驱动配置文件位于 `C:\VirtualDisplayDriver\vdd_settings.xml`（或系统盘根目录下的 `VirtualDisplayDriver` 文件夹）。

主要配置项包括：

- **显示器数量** - 设置要创建的虚拟显示器数量
- **分辨率** - 自定义支持的分辨率列表
- **刷新率** - 全局刷新率设置，将应用到所有分辨率
- **HDR 支持** - 启用/禁用 HDR 功能
- **颜色格式** - 支持 RGB、YCbCr444、YCbCr422、YCbCr420
- **EDID** - 自定义 EDID 支持
- **日志** - 启用/禁用日志记录

示例配置：

```xml
<vdd_settings>
    <monitors>
        <count>1</count>
    </monitors>
    <gpu>
        <friendlyname>default</friendlyname>
    </gpu>
    <resolutions>
        <resolution>
            <width>1920</width>
            <height>1080</height>
            <refresh_rate>60</refresh_rate>
        </resolution>
    </resolutions>
</vdd_settings>
```

## 📺 支持的显示器参数

### 分辨率支持

驱动支持自定义任意分辨率，默认预设包括：

| 分辨率 | 说明 |
|--------|------|
| 800×600 | 基础分辨率 |
| 1366×768 | 常见笔记本分辨率 |
| 1920×1080 | Full HD (1080p) |
| 2560×1440 | Quad HD (1440p) |
| 3840×2160 | Ultra HD (4K) |

> **注意**: 您可以在配置文件中添加任意自定义分辨率，支持的最大分辨率取决于您的 GPU 和系统配置。

### 刷新率支持

#### 全局刷新率（推荐）
全局刷新率会应用到所有配置的分辨率上，支持的刷新率包括：

- **60 Hz** - 标准刷新率
- **90 Hz** - 高刷新率
- **120 Hz** - 高刷新率
- **144 Hz** - 游戏高刷新率
- **165 Hz** - 游戏高刷新率
- **240 Hz** - 超高刷新率

#### 单独刷新率
每个分辨率可以单独配置刷新率，支持范围：**30 Hz - 240 Hz**

> **提示**: 使用全局刷新率配置可以快速为所有分辨率添加多个刷新率选项。

### 颜色格式支持

驱动支持以下颜色格式：

| 格式 | 说明 | 适用场景 |
|------|------|----------|
| **RGB** | 红绿蓝三原色（默认） | 通用，最佳兼容性 |
| **YCbCr444** | 4:4:4 色度采样 | 高质量视频，无压缩 |
| **YCbCr422** | 4:2:2 色度采样 | 视频处理，中等质量 |
| **YCbCr420** | 4:2:0 色度采样 | 视频压缩，节省带宽 |

> **注意**: 如果配置了无效的颜色格式，驱动会自动回退到 RGB 格式。

### HDR 支持

#### HDR 模式
- **HDR Plus (12-bit)** - 12 位色深，提供最佳的 HDR 体验
- **SDR 10-bit** - 10 位色深，标准动态范围

> **重要**: HDR Plus 和 SDR 10-bit 不能同时启用，因为它们存在冲突。启用 HDR Plus 时，SDR 10-bit 会自动禁用。

#### 色域支持
- **高色域 (High Color Space)** - 支持更广的色域范围
- **宽色域 (Wide Color Space)** - 支持宽色域显示

### 光标支持

#### 硬件光标
- **启用/禁用** - 可选择使用硬件光标或软件光标
- **最大尺寸** - 支持最大 128×128 像素
  - 较旧的 Intel CPU 可能限制为 64×64
- **Alpha 通道支持** - 支持 32 位 Alpha 光标格式
- **XOR 光标支持** - 完整的 XOR 光标支持

> **提示**: 如果禁用硬件光标，串流应用将使用客户端光标。

### EDID 支持

- **自定义 EDID** - 支持加载自定义 EDID 文件（`user_edid.bin`）
- **CEA 扩展覆盖** - 可以覆盖或添加 CEA 扩展块以启用 HDR
- **制造商信息** - 可选择防止制造商信息伪造

> **注意**: 自定义 EDID 不支持模拟分辨率，仅用于显示特性模拟。

### 多显示器支持

- **同时创建多个虚拟显示器** - 支持创建多个虚拟显示器实例
- **独立配置** - 每个虚拟显示器可以独立配置
- **动态管理** - 支持运行时创建和销毁虚拟显示器

### 性能参数

实际支持的最大参数取决于：

- **GPU 性能** - 显卡的计算能力
- **显存大小** - 可用显存容量
- **系统配置** - CPU、内存等系统资源
- **Windows 版本** - 不同 Windows 版本可能有不同的限制

> **建议**: 对于游戏串流，推荐使用 1920×1080 @ 60-144 Hz 或 2560×1440 @ 60-120 Hz 以获得最佳性能和画质平衡。

## 🛠️ 社区脚本

[`Community Scripts`](./Community%20Scripts/) 提供一组范围明确的 PowerShell 脚本，用于调整活动的 Zako 虚拟显示器：

- 列出 Zako 显示器及 Display ID
- 更改分辨率和刷新率
- 调整旋转和缩放比例
- 设置主显示器
- 开启、关闭或切换 HDR

脚本使用 `DISPLAY\ZAK2333` 硬件标识定位当前驱动创建的显示器，并支持通过 `-DisplayId` 选择多个虚拟显示器中的某一个。脚本依赖 `DisplayConfig` 1.1.1 或更高版本：

```powershell
Install-Module DisplayConfig -Scope CurrentUser
Set-Location '.\Community Scripts'
.\list-VDD.ps1
.\changeres-VDD.ps1 2560 1440 -DisplayId 2
```

完整命令、参数和限制请参阅[社区脚本说明](./Community%20Scripts/README.md)。脚本不会自动安装依赖、禁用 PnP 设备或隐式修改全局显示拓扑。

## 🔧 与 Foundation Sunshine 集成

本驱动专为 Foundation Sunshine 设计，提供以下优势：

1. **无缝集成** - 与 Sunshine 的虚拟显示器功能完美配合
2. **HDR 支持** - 支持 Sunshine 的 HDR 串流功能
3. **动态管理** - 支持 Sunshine 动态创建和销毁虚拟显示器
4. **性能优化** - 针对游戏串流场景进行了优化

### 使用建议

1. 安装本驱动后，Sunshine 可以自动检测并使用虚拟显示器
2. 在 Sunshine 配置中启用虚拟显示器功能
3. 根据需要调整 `vdd_settings.xml` 中的配置
4. 使用社区脚本快速调整虚拟显示器设置

## 📖 开发文档

### 构建要求

- Visual Studio 2019 或更高版本
- Windows Driver Kit (WDK)
- Windows SDK

### 构建步骤

1. 克隆仓库
```bash
git clone https://github.com/qiin2333/zako-vdd.git
cd Virtual-Display-Driver
```

2. 使用 Visual Studio 打开仓库根目录下的 `ZakoVDD.sln`

3. 选择配置（Debug 或 Release）和平台（x64）

4. 构建解决方案

### 项目结构

```
Virtual-Display-Driver/
├── ZakoVDD.sln               # Visual Studio 解决方案
├── ZakoVDD/                  # 主驱动工程
│   ├── Adapter/              # GPU 适配器选择
│   ├── Callbacks/            # IddCx 回调实现
│   ├── Config/               # 配置读取和写入
│   ├── Control/              # 控制通道和命令处理
│   ├── Device/               # WDF/IddCx 设备生命周期
│   ├── Rendering/            # D3D 和交换链处理
│   └── ZakoVDD.inf           # 驱动安装文件
├── vdd_settings.xml          # 默认配置文件
├── Community Scripts/        # PowerShell 管理脚本
├── Common/Include/           # 公共头文件和 IOCTL 协议
├── tools/vdd_capture_test/   # 共享帧导出验证工具
└── README.md                 # 本文件
```

## 🐛 故障排除

### 常见问题

1. **驱动无法安装**
   - 确保已正确安装证书
   - 检查是否以管理员权限运行
   - 确认系统版本符合要求（Windows 10 22H2+）

2. **虚拟显示器不显示**
   - 检查设备管理器中驱动是否正常加载
   - 查看 Windows 显示设置
   - 检查 `vdd_settings.xml` 配置是否正确

3. **HDR 不工作**
   - 确认在配置文件中启用了 HDR 支持
   - 检查 GPU 是否支持 HDR
   - 查看日志文件获取详细信息

4. **性能问题**
   - 尝试调整分辨率或刷新率
   - 检查 GPU 驱动是否为最新版本
   - 查看系统资源使用情况

### 日志

启用日志功能可以帮助诊断问题：

1. 编辑 `vdd_settings.xml`
2. 设置 `<logging>true</logging>`
3. 日志文件将保存在配置目录中

> **警告**: 长时间启用日志（特别是调试日志）可能导致日志文件过大。

## 📝 许可证

本项目采用 MIT 许可证。详情请参阅 [LICENSE](LICENSE) 文件。

## 🤝 贡献

欢迎提交 Issue 和 Pull Request！

在提交问题或功能请求时，请：

1. 检查是否已有相关 Issue
2. 提供详细的系统信息和错误描述
3. 如果可能，附上日志文件

## 🔗 相关链接

- [Foundation Sunshine](https://github.com/qiin2333/foundation-sunshine) - 游戏串流主机
- [Moonlight](https://moonlight-stream.org/) - 游戏串流客户端
- [IDD (Indirect Display Driver) 文档](https://docs.microsoft.com/en-us/windows-hardware/drivers/display/indirect-display-driver-model-overview)

## ⚠️ 免责声明

本驱动为社区项目，使用需自行承担风险。请确保在测试环境中充分测试后再用于生产环境。

