# Virtual Display Driver (HDR)

专为 [Foundation Sunshine](https://github.com/qiin2333/foundation-sunshine) 设计的 Windows 虚拟显示器驱动，提供完整的 HDR 支持和灵活的虚拟显示器管理功能。

## 🌟 特性

- **HDR 支持** - 完整的 HDR (High Dynamic Range) 支持，提供真正的 HDR 游戏串流体验
- **虚拟显示器管理** - 动态创建和管理多个虚拟显示器，无需物理显示器
- **灵活配置** - 支持自定义分辨率、刷新率、颜色格式等
- **EDID 支持** - 支持自定义 EDID，可模拟特定显示器的特性
- **硬件加速** - 利用 GPU 硬件加速进行渲染
- **低延迟** - 优化的渲染管线，提供低延迟的虚拟显示体验
- **多显示器支持** - 支持同时创建和管理多个虚拟显示器
- **社区脚本** - 提供丰富的 PowerShell 脚本用于快速管理虚拟显示器

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
   - 或使用提供的 PowerShell 脚本进行快速配置

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

## 🛠️ 社区脚本

项目提供了丰富的 PowerShell 脚本用于管理虚拟显示器：

- `toggle-VDD.ps1` - 启用/禁用虚拟显示器
- `changeres-VDD.ps1` - 更改虚拟显示器分辨率
- `refreshrate-VDD.ps1` - 更改刷新率
- `rotate-VDD.ps1` - 旋转虚拟显示器
- `scale-VDD.ps1` - 调整缩放比例
- `primary-VDD.ps1` - 设置为主显示器
- `HDRswitch-VDD.ps1` - 切换 HDR 模式
- `get_disp_num.ps1` - 获取显示器编号

> **注意**: 使用这些脚本需要管理员权限，并且需要对 PowerShell 有一定的了解。

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

2. 使用 Visual Studio 打开 `Virtual Display Driver (HDR)/ZakoVDD/ZakoVDD.sln`

3. 选择配置（Debug 或 Release）和平台（x64）

4. 构建解决方案

### 项目结构

```
Virtual-Display-Driver/
├── Virtual Display Driver (HDR)/
│   ├── ZakoVDD/              # 主驱动代码
│   │   ├── Driver.cpp        # 驱动主逻辑
│   │   ├── Driver.h          # 驱动头文件
│   │   └── ZakoVDD.inf       # 驱动安装文件
│   ├── vdd_settings.xml      # 默认配置文件
│   └── *.edid                # EDID 文件
├── Community Scripts/        # PowerShell 管理脚本
├── Common/                   # 公共头文件
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

