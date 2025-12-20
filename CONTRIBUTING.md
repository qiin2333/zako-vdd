# 贡献指南

感谢您对 Zako VDD 项目的关注！我们欢迎各种形式的贡献。

## 如何贡献

### 报告 Bug

如果您发现了 Bug，请：

1. 检查 [Issues](https://github.com/qiin2333/zako-vdd/issues) 中是否已有相关报告
2. 如果没有，请使用 [Bug Report 模板](https://github.com/qiin2333/zako-vdd/issues/new?template=bug_report.yml) 创建新 Issue
3. 提供尽可能详细的信息，包括：
   - 操作系统版本
   - 驱动版本
   - 复现步骤
   - 日志文件（如果可能）

### 提出功能建议

如果您有功能建议，请：

1. 检查是否已有相关 Issue 或讨论
2. 使用 [Feature Request 模板](https://github.com/qiin2333/zako-vdd/issues/new?template=feature_request.yml) 创建新 Issue
3. 详细描述功能需求和使用场景

### 提交代码

1. **Fork 仓库**
   ```bash
   git clone https://github.com/your-username/zako-vdd.git
   cd zako-vdd
   ```

2. **创建分支**
   ```bash
   git checkout -b feature/your-feature-name
   # 或
   git checkout -b fix/your-bug-fix
   ```

3. **进行更改**
   - 遵循项目的代码风格
   - 添加必要的注释
   - 确保代码可以编译

4. **测试您的更改**
   - 在 Windows 10 22H2+ 上测试
   - 测试相关功能是否正常工作
   - 检查是否有编译警告或错误

5. **提交更改**
   ```bash
   git add .
   git commit -m "描述您的更改"
   ```

6. **推送并创建 Pull Request**
   ```bash
   git push origin feature/your-feature-name
   ```
   然后在 GitHub 上创建 Pull Request

## 开发环境设置

### 要求

- Visual Studio 2019 或更高版本
- Windows Driver Kit (WDK)
- Windows SDK
- Git

### 构建步骤

1. 克隆仓库
2. 使用 Visual Studio 打开 `Virtual Display Driver (HDR)/ZakoVDD/ZakoVDD.sln`
3. 选择配置（Debug 或 Release）和平台（x64）
4. 构建解决方案

## 代码规范

- 遵循现有的代码风格
- 使用有意义的变量和函数名
- 添加必要的注释，特别是复杂的逻辑
- 保持代码简洁和可读性

## 提交信息规范

提交信息应该清晰描述所做的更改：

- `feat: 添加新功能`
- `fix: 修复 Bug`
- `docs: 更新文档`
- `refactor: 代码重构`
- `perf: 性能优化`
- `test: 添加测试`

## 社区脚本贡献

如果您想贡献 PowerShell 脚本：

1. 确保脚本有适当的错误处理
2. 添加注释说明脚本的用途
3. 在 `Community Scripts/README.md` 中更新文档
4. 测试脚本在不同环境下的工作情况

## 问题反馈

如果您在贡献过程中遇到问题：

- 查看现有的 [Issues](https://github.com/qiin2333/zako-vdd/issues)
- 使用 [Question 模板](https://github.com/qiin2333/zako-vdd/issues/new?template=Question.yml) 提问
- 或使用 [Discussions](https://github.com/qiin2333/zako-vdd/discussions) 进行讨论

## 行为准则

- 尊重所有贡献者
- 接受建设性的批评
- 专注于对项目最有利的事情
- 对其他社区成员表示同理心

感谢您的贡献！🎉

