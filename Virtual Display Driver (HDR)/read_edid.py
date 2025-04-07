 # 读取二进制文件
with open('zyt0214.bin', 'rb') as f:
    edid_bytes = f.read()

# 输出验证信息
print(f"成功读取EDID文件，总长度：{len(edid_bytes)}字节")
print("前16字节示例：", edid_bytes[:16].hex(' '))

# 完整EDID数据格式化输出
print("完整EDID数据：")
formatted = []
for i in range(0, len(edid_bytes), 16):
    # 每16字节转换为带0x前缀的十六进制字符串
    hex_line = ', '.join([f'0x{b:02x}' for b in edid_bytes[i:i+16]])
    formatted.append(f'    {hex_line},')  # 保持与C/C++数组相同的格式

# 组合成完整的数组格式
print('[\n' + '\n'.join(formatted) + '\n]')