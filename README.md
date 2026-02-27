# convertserver - 快速 convertalis 服务

## 概述

convertserver 是一个常驻内存的服务，用于加速 MMseqs2 的 convertalis 步骤。

**性能提升**: 从 **39s** 降低到 **8ms** (~5000x 加速)

## 实测性能

| 方法 | 耗时 | 说明 |
|------|------|------|
| 原始 convertalis | **39s** | 每次都加载 ~45GB 索引文件 |
| **convertalis-fast + convertserver** | **8ms** | 从内存哈希表查询 |

测试条件:
- 查询序列: 3 条
- 目标数据库: 505,847,454 条序列
- 结果数量: 900 条对齐

## 原理

传统的 convertalis 每次运行都需要:
1. 打开序列数据库索引: 13GB
2. 打开头文件数据库索引: 12GB
3. 读取 lookup 文件: 20GB
4. 总计: ~45GB 需要加载

convertserver 将 ID→名称 映射预加载到内存 (~23GB)，后续查询直接从内存获取。

## 架构

```
┌─────────────────────────────────────────────────────────────┐
│                    convertserver (常驻服务)                   │
├─────────────────────────────────────────────────────────────┤
│  预加载数据:                                                  │
│  ├── id_to_name 哈希表: ~23GB (5亿条 ID→名称映射)            │
│  └── Unix Socket: /tmp/convertserver.sock                   │
├─────────────────────────────────────────────────────────────┤
│  协议:                                                       │
│  ├── GET <id>\n → <name>\n                                  │
│  ├── BATCH <id1> <id2> ...\n → <name1>\t<name2>\t...\n      │
│  ├── PING → PONG                                            │
│  └── STAT → ENTRIES:<count>                                 │
└─────────────────────────────────────────────────────────────┘
                              ↑
                              │ Unix Socket
                              ↓
┌─────────────────────────────────────────────────────────────┐
│                  convertalis-fast (客户端)                    │
├─────────────────────────────────────────────────────────────┤
│  流程:                                                       │
│  1. 打开结果文件 (~45KB)                                     │
│  2. 解析对齐记录                                              │
│  3. 从 convertserver 获取 target 名称 (批量)                 │
│  4. 输出格式化结果                                            │
│  实测耗时: 8ms                                               │
└─────────────────────────────────────────────────────────────┘
```

## 使用方法

### 1. 编译

```bash
cd /path/to/convertserver
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 2. 启动服务

```bash
# 后台启动 (加载 5 亿条记录约需 73s，内存 ~23GB)
./convertserver /path/to/targetDB.lookup /tmp/convertserver.sock &

# 查看启动日志
# [INFO] Loading lookup file: .../targetDB.lookup
# [INFO] File size: 19.1112 GB
# [INFO] Loaded 505847454 entries in 73s
# [INFO] Estimated memory: ~23.5554 GB
# [INFO] convertserver started
# [INFO] Socket path: /tmp/convertserver.sock
# [INFO] Ready to accept connections
```

### 3. 使用客户端

```bash
# 输入文件格式: queryName\ttargetId\tfident\talnlen\tmismatch\tgapopen\tqstart\tqend\ttstart\ttend\tevalue\tbits
# 输出文件格式: queryName\ttargetName\tfident\talnlen\tmismatch\tgapopen\tqstart\tqend\ttstart\ttend\tevalue\tbits

./convertalis-fast \
    /path/to/result_id_only.m8 \
    /path/to/output.m8 \
    --socket-path /tmp/convertserver.sock
```

### 4. 关闭服务

```bash
pkill -f convertserver
```

## 注意事项

1. **内存需求**: ~25GB (ID→名称 哈希表)
2. **启动时间**: ~73s (加载 5 亿条记录)
3. **系统要求**: Unix/Linux (Unix Domain Socket)
4. **并发支持**: 多线程安全
5. **兼容性**: 输出格式与原始 convertalis 一致

## 文件结构

```
convertserver/
├── CMakeLists.txt          # CMake 编译配置
├── Makefile                # 简化编译脚本
├── README.md               # 本文档
└── src/
    ├── convertserver.cpp   # 服务端 (~285行)
    └── convertalis_fast.cpp # 客户端 (~300行)
```

## 测试

### 完整流程测试

```bash
# 终端1: 启动服务 (先确保已编译)
./convertserver /path/to/targetDB.lookup /tmp/convertserver.sock

# 终端2: 使用客户端转换结果
./convertalis-fast input.m8 output.m8 --socket-path /tmp/convertserver.sock

# 测试完成后关闭服务
pkill -f convertserver
```

### 协议调试 (可选)

直接通过 Unix Socket 测试服务协议：

```bash
python3 << 'EOF'
import socket
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect("/tmp/convertserver.sock")

# PING 测试
sock.send(b"PING\n")
print("PING:", sock.recv(1024).decode().strip())

# STAT 测试 (查看已加载的序列数量)
sock.send(b"STAT\n")
print("STAT:", sock.recv(1024).decode().strip())

# GET 测试 (查询 ID=0 的序列名称)
sock.send(b"GET 0\n")
print("GET 0:", sock.recv(1024).decode().strip())

sock.close()
EOF
```

## 与原始 convertalis 的区别

| 特性 | 原始 convertalis | convertalis-fast |
|------|------------------|------------------|
| 输入格式 | MMseqs2 二进制结果 | M8 格式 (ID) |
| 依赖 | MMseqs2 库 | 独立程序 |
| lookup 加载 | 每次运行 | 服务启动时一次 |
| 耗时 | 39s | 8ms |
| 内存 | 临时 ~45GB | 服务常驻 ~23GB |

## 文档

- [设计文档](./convertserver_design_document.md) - 详细的架构设计与实现说明
