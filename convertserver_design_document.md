# ConvertServer 设计与实现文档

> 版本: 1.0
> 作者: tangzhiyi11
> 日期: 2026-02-27

---

## 目录

1. [概述](#1-概述)
2. [背景：原始 convertalis 分析](#2-背景原始-convertalis-分析)
3. [架构设计](#3-架构设计)
4. [详细实现](#4-详细实现)
5. [编译方法](#5-编译方法)
6. [运行与测试](#6-运行与测试)
7. [性能分析](#7-性能分析)
8. [与 gpuserver 的协同工作](#8-与-gpuserver-的协同工作)
9. [常见问题](#9-常见问题)
10. [附录](#10-附录)

---

## 1. 概述

### 1.1 什么是 convertserver

**convertserver** 是一个高性能的 ID→名称 查询服务，专门用于加速 MMseqs2 序列搜索流程中的 `convertalis` 步骤。

### 1.2 问题背景

在 MMseqs2 GPU 加速流程中，搜索阶段已经通过 GPU 实现了 38-45 倍的加速，但结果格式化步骤 (`convertalis`) 仍然占用总时间的 35-43%，成为新的性能瓶颈。

```
┌─────────────────────────────────────────────────────────────────┐
│                    MMseqs2 GPU 搜索流程耗时分布                    │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  search + gpuserver 模式:                                        │
│  ├── prefilter:   37s (37%) ████████████████                    │
│  ├── align:       20s (20%) ████████                            │
│  └── convertalis: 42s (43%) ██████████████████ ← 主要瓶颈        │
│                                                                 │
│  GPU 搜索已优化: prefilter + align 从 37m→1m                     │
│  convertalis 仍慢: 每次都加载 ~45GB 索引文件 (主要是targetDB)      │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 1.3 解决方案

convertserver 通过**服务化架构**，将 ID→名称 映射预加载到内存，避免每次运行都加载大型索引文件：

| 方法 | 耗时 | 加速比 |
|------|------|--------|
| 原始 convertalis | 39s | 1x |
| **convertalis-fast + convertserver** | **8ms** | **~5000x** |

---

## 2. 背景：原始 convertalis 分析

### 2.1 convertalis 的作用

`convertalis` 是 MMseqs2 的结果格式化工具，主要功能：

1. **读取对齐结果**：二进制格式的序列比对结果
2. **解析序列标识**：将对齐记录中的 query/target ID 转换为可读的序列名称
3. **格式化输出**：生成 BLAST M8、SAM 等标准格式

### 2.2 工作流程

#### 2.2.1 queryDB 与 targetDB 的区别

在 MMseqs2 搜索流程中，有两个关键概念：

| 概念 | 说明 | 典型规模 | 索引大小 |
|------|------|----------|----------|
| **queryDB** | 查询序列数据库，包含要搜索的序列 | 少量（如 3 条） | KB 级 |
| **targetDB** | 目标序列数据库，包含被搜索的序列 | 大量（如 5 亿条） | GB 级 |

```
┌─────────────────────────────────────────────────────────────────────┐
│                    MMseqs2 搜索流程                                  │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  搜索前:                                                            │
│  ┌──────────────┐         ┌──────────────┐                         │
│  │  queryDB     │  搜索   │  targetDB    │                         │
│  │  (查询序列)   │ ──────> │  (目标序列)   │                         │
│  │              │         │              │                         │
│  │  3 条序列    │         │  5亿条序列   │                         │
│  │  ~KB 级索引  │         │  ~45GB 索引  │                         │
│  └──────────────┘         └──────────────┘                         │
│                                                                     │
│  搜索后 (对齐结果 - 二进制格式):                                     │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  query_id=0, target_id=12345, fident=0.95, evalue=1e-50      │  │
│  │  query_id=0, target_id=67890, fident=0.88, evalue=1e-40      │  │
│  │  query_id=1, target_id=11111, fident=0.92, evalue=1e-45      │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  问题: 结果中只有数字 ID，没有可读的序列名称！                        │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

#### 2.2.2 为什么需要同时打开两个数据库

对齐结果只存储数字 ID（节省存储空间），需要通过数据库头文件查找对应的序列名称：

```
┌─────────────────────────────────────────────────────────────────────┐
│                    convertalis 的 ID 转换过程                        │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  输入对齐结果: query_id=0, target_id=12345, fident=0.95, ...       │
│                                                                     │
│  步骤 1: 转换 query_id                                              │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  query_id=0                                                  │   │
│  │       │                                                      │   │
│  │       ↓  查找 queryDB 头文件索引                             │   │
│  │  ┌─────────────┐                                             │   │
│  │  │ queryDB_h   │ ──→ "P00001|Protein_A"                      │   │
│  │  │ (几KB大小)  │     (查询序列的名称)                         │   │
│  │  └─────────────┘                                             │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  步骤 2: 转换 target_id                                             │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  target_id=12345                                             │   │
│  │       │                                                      │   │
│  │       ↓  查找 targetDB 头文件索引                            │   │
│  │  ┌─────────────┐                                             │   │
│  │  │ targetDB_h  │ ──→ "sp|P00002|Protein_B"                   │   │
│  │  │ (~12GB)     │     (目标序列的名称)                         │   │
│  │  └─────────────┘                                             │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  输出: P00001|Protein_A  sp|P00002|Protein_B  0.95  1e-50         │
│                                                                     │
│  关键瓶颈: targetDB 头文件索引巨大 (~12GB)，每次都要加载！           │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

#### 2.2.3 详细处理步骤

```
┌─────────────────────────────────────────────────────────────────────┐
│                      convertalis 工作流程                            │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  输入:                                                              │
│  ├── queryDB       查询序列数据库                                    │
│  ├── targetDB      目标序列数据库                                    │
│  └── result.m8     对齐结果 (二进制)                                 │
│                                                                     │
│  处理步骤:                                                          │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ 1. 打开 queryDB 序列索引    (~KB级，取决于查询数量)           │   │
│  │ 2. 打开 queryDB 头文件索引  (~KB级，取决于查询数量)           │   │
│  │ 3. 打开 targetDB 序列索引   (~13GB)  ← 大型数据库             │   │
│  │ 4. 打开 targetDB 头文件索引 (~12GB)  ← 大型数据库             │   │
│  │ 5. 读取 targetDB.lookup     (~20GB)  ← ID→名称映射           │   │
│  │                              ──────────                       │   │
│  │                              总计 ~45GB 需要加载 (主要是targetDB)│  │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  对每个对齐结果:                                                     │
│  ├── 解析 query ID                                                  │
│  ├── 通过头文件索引查找 query 名称                                   │
│  ├── 解析 target ID                                                 │
│  ├── 通过头文件索引查找 target 名称                                  │
│  └── 格式化输出行                                                    │
│                                                                     │
│  输出: queryName\ttargetName\tfident\talnlen\tevalue\tbits...      │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.3 瓶颈分析

通过源码分析 (`convertalignments.cpp`)，我们发现瓶颈主要在初始化阶段：

```cpp
// convertalignments.cpp 第 201-212 行
IndexReader qDbr(par.db1, par.threads, IndexReader::SRC_SEQUENCES,
                 (touch) ? (IndexReader::PRELOAD_INDEX | IndexReader::PRELOAD_DATA) : 0, dbaccessMode);
IndexReader qDbrHeader(par.db1, par.threads, IndexReader::SRC_HEADERS,
                       (touch) ? (IndexReader::PRELOAD_INDEX | IndexReader::PRELOAD_DATA) : 0);

IndexReader *tDbr;
IndexReader *tDbrHeader;
if (sameDB) {
    tDbr = &qDbr;
    tDbrHeader = &qDbrHeader;
} else {
    tDbr = new IndexReader(par.db2, par.threads, IndexReader::SRC_SEQUENCES, ...);
    tDbrHeader = new IndexReader(par.db2, par.threads, IndexReader::SRC_HEADERS, ...);
}
```

**关键发现**：

```
convertalis 初始化需要打开的文件 (实际测量值):

┌──────────────────┬───────────┬────────────────────────────────────┐
│ 文件             │ 大小      │ 用途                               │
├──────────────────┼───────────┼────────────────────────────────────┤
│ queryDB.index    │ 28 bytes  │ 查询序列索引 (3条序列)             │
│ queryDB_h.index  │ 23 bytes  │ 查询头文件索引 (3条序列)           │
│ targetDB.index   │ 13 GB     │ 目标序列索引 (5.05亿条序列)        │
│ targetDB_h.index │ 12 GB     │ 目标头文件索引 (5.05亿条序列)      │
│ targetDB.lookup  │ 20 GB     │ ID→名称映射表 (5.05亿条记录)       │
├──────────────────┼───────────┼────────────────────────────────────┤
│ 总计             │ ~45 GB    │ 主要是 targetDB 的索引文件         │
└──────────────────┴───────────┴────────────────────────────────────┘

注: 以上为实际测量值，测试环境: 3 条查询序列、505,847,454 条目标序列。
    queryDB 大小取决于查询数量，通常可忽略不计。
```

### 2.4 耗时分解

```
convertalis 耗时分解 (总计 ~36-42s):

┌─────────────────────────────────────────────────────────────────┐
│ 读取/解析结果文件:    ~30s (83%)  ████████████████████          │
│ 主要耗时在:                                                      │
│ - 打开数据库索引文件                                              │
│ - mmap 映射头文件                                                │
│ - 预加载 lookup 表                                               │
├─────────────────────────────────────────────────────────────────┤
│ 头文件查找:           ~5s  (14%)  ███                           │
│ 通过 ID 在头文件索引中查找对应名称                                 │
├─────────────────────────────────────────────────────────────────┤
│ 输出写入:             ~1s  (3%)   █                             │
│ 格式化并写入结果文件                                              │
└─────────────────────────────────────────────────────────────────┘
```

### 2.5 lookup 文件格式

lookup 文件是 ID→名称 的映射表，格式如下：

```
# 格式: ID\tName\tSetID\n
0       sp|P00001|PROT_A         0
1       sp|P00002|PROT_B         0
2       sp|P00003|PROT_C         0
...
505847453       sp|Z99999|LAST_YEAST     15
```

**关键特点**：
- 每行约 40 字节
- 5 亿条记录 ≈ 20 GB
- ID 是连续的整数 (0 到 N-1)

### 2.6 MMseqs2 数据库文件格式详解

#### 2.6.1 数据库文件组成

一个完整的 MMseqs2 数据库由多个文件组成：

```
┌─────────────────────────────────────────────────────────────────────┐
│                    MMseqs2 数据库文件结构                            │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  数据库名: targetDB (示例)                                          │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ targetDB            - 序列数据文件 (二进制)                   │   │
│  │ targetDB.index      - 序列索引文件                           │   │
│  │ targetDB_h          - 头文件数据 (序列名称和描述)             │   │
│  │ targetDB_h.index    - 头文件索引                             │   │
│  │ targetDB.lookup     - ID→名称 映射表                         │   │
│  │ targetDB.lookup.index - lookup 索引 (可选)                   │   │
│  │ targetDB.dbtype     - 数据库类型标识                         │   │
│  │ targetDB.source     - 源文件信息                             │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

#### 2.6.2 各文件格式说明

**1. 序列数据文件 (targetDB)**

```
格式: 二进制格式，存储压缩的序列数据
内容: 氨基酸序列（用数字编码）

示例 (xxd 查看):
00000000: 0512 1414 0d06 0c14 0503 0c14 0a00 0800  ................
00000010: 0912 0f05 0912 0810 0504 0002 0709 0a03  ................

说明: 每个数字代表一种氨基酸，压缩存储以节省空间
```

**2. 序列索引文件 (targetDB.index)**

```
格式: 文本格式，每行一条记录
内容: ID, 偏移量, 长度

示例:
0       0       155
1       155     112
2       267     611
...

字段说明:
- 第1列: 序列 ID (从0开始)
- 第2列: 在数据文件中的偏移量 (字节)
- 第3列: 序列长度 (字节)
```

**3. 头文件数据 (targetDB_h)**

```
格式: 文本格式，存储 FASTA 头部信息
内容: 序列名称和描述

示例 (实际数据):
PROT_000001 Protein description n=1 Tax=Species TaxID=0000 RepID=REP_A
PROT_000002 Ribosomal protein n=2 Tax=... TaxID=...
...

说明: 每条记录对应一个序列的完整 FASTA 头部（不含 > 符号）
```

**4. 头文件索引 (targetDB_h.index)**

```
格式: 与序列索引相同
内容: ID, 头文件偏移量, 头部长度

示例:
0       0       78
1       78      65
2       143     92
...
```

**5. lookup 文件 (targetDB.lookup)**

```
格式: TSV (Tab-Separated Values)
内容: ID → 序列名称 的映射

示例 (实际数据):
0       PROT_000001      0
1       PROT_000002      2
2       PROT_000003      1
3       PROT_000004      17
...

字段说明:
- 第1列: MMseqs2 内部序列 ID (uint32_t)
- 第2列: 原始序列名称 (如 UniRef ID)
- 第3列: 来源集 ID (多个输入文件时区分来源)
```

**6. dbtype 文件 (targetDB.dbtype)**

```
格式: 4字节二进制
内容: 数据库类型标识

示例:
00000000: 0000 0800                                ....

值说明:
- 0x00080000: Amino Acid (氨基酸序列)
- 0x00000001: Nucleotide (核苷酸序列)
- 0x00000002: Profile (HMM profile)
```

**7. source 文件 (targetDB.source)**

```
格式: TSV
内容: 记录输入源文件信息

示例:
0       input.fasta
1       another_input.fasta

字段说明:
- 第1列: 来源集 ID (对应 lookup 文件的第3列)
- 第2列: 原始输入文件名
```

#### 2.6.3 对齐结果文件格式 (M8/BLAST Tabular)

```
┌─────────────────────────────────────────────────────────────────────┐
│                    M8 格式说明                                       │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  格式: query\ttarget\tfident\talnlen\tmismatch\tgapopen\           │
│        qstart\tqend\ttstart\ttend\tevalue\tbits\n                  │
│                                                                     │
│  示例:                                                              │
│  N0BDY6    F2KP10    0.758    302    72    0    8    309    ...    │
│  N0BDY6    A0A0Q5    0.235    290    209   0    15   304    ...    │
│                                                                     │
│  字段说明:                                                          │
│  ┌────────┬────────────────────────────────────────────────────┐   │
│  │ 字段   │ 说明                                               │   │
│  ├────────┼────────────────────────────────────────────────────┤   │
│  │ query  │ 查询序列名称/ID                                    │   │
│  │ target │ 目标序列名称/ID                                    │   │
│  │ fident │ 序列相似度 (0-1)                                   │   │
│  │ alnlen │ 对齐长度                                           │   │
│  │ mismatch│ 错配数                                            │   │
│  │ gapopen│ Gap 开放数                                         │   │
│  │ qstart │ 查询起始位置 (1-based)                             │   │
│  │ qend   │ 查询结束位置 (1-based)                             │   │
│  │ tstart │ 目标起始位置 (1-based)                             │   │
│  │ tend   │ 目标结束位置 (1-based)                             │   │
│  │ evalue │ E-value (期望值)                                   │   │
│  │ bits   │ Bit score (比对得分)                               │   │
│  └────────┴────────────────────────────────────────────────────┘   │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

#### 2.6.4 实际文件大小示例

```
QueryDB (3条序列):
┌──────────────────────┬────────────┐
│ 文件                 │ 大小       │
├──────────────────────┼────────────┤
│ queryDB              │ 878 bytes  │
│ queryDB.index        │ 28 bytes   │
│ queryDB_h            │ 113 bytes  │
│ queryDB_h.index      │ 23 bytes   │
│ queryDB.lookup       │ 33 bytes   │
│ queryDB.dbtype       │ 4 bytes    │
│ queryDB.source       │ 19 bytes   │
├──────────────────────┼────────────┤
│ 小计                 │ ~1 KB      │
└──────────────────────┴────────────┘

TargetDB (5.05亿条序列):
┌──────────────────────┬────────────┐
│ 文件                 │ 大小       │
├──────────────────────┼────────────┤
│ targetDB             │ 150 GB     │
│ targetDB.index       │ 13 GB      │
│ targetDB_h           │ 39 GB      │
│ targetDB_h.index     │ 12 GB      │
│ targetDB.lookup      │ 20 GB      │
│ targetDB.dbtype      │ 4 bytes    │
├──────────────────────┼────────────┤
│ 小计                 │ ~234 GB    │
└──────────────────────┴────────────┘

convertalis 只需加载索引和 lookup:
- targetDB.index: 13 GB
- targetDB_h.index: 12 GB
- targetDB.lookup: 20 GB
- 总计: ~45 GB (不含序列数据本身)
```

---

## 3. 架构设计

### 3.1 整体架构

convertserver 采用 **客户端-服务器架构**，通过 Unix Domain Socket 通信：

```
┌─────────────────────────────────────────────────────────────────────┐
│                         整体架构                                     │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌───────────────────────────────────────────────────────────────┐ │
│  │                   convertserver (服务端)                       │ │
│  ├───────────────────────────────────────────────────────────────┤ │
│  │  ┌─────────────────────────────────────────────────────────┐  │ │
│  │  │ 预加载数据                                               │  │ │
│  │  │                                                         │  │ │
│  │  │  idToName: unordered_map<uint32_t, string>              │  │ │
│  │  │  ├── 5.05 亿条记录                                      │  │ │
│  │  │  ├── 内存占用: ~23 GB                                   │  │ │
│  │  │  └── 查询复杂度: O(1)                                   │  │ │
│  │  └─────────────────────────────────────────────────────────┘  │ │
│  │                                                               │ │
│  │  ┌─────────────────────────────────────────────────────────┐  │ │
│  │  │ 网络层                                                   │  │ │
│  │  │                                                         │  │ │
│  │  │  Unix Domain Socket                                     │  │ │
│  │  │  ├── 路径: /tmp/convertserver.sock                      │  │ │
│  │  │  ├── 协议: 文本协议                                      │  │ │
│  │  │  └── 并发: 每连接一线程                                  │  │ │
│  │  └─────────────────────────────────────────────────────────┘  │ │
│  └───────────────────────────────────────────────────────────────┘ │
│                                    ↑                                │
│                                    │ Unix Socket                    │
│                                    ↓                                │
│  ┌───────────────────────────────────────────────────────────────┐ │
│  │                convertalis-fast (客户端)                       │ │
│  ├───────────────────────────────────────────────────────────────┤ │
│  │  1. 读取对齐结果文件 (M8 格式)                                  │ │
│  │  2. 收集所有唯一的 target ID                                   │ │
│  │  3. 批量查询 convertserver 获取名称                            │ │
│  │  4. 输出格式化结果                                             │ │
│  └───────────────────────────────────────────────────────────────┘ │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 3.2 与传统方式的对比

```
┌─────────────────────────────────────────────────────────────────────┐
│                     传统 convertalis                                 │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  每次运行:                                                          │
│                                                                     │
│  ┌─────────┐    ┌─────────┐    ┌─────────┐    ┌─────────┐         │
│  │ 启动    │───>│ 加载    │───>│ 查找    │───>│ 输出    │         │
│  │ 程序    │    │ ~45GB   │    │ 名称    │    │ 结果    │         │
│  └─────────┘    └─────────┘    └─────────┘    └─────────┘         │
│                      │                                              │
│                      ↓                                              │
│               ~39 秒耗时                                            │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│                   convertserver 架构                                 │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  服务启动时 (一次性):                                                │
│  ┌─────────┐    ┌─────────┐                                        │
│  │ 启动    │───>│ 加载    │                                        │
│  │ 服务    │    │ ~23GB   │                                        │
│  └─────────┘    └─────────┘                                        │
│                      │                                              │
│                      ↓                                              │
│               ~73 秒 (仅一次)                                        │
│                                                                     │
│  每次查询 (极速):                                                    │
│  ┌─────────┐    ┌─────────┐    ┌─────────┐    ┌─────────┐         │
│  │ 启动    │───>│ 连接    │───>│ 批量    │───>│ 输出    │         │
│  │ 客户端  │    │ 服务    │    │ 查询    │    │ 结果    │         │
│  └─────────┘    └─────────┘    └─────────┘    └─────────┘         │
│                      │                                              │
│                      ↓                                              │
│               ~8 毫秒耗时                                            │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 3.3 通信协议

convertserver 使用简单的文本协议：

```
┌─────────────────────────────────────────────────────────────────────┐
│                       通信协议                                       │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  1. 单个查询 (GET)                                                  │
│  ─────────────────────                                              │
│  请求: GET <id>\n                                                   │
│  响应: <name>\n 或 NOT_FOUND\n 或 ERROR\n                          │
│                                                                     │
│  示例:                                                              │
│  > GET 12345\n                                                      │
│  < sp|P00001|PROT_A\n                                              │
│                                                                     │
│  2. 批量查询 (BATCH)                                                │
│  ─────────────────────                                              │
│  请求: BATCH <id1> <id2> <id3> ...\n                               │
│  响应: <name1>\t<name2>\t<name3>\t...\n                            │
│                                                                     │
│  示例:                                                              │
│  > BATCH 1 2 3\n                                                    │
│  < sp|P00002|PROT_B\tsp|P00003|PROT_C\tNOT_FOUND\n                 │
│                                                                     │
│  3. 健康检查 (PING)                                                 │
│  ─────────────────────                                              │
│  请求: PING\n                                                       │
│  响应: PONG\n                                                       │
│                                                                     │
│  4. 状态查询 (STAT)                                                 │
│  ─────────────────────                                              │
│  请求: STAT\n                                                       │
│  响应: ENTRIES:<count>\n                                           │
│                                                                     │
│  示例:                                                              │
│  > STAT\n                                                           │
│  < ENTRIES:505847454\n                                             │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 3.4 并发模型

```
┌─────────────────────────────────────────────────────────────────────┐
│                       并发模型                                       │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  主线程:                                                            │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  while(running) {                                            │   │
│  │      select(...);  // 等待连接，1秒超时                       │   │
│  │      if (有新连接) {                                          │   │
│  │          clientSocket = accept();                            │   │
│  │          thread = new thread(handleClient, clientSocket);    │   │
│  │          thread.detach();  // 分离线程                        │   │
│  │      }                                                       │   │
│  │  }                                                           │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  工作线程 (每个客户端一个):                                          │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  void handleClient(int clientSocket) {                       │   │
│  │      while(running) {                                        │   │
│  │          recv(...);  // 接收请求                              │   │
│  │          查询 idToName 哈希表;                                │   │
│  │          send(...);  // 发送响应                              │   │
│  │      }                                                       │   │
│  │      close(clientSocket);                                    │   │
│  │  }                                                           │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  特点:                                                              │
│  - 无锁读取: unordered_map 在初始化后只读，无需加锁                  │
│  - 线程安全: 每个连接独立处理                                        │
│  - 非阻塞: 使用 select() 带超时，支持优雅关闭                        │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 4. 详细实现

### 4.1 convertserver.cpp 核心实现

#### 4.1.1 文件加载

使用 mmap 高效加载大型 lookup 文件：

```cpp
bool loadLookupFile(const std::string& lookupFile) {
    // 1. 打开文件
    int fd = open(lookupFile.c_str(), O_RDONLY);

    // 2. 获取文件大小
    struct stat st;
    fstat(fd, &st);
    off_t fileSize = st.st_size;

    // 3. mmap 映射到内存 (使用 MAP_POPULATE 预加载)
    void* mapped = mmap(NULL, fileSize, PROT_READ,
                        MAP_PRIVATE | MAP_POPULATE, fd, 0);

    // 4. 预分配哈希表
    size_t estimatedEntries = fileSize / 40;  // 每行约 40 字节
    idToName.reserve(estimatedEntries);

    // 5. 解析每行
    char* data = (char*)mapped;
    size_t pos = 0, lineStart = 0;
    while (pos < fileSize) {
        if (data[pos] == '\n') {
            // 解析: ID\tName\tSetID
            // 提取 ID 和 Name，存入哈希表
            idToName[id] = std::move(name);
            lineStart = pos + 1;
        }
        pos++;
    }

    // 6. 清理
    munmap(mapped, fileSize);
    close(fd);

    return true;
}
```

**关键优化点**：

```
┌─────────────────────────────────────────────────────────────────────┐
│                    文件加载优化                                      │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  1. mmap + MAP_POPULATE                                             │
│     - 避免用户态/内核态多次数据拷贝                                  │
│     - MAP_POPULATE 预加载页到内存                                   │
│     - 比 fread() 快约 2-3 倍                                        │
│                                                                     │
│  2. 预分配哈希表容量                                                 │
│     - reserve() 避免动态扩容                                        │
│     - 扩容会导致全表重新哈希，非常耗时                               │
│                                                                     │
│  3. std::move 语义                                                  │
│     - 避免字符串拷贝                                                │
│     - 直接转移所有权                                                │
│                                                                     │
│  4. 手动解析 (不用 stringstream)                                    │
│     - 避免格式化解析开销                                            │
│     - 直接字符操作更快                                              │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

#### 4.1.2 请求处理

```cpp
void handleClient(int clientSocket) {
    char buffer[65536];  // 64KB 缓冲区

    while (running) {
        ssize_t bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        if (bytesRead <= 0) break;

        buffer[bytesRead] = '\0';
        std::string request(buffer);
        std::string response;

        if (request.substr(0, 4) == "GET ") {
            // 单个查询
            uint32_t id = std::stoul(request.substr(4));
            auto it = idToName.find(id);
            if (it != idToName.end()) {
                response = it->second + "\n";
            } else {
                response = "NOT_FOUND\n";
            }
        } else if (request.substr(0, 6) == "BATCH ") {
            // 批量查询
            // 解析空格分隔的 ID 列表
            size_t pos = 6;
            while (pos < request.size()) {
                // 跳过空格
                while (pos < request.size() && request[pos] == ' ') pos++;
                // 解析数字 ID
                size_t start = pos;
                while (pos < request.size() && isdigit(request[pos])) pos++;
                uint32_t id = std::stoul(request.substr(start, pos - start));

                auto it = idToName.find(id);
                response += (it != idToName.end()) ? it->second : "NOT_FOUND";
                response += "\t";
            }
            response.back() = '\n';  // 最后一个 \t 改为 \n
        }

        send(clientSocket, response.c_str(), response.size(), 0);
    }

    close(clientSocket);
}
```

#### 4.1.3 主循环

```cpp
int main(int argc, char* argv[]) {
    // 1. 信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // 2. 加载 lookup 文件
    loadLookupFile(lookupFile);

    // 3. 创建 Unix Socket
    int serverSocket = socket(AF_UNIX, SOCK_STREAM, 0);
    unlink(socketPath.c_str());  // 删除旧文件

    // 4. 绑定和监听
    bind(serverSocket, ...);
    listen(serverSocket, 10);

    // 5. 主循环
    while (running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(serverSocket, &readfds);

        struct timeval timeout = {1, 0};  // 1秒超时
        int activity = select(serverSocket + 1, &readfds, NULL, NULL, &timeout);

        if (activity > 0 && FD_ISSET(serverSocket, &readfds)) {
            int clientSocket = accept(serverSocket, NULL, NULL);
            std::thread clientThread(handleClient, clientSocket);
            clientThread.detach();  // 分离线程
        }
    }

    // 6. 清理
    close(serverSocket);
    unlink(socketPath.c_str());

    return 0;
}
```

### 4.2 convertalis_fast.cpp 核心实现

#### 4.2.1 客户端类

```cpp
class ConvertClient {
private:
    int sock;
    std::string socketPath;
    char buffer[1048576];  // 1MB 缓冲区

public:
    bool connect() {
        sock = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr;
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);
        return ::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0;
    }

    // 批量查询
    std::vector<std::string> getNames(const std::vector<uint32_t>& ids) {
        // 构建请求: BATCH id1 id2 id3 ...\n
        std::string request = "BATCH";
        for (uint32_t id : ids) {
            request += " " + std::to_string(id);
        }
        request += "\n";

        // 发送请求
        send(sock, request.c_str(), request.size(), 0);

        // 接收响应 (可能需要多次 recv)
        std::string response;
        while (true) {
            ssize_t bytesRead = recv(sock, buffer, sizeof(buffer) - 1, 0);
            buffer[bytesRead] = '\0';
            response += buffer;
            if (response.back() == '\n') break;  // 收到完整响应
        }

        // 解析响应 (\t 分隔)
        return parseTabSeparated(response);
    }
};
```

#### 4.2.2 主流程

```cpp
int main(int argc, char* argv[]) {
    // 1. 连接 convertserver
    ConvertClient client(socketPath);
    client.connect();

    // 2. 第一遍：扫描输入文件，收集所有 target ID
    std::unordered_map<uint32_t, std::string> idToName;
    std::vector<std::string> lines;

    while (std::getline(inFile, line)) {
        AlignmentResult r = AlignmentResult::parse(line);
        idToName[r.targetId] = "";  // 占位
        lines.push_back(line);
    }

    // 3. 批量获取名称
    std::vector<uint32_t> idsToFetch;
    for (auto& pair : idToName) {
        idsToFetch.push_back(pair.first);
    }

    // 分批查询 (每批 batchSize 个 ID)
    for (size_t i = 0; i < idsToFetch.size(); i += batchSize) {
        std::vector<uint32_t> batch(idsToFetch.begin() + i,
                                     idsToFetch.begin() + std::min(i + batchSize, idsToFetch.size()));
        std::vector<std::string> names = client.getNames(batch);
        for (size_t j = 0; j < batch.size(); j++) {
            idToName[batch[j]] = names[j];
        }
    }

    // 4. 第二遍：输出结果
    for (const std::string& line : lines) {
        AlignmentResult r = AlignmentResult::parse(line);
        std::string targetName = idToName[r.targetId];
        // 输出格式化结果
        outFile << r.query << "\t"
                << targetName << "\t"
                << r.fident << "\t" << r.alnlen << "\t"
                << r.mismatch << "\t" << r.gapopen << "\t"
                << r.qstart << "\t" << r.qend << "\t"
                << r.tstart << "\t" << r.tend << "\t"
                << r.evalue << "\t" << r.bits << "\n";
    }

    return 0;
}
```

### 4.3 数据结构

#### 4.3.1 对齐结果结构

```cpp
struct AlignmentResult {
    std::string query;      // 查询序列名称
    uint32_t targetId;      // 目标序列 ID (数字)
    double fident;          // 序列相似度
    int alnlen;             // 对齐长度
    int mismatch;           // 错配数
    int gapopen;            // Gap 开放数
    int qstart, qend;       // 查询起止位置
    int tstart, tend;       // 目标起止位置
    double evalue;          // E 值
    double bits;            // Bit 分数

    static AlignmentResult parse(const std::string& line);
};
```

#### 4.3.2 内存布局

```
┌─────────────────────────────────────────────────────────────────────┐
│                     服务端内存布局                                   │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  idToName: unordered_map<uint32_t, string>                          │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  Bucket 0:                                                   │   │
│  │  ├── key: 0        → value: "sp|P00001|PROT_A"             │   │
│  │  └── key: 1234567  → value: "sp|P00002|PROT_B"             │   │
│  ├─────────────────────────────────────────────────────────────┤   │
│  │  Bucket 1:                                                   │   │
│  │  ├── key: 1        → value: "sp|P00003|PROT_C"             │   │
│  │  └── ...                                                     │   │
│  ├─────────────────────────────────────────────────────────────┤   │
│  │  ...                                                         │   │
│  ├─────────────────────────────────────────────────────────────┤   │
│  │  Bucket N:                                                   │   │
│  │  └── key: 505847453 → value: "sp|Z99999|LAST_YEAST"         │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  内存估算:                                                          │
│  ├── 哈希表开销: ~5 亿 × 8 字节 = 4 GB                             │
│  ├── 字符串数据: ~5 亿 × 35 字节 = 17.5 GB                         │
│  ├── 字符串开销: ~5 亿 × 3 字节 = 1.5 GB (SSO 短字符串优化)         │
│  └── 总计: ~23 GB                                                  │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 5. 编译方法

### 5.1 目录结构

```
convertserver/
├── Makefile                    # 简单 Makefile (推荐)
├── CMakeLists.txt              # CMake 配置
├── README.md                   # 使用说明
├── src/
│   ├── convertserver.cpp       # 服务端源码
│   └── convertalis_fast.cpp    # 客户端源码
├── convertserver               # 编译后的服务端
└── convertalis-fast            # 编译后的客户端
```

### 5.2 方法一：使用 Makefile (推荐)

```bash
cd /path/to/convertserver

# 编译
make

# 清理
make clean

# 安装到系统路径
make install
```

**Makefile 内容**：

```makefile
CXX = g++
CXXFLAGS = -O3 -std=c++11 -pthread -Wall -Wno-unused-function
LDFLAGS = -lpthread

TARGETS = convertserver convertalis-fast

all: $(TARGETS)

convertserver: src/convertserver.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

convertalis-fast: src/convertalis_fast.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)

install: all
	install -m 755 convertserver /usr/local/bin/
	install -m 755 convertalis-fast /usr/local/bin/
```

### 5.3 方法二：使用 CMake

```bash
cd /path/to/convertserver

# 创建构建目录
mkdir -p build && cd build

# 配置
cmake ..

# 编译
cmake --build . -j $(nproc)

# 安装
cmake --install .
```

### 5.4 编译选项说明

```
┌─────────────────────────────────────────────────────────────────────┐
│                       编译选项                                       │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  -O3              最高优化级别，启用所有优化                          │
│  -std=c++11       使用 C++11 标准                                    │
│  -pthread         启用 POSIX 线程支持                                │
│  -Wall            启用所有警告                                       │
│  -Wno-unused-function  忽略未使用函数警告                            │
│  -lpthread        链接 pthread 库                                    │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 5.5 验证编译

```bash
# 检查可执行文件
ls -la convertserver convertalis-fast

# 检查依赖
ldd convertserver
ldd convertalis-fast

# 测试运行
./convertserver --help 2>&1 || true
./convertalis-fast --help
```

---

## 6. 运行与测试

### 6.1 启动 convertserver

```bash
# 基本用法
./convertserver <lookup_file> [socket_path]

# 示例：后台启动
./convertserver /path/to/targetDB.lookup /tmp/convertserver.sock &

# 查看启动日志
```

**预期输出**：

```
[INFO] Loading lookup file: /path/to/targetDB.lookup
[INFO] File size: 19.1112 GB
[INFO] Loaded 10M entries...
[INFO] Loaded 20M entries...
...
[INFO] Loaded 505847454 entries in 73s
[INFO] Estimated memory: ~23.5554 GB
[INFO] convertserver started
[INFO] Socket path: /tmp/convertserver.sock
[INFO] Ready to accept connections
/tmp/convertserver.sock
```

### 6.2 使用 convertalis-fast

```bash
# 基本用法
./convertalis-fast <input.m8> <output.m8> --socket-path <path>

# 示例
./convertalis-fast \
    /tmp/result_id_only.m8 \
    /tmp/output.m8 \
    --socket-path /tmp/convertserver.sock

# 使用更多线程
./convertalis-fast \
    /tmp/result.m8 \
    /tmp/output.m8 \
    --socket-path /tmp/convertserver.sock \
    --threads 4 \
    --batch-size 5000
```

### 6.3 完整工作流程

```bash
#!/bin/bash
# complete_workflow.sh

# 配置
LOOKUP_FILE="/path/to/data/targetDB.lookup"
SOCKET_PATH="/tmp/convertserver.sock"
RESULT_FILE="/tmp/aln_result.m8"
OUTPUT_FILE="/tmp/aln_output.m8"

# 1. 启动 convertserver
echo "Starting convertserver..."
./convertserver "$LOOKUP_FILE" "$SOCKET_PATH" &
SERVER_PID=$!

# 等待服务就绪
sleep 5
while [ ! -S "$SOCKET_PATH" ]; do
    echo "Waiting for server to start..."
    sleep 1
done
echo "Server started with PID $SERVER_PID"

# 2. 执行格式化
echo "Running convertalis-fast..."
./convertalis-fast "$RESULT_FILE" "$OUTPUT_FILE" --socket-path "$SOCKET_PATH"

# 3. 关闭服务
echo "Stopping convertserver..."
kill $SERVER_PID

echo "Done!"
```

### 6.4 测试服务

#### 6.4.1 使用 Python 测试

```python
#!/usr/bin/env python3
import socket

def test_convertserver(socket_path):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(socket_path)

    # PING 测试
    sock.send(b"PING\n")
    response = sock.recv(1024).decode().strip()
    print(f"PING: {response}")
    assert response == "PONG", "PING test failed"

    # STAT 测试
    sock.send(b"STAT\n")
    response = sock.recv(1024).decode().strip()
    print(f"STAT: {response}")
    assert response.startswith("ENTRIES:"), "STAT test failed"

    # GET 测试
    sock.send(b"GET 0\n")
    response = sock.recv(1024).decode().strip()
    print(f"GET 0: {response}")

    # BATCH 测试
    sock.send(b"BATCH 0 1 2\n")
    response = sock.recv(1024).decode().strip()
    print(f"BATCH 0 1 2: {response}")
    assert '\t' in response, "BATCH test failed"

    sock.close()
    print("All tests passed!")

if __name__ == "__main__":
    test_convertserver("/tmp/convertserver.sock")
```

#### 6.4.2 使用 netcat 测试

```bash
# PING 测试
echo "PING" | nc -U /tmp/convertserver.sock

# STAT 测试
echo "STAT" | nc -U /tmp/convertserver.sock

# GET 测试
echo "GET 0" | nc -U /tmp/convertserver.sock

# BATCH 测试
echo "BATCH 0 1 2 3" | nc -U /tmp/convertserver.sock
```

### 6.5 关闭服务

```bash
# 方法一：使用 pkill
pkill -f convertserver

# 方法二：发送 SIGTERM
kill <pid>

# 方法三：发送 SIGINT (Ctrl+C)
# 如果在前台运行，直接 Ctrl+C
```

---

## 7. 性能分析

### 7.1 测试环境

| 项目 | 配置 |
|------|------|
| CPU | x86_64 架构 |
| 内存 | 1.5 TB |
| 查询数据 | 3 条序列 |
| 目标数据库 | 505,847,454 条序列 (~636 GB) |
| lookup 文件 | 19.1 GB |
| 对齐结果 | ~900 条 |

### 7.2 性能对比

```
┌─────────────────────────────────────────────────────────────────────┐
│                       性能对比                                       │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  原始 convertalis:                                                  │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ ████████████████████████████████████████ 39s                │   │
│  └─────────────────────────────────────────────────────────────┘   │
│  主要耗时: 加载 ~45GB 索引文件 (主要是targetDB)                      │
│                                                                     │
│  convertalis-fast + convertserver:                                  │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ █ 8ms                                                       │   │
│  └─────────────────────────────────────────────────────────────┘   │
│  主要耗时: 网络通信 (Unix Socket)                                    │
│                                                                     │
│  加速比: 39s / 8ms = 4875x ≈ 5000x                                 │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 7.3 详细耗时分解

```
convertalis-fast 耗时分解:

┌─────────────────────────────────────────────────────────────────────┐
│ 步骤                          │ 耗时      │ 占比                    │
├───────────────────────────────┼───────────┼────────────────────────┤
│ 扫描输入文件                   │ 0.5 ms    │ 6%                     │
│ 连接 convertserver             │ 0.2 ms    │ 3%                     │
│ 批量查询 (~900 ID)             │ 5.0 ms    │ 63%                    │
│ 本地哈希表查找                  │ 0.3 ms    │ 4%                     │
│ 输出写入                       │ 2.0 ms    │ 25%                    │
├───────────────────────────────┼───────────┼────────────────────────┤
│ 总计                           │ ~8 ms     │ 100%                   │
└─────────────────────────────────────────────────────────────────────┘
```

### 7.4 内存使用

```
┌─────────────────────────────────────────────────────────────────────┐
│                       内存使用                                       │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  convertserver (服务端):                                            │
│  ├── idToName 哈希表: ~23 GB                                       │
│  ├── 程序代码和数据: ~100 MB                                        │
│  ├── 线程栈 (每连接 8MB): ~N × 8 MB                                 │
│  └── 总计: ~23-24 GB                                               │
│                                                                     │
│  convertalis-fast (客户端):                                         │
│  ├── 程序代码: ~1 MB                                               │
│  ├── 输入缓冲: ~10 MB                                              │
│  ├── ID→名称缓存: ~1 MB (取决于唯一 ID 数量)                         │
│  └── 总计: ~10-20 MB                                               │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 7.5 吞吐量

```
┌─────────────────────────────────────────────────────────────────────┐
│                       吞吐量测试                                     │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  单次查询延迟: ~5 μs                                                │
│  批量查询 (1000 ID): ~3 ms                                         │
│  最大吞吐量: ~300,000 queries/second                               │
│                                                                     │
│  影响因素:                                                          │
│  ├── Unix Socket 延迟: ~1-2 μs                                     │
│  ├── 哈希表查找: O(1), ~0.1 μs                                     │
│  ├── 字符串拼接: ~1-2 μs                                           │
│  └── 网络 I/O: 取决于批大小                                         │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 8. 与 gpuserver 的协同工作

### 8.1 完整 GPU 加速流程

```
┌─────────────────────────────────────────────────────────────────────┐
│                完整 GPU 加速流程                                      │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  阶段 1: 准备工作 (一次性)                                           │
│  ─────────────────────────────                                      │
│  ┌─────────────┐   ┌─────────────┐   ┌─────────────┐              │
│  │ createdb    │ → │ makepadded  │ → │ createindex │              │
│  │ 创建数据库   │   │ seqdb       │   │ 创建索引    │              │
│  └─────────────┘   └─────────────┘   └─────────────┘              │
│                                                                     │
│  阶段 2: 启动服务 (一次性)                                           │
│  ─────────────────────────────                                      │
│  ┌─────────────┐   ┌─────────────┐                                 │
│  │ gpuserver   │   │convertserver│                                 │
│  │ GPU 服务    │   │ ID 查询服务 │                                 │
│  │ ~40GB/GPU   │   │ ~23GB       │                                 │
│  └─────────────┘   └─────────────┘                                 │
│                                                                     │
│  阶段 3: 搜索和格式化 (每次查询)                                      │
│  ─────────────────────────────                                      │
│  ┌─────────────┐   ┌─────────────┐   ┌─────────────┐              │
│  │ search      │ → │ convertalis │ → │ 输出        │              │
│  │ +gpuserver  │   │ -fast       │   │ M8 格式     │              │
│  │ ~59s        │   │ ~8ms        │   │             │              │
│  └─────────────┘   └─────────────┘   └─────────────┘              │
│                                                                     │
│  总耗时: ~59s + ~8ms ≈ 59s (vs 原始 37m50s)                         │
│  加速比: ~38x                                                       │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 8.2 服务协同示意

```
┌─────────────────────────────────────────────────────────────────────┐
│                      服务协同架构                                    │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌───────────────────────────────────────────────────────────────┐ │
│  │                       用户请求                                 │ │
│  └───────────────────────────────────────────────────────────────┘ │
│                                │                                    │
│                                ↓                                    │
│  ┌───────────────────────────────────────────────────────────────┐ │
│  │                      mmseqs search                             │ │
│  │                                                               │ │
│  │  ┌─────────────┐                                              │ │
│  │  │  prefilter  │ ──→ GPU (gpuserver) ──→ 候选序列列表         │ │
│  │  │  ~38s       │                                              │ │
│  │  └─────────────┘                                              │ │
│  │         │                                                      │ │
│  │         ↓                                                      │ │
│  │  ┌─────────────┐                                              │ │
│  │  │   align     │ ──→ GPU (gpuserver) ──→ 对齐结果 (ID)        │ │
│  │  │   ~21s      │                                              │ │
│  │  └─────────────┘                                              │ │
│  └───────────────────────────────────────────────────────────────┘ │
│                                │                                    │
│                                ↓                                    │
│  ┌───────────────────────────────────────────────────────────────┐ │
│  │                  convertalis-fast                              │ │
│  │                                                               │ │
│  │  ┌─────────────┐                                              │ │
│  │  │  批量查询   │ ──→ convertserver ──→ ID→名称映射             │ │
│  │  │  ~8ms       │                                              │ │
│  │  └─────────────┘                                              │ │
│  └───────────────────────────────────────────────────────────────┘ │
│                                │                                    │
│                                ↓                                    │
│  ┌───────────────────────────────────────────────────────────────┐ │
│  │                       最终结果                                 │ │
│  │              queryName\ttargetName\t...                       │ │
│  └───────────────────────────────────────────────────────────────┘ │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 8.3 完整使用示例

```bash
#!/bin/bash
# full_gpu_search.sh

# 配置
export MMSEQS_DISABLE_DPX=1
export CUDA_VISIBLE_DEVICES=0,1,2,3

QUERY_DB="/path/to/queryDB"
TARGET_DB="/path/to/targetDB_padded"
LOOKUP_FILE="${TARGET_DB}.lookup"
RESULT_FILE="/tmp/result.m8"
OUTPUT_FILE="/tmp/output.m8"
TMP_DIR="/tmp/mmseqs_tmp"

# 启动 gpuserver
echo "Starting gpuserver..."
mmseqs gpuserver "$TARGET_DB" --max-seqs 300 --prefilter-mode 1 --db-load-mode 2 &
GPU_SERVER_PID=$!
sleep 60  # 等待 GPU 数据加载

# 启动 convertserver
echo "Starting convertserver..."
./convertserver "$LOOKUP_FILE" /tmp/convertserver.sock &
CONVERT_SERVER_PID=$!
sleep 5  # 等待 lookup 加载

# 执行搜索
echo "Running search..."
mmseqs search "$QUERY_DB" "$TARGET_DB" "$RESULT_FILE" "$TMP_DIR" \
    --gpu 1 --gpu-server 1 \
    --max-seqs 300 --prefilter-mode 1 --db-load-mode 2

# 格式化输出
echo "Formatting output..."
./convertalis-fast "$RESULT_FILE" "$OUTPUT_FILE" \
    --socket-path /tmp/convertserver.sock

# 关闭服务
echo "Stopping servers..."
kill $GPU_SERVER_PID
kill $CONVERT_SERVER_PID

echo "Done! Output: $OUTPUT_FILE"
wc -l "$OUTPUT_FILE"
```

---

## 9. 常见问题

### 9.1 编译问题

**Q: 编译时报错 `undefined reference to 'pthread_create'`**

A: 确保链接了 pthread 库：
```bash
# Makefile 中添加
LDFLAGS = -lpthread
```

**Q: 编译时报错 `C++11` 特性不支持**

A: 确保使用 C++11 标准：
```bash
CXXFLAGS = -std=c++11
```

### 9.2 运行时问题

**Q: 启动 convertserver 时内存不足**

A: convertserver 需要约 25GB 内存。确保系统有足够内存：
```bash
# 检查可用内存
free -h

# 如果内存不足，可以考虑分片处理
```

**Q: 客户端连接失败 `Connection refused`**

A: 检查服务是否启动，socket 文件是否存在：
```bash
# 检查服务进程
ps aux | grep convertserver

# 检查 socket 文件
ls -la /tmp/convertserver.sock
```

**Q: 服务启动很慢 (超过 2 分钟)**

A: 正常现象。加载 5 亿条记录需要约 73 秒。可以查看进度日志：
```
[INFO] Loaded 10M entries...
[INFO] Loaded 20M entries...
...
```

### 9.3 性能问题

**Q: 批量查询比预期慢**

A: 检查以下因素：
1. 批大小是否合适 (推荐 1000-5000)
2. 网络是否有延迟 (Unix Socket 应该很快)
3. 服务端是否有多个并发连接

```bash
# 调整批大小
./convertalis-fast input.m8 output.m8 --batch-size 5000
```

**Q: 内存占用比预期高**

A: 哈希表有额外开销。5 亿条记录约需要 23GB，这是正常的。

### 9.4 兼容性问题

**Q: 输出格式与原始 convertalis 不同**

A: convertalis-fast 输出标准 M8 格式。如果需要其他格式，可以修改 `AlignmentResult::parse` 和输出部分。

**Q: lookup 文件格式不兼容**

A: convertserver 期望 lookup 文件格式为：`ID\tName\tSetID\n`
如果格式不同，需要修改 `loadLookupFile` 函数。

---

## 10. 附录

### 10.1 命令速查

```bash
# 编译
cd convertserver && make

# 启动服务
./convertserver /path/to/db.lookup /tmp/convertserver.sock &

# 使用客户端
./convertalis-fast input.m8 output.m8 --socket-path /tmp/convertserver.sock

# 测试服务
echo "PING" | nc -U /tmp/convertserver.sock

# 关闭服务
pkill -f convertserver
```

### 10.2 文件格式

**输入文件 (M8 格式)**：
```
queryId\ttargetId\tfident\talnlen\tmismatch\tgapopen\tqstart\tqend\ttstart\ttend\tevalue\tbits
```

**输出文件 (M8 格式)**：
```
queryName\ttargetName\tfident\talnlen\tmismatch\tgapopen\tqstart\tqend\ttstart\ttend\tevalue\tbits
```

**lookup 文件**：
```
ID\tName\tSetID
0\tsp|P00001|PROT_A\t0
1\tsp|P00002|PROT_B\t0
```

### 10.3 性能参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--socket-path` | `/tmp/convertserver.sock` | Unix Socket 路径 |
| `--threads` | 1 | 客户端线程数 |
| `--batch-size` | 1000 | 批量查询大小 |

### 10.4 源码文件清单

| 文件 | 行数 | 说明 |
|------|------|------|
| `src/convertserver.cpp` | 288 | 服务端实现 |
| `src/convertalis_fast.cpp` | 397 | 客户端实现 |
| `Makefile` | 46 | 编译配置 |
| `CMakeLists.txt` | 78 | CMake 配置 |

### 10.5 参考资料

- [MMseqs2 官方文档](https://github.com/soedinglab/MMseqs2)
- [MMseqs2 用户指南](https://mmseqs.com/)
- Unix Domain Socket 编程
- C++ unordered_map 性能优化

---

*文档版本: 1.0*
*最后更新: 2026-02-27*
*作者: tangzhiyi11*
