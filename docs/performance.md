# PhotoBridge 性能观测与 Benchmark

## 目标

PhotoBridge 的性能观测不是为了追求单点跑分，而是为了证明这个项目已经具备存储系统里的关键工程能力：

- 分片上传的数据路径可以被端到端测量。
- CRC32C、chunk 写入、status 查询、complete 合并这些阶段可以被定位。
- 不同文件大小和 chunk 大小会影响吞吐、请求次数和合并成本。
- 项目可以从功能正确性继续走向性能分析和系统优化。

这份文档和 [存储系统设计](./storage-design.md) 是配套关系：系统设计解释数据路径、元数据路径和故障恢复路径；性能观测用 benchmark 数据验证这些路径的成本和瓶颈。

## 当前观测能力

服务端已经通过 `ScopedTimer` 在关键路由打印请求耗时：

```text
[perf] uploads.init elapsed_ms=1
[perf] uploads.chunk elapsed_ms=0
[perf] uploads.status elapsed_ms=2
[perf] uploads.complete elapsed_ms=1
```

这类日志回答的是“某个接口处理一次请求用了多久”。它适合作为第一层观测，但不足以单独证明吞吐能力，所以新增了 benchmark 脚本来测完整上传链路。

## 运行 Benchmark

先启动本地服务，并设置 token：

```powershell
$env:PHOTO_BRIDGE_TOKEN = "your-token"
```

运行默认 benchmark：

```powershell
.\scripts\benchmark_upload.ps1
```

默认会测试：

- 文件大小：16 MiB、64 MiB
- chunk 大小：1 MiB、2 MiB、4 MiB
- 完整链路：init -> chunk upload -> status -> complete -> delete

也可以指定更接近服务器验证的参数：

```powershell
.\scripts\benchmark_upload.ps1 -FileSizeMB 16,64,256 -ChunkSizeMB 1,2,4
```

指定远端服务器：

```powershell
.\scripts\benchmark_upload.ps1 -BaseUrl "http://your-server:8080" -Token "your-token" -FileSizeMB 16,64 -ChunkSizeMB 2,4
```

如果想保留 benchmark 上传后的文件用于手动检查：

```powershell
.\scripts\benchmark_upload.ps1 -KeepUploadedFiles
```

## 输出文件

脚本会把每次测试结果追加到：

```text
docs/performance-results.jsonl
```

每一行是一条 JSON 记录，示例：

```json
{"timestamp":"2026-05-25T17:30:00.0000000+08:00","base_url":"http://127.0.0.1:8080","filename":"bench-20260525173000-16m-2m.bin","file_size_mib":16,"chunk_size_mib":2,"chunk_count":8,"init_ms":1.2,"upload_ms":45.8,"status_ms":0.9,"complete_ms":12.4,"total_ms":61.5,"avg_chunk_ms":5.3,"p95_chunk_ms":7.1,"total_throughput_mib_s":260.16,"upload_throughput_mib_s":349.34,"complete_throughput_mib_s":1290.32}
```

## 指标解释

| 指标 | 含义 | 面试里可以说明什么 |
|---|---|---|
| `file_size_mib` | 测试文件大小 | 数据规模 |
| `chunk_size_mib` | 单个分片大小 | 分片策略 |
| `chunk_count` | 总分片数 | 请求数量和元数据压力 |
| `init_ms` | 初始化会话耗时 | session 元数据创建成本 |
| `upload_ms` | 所有 chunk 上传总耗时 | 主要写入路径吞吐 |
| `status_ms` | 查询断点续传状态耗时 | replay/status 检查成本 |
| `complete_ms` | 合并分片耗时 | 顺序读 chunk + 写最终文件成本 |
| `total_ms` | 完整链路总耗时 | 用户视角端到端延迟 |
| `avg_chunk_ms` | chunk 平均上传耗时 | 单 chunk 写入稳定性 |
| `p95_chunk_ms` | chunk 上传 P95 | 尾延迟 |
| `total_throughput_mib_s` | 端到端吞吐 | 整体系统能力 |
| `upload_throughput_mib_s` | 上传阶段吞吐 | 网络/写盘/校验综合能力 |
| `complete_throughput_mib_s` | 合并阶段吞吐 | 本地存储读写能力 |

## 与系统设计的映射

| Benchmark 阶段 | 系统设计路径 | 主要代码模块 | 观测意义 |
|---|---|---|---|
| init | 创建分片上传 session | `HttpServer` / `ChunkUploadStore` | 观察 session metadata 初始化成本 |
| chunk upload | 写 chunk part、计算 CRC32C、写 chunk meta | `ChunkUploadStore` / `Checksum` | 观察主要写入路径吞吐和单 chunk 延迟 |
| status | 遍历 part/meta，判断 uploaded/missing | `ChunkUploadStore` | 观察断点续传状态查询成本 |
| complete | 校验所有 chunk，合并最终文件，追加 metadata | `ChunkUploadStore` / `MetadataStore` | 观察 commit 阶段顺序 IO 和元数据写入成本 |
| delete cleanup | 删除 benchmark 产物并写 tombstone | `FileStore` / `MetadataStore` | 避免 benchmark 污染文件列表 |

因此，benchmark 数字应该和系统设计一起读：当 `chunk_size_mib` 增大时，请求数下降，HTTP 往返和 per-chunk metadata 成本下降；当 `file_size_mib` 增大时，`complete_ms` 近似线性增长，说明 complete 阶段主要受顺序读写影响。

## 如何分析结果

优先看三个方向：

1. chunk 过小时，请求数量变多，`chunk_count` 增加，HTTP 调用和元数据操作成本会上升。
2. chunk 过大时，单次请求体变大，失败重传成本也会变大。
3. `complete_ms` 随文件大小线性增长，说明 complete 阶段主要是顺序读取 chunk 并写最终文件。

如果 `upload_ms` 占比明显高，下一步关注 chunk 写入、CRC32C 计算和 HTTP 请求开销。

如果 `complete_ms` 占比明显高，下一步关注合并过程是否可以减少拷贝、使用更大的缓冲区、或者改成后台 commit。

如果 `p95_chunk_ms` 远高于 `avg_chunk_ms`，说明存在尾延迟，可能与磁盘抖动、GC/系统调度、控制台日志输出或网络波动有关。

## 面试叙事

这部分可以这样讲：

> 我没有只实现一个上传接口，而是把它拆成了可观测的数据路径。分片上传会经过 session 初始化、chunk 写入、CRC32C 校验、status 查询、complete 合并和 metadata 追加。我用 smoke test 保证正确性，再用 benchmark 脚本测不同文件大小和 chunk 大小下的吞吐、P95 chunk 延迟和 complete 合并成本。这样可以分析 chunk 策略对系统吞吐和失败重传成本的影响。

这个叙事比“我做了上传功能”更接近分布式存储岗位关注的能力：

- 数据路径设计
- 元数据一致性
- 完整性校验
- 故障恢复
- 性能观测
- 参数化 benchmark

## 首次 Benchmark 结果，2026-05-25

测试方式：

- 本地测试：Windows 本机请求 `http://127.0.0.1:8080`。
- 远端测试：Windows 本机通过公网请求阿里云服务器 `http://120.79.145.171:8080`。
- 测试链路：`init -> chunk upload -> status -> complete -> delete`。

| 环境 | 文件大小 | Chunk 大小 | Chunk 数 | 总耗时 ms | 上传耗时 ms | 合并耗时 ms | 端到端吞吐 MiB/s | 平均 Chunk ms | P95 Chunk ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 本地 | 16 MiB | 1 MiB | 16 | 3654.63 | 2497.12 | 585.91 | 4.38 | 42.64 | 44.76 |
| 本地 | 16 MiB | 2 MiB | 8 | 3417.62 | 2305.47 | 556.46 | 4.68 | 78.74 | 80.99 |
| 本地 | 16 MiB | 4 MiB | 4 | 3207.50 | 2099.15 | 558.83 | 4.99 | 143.36 | 145.62 |
| 本地 | 64 MiB | 1 MiB | 64 | 14017.48 | 9520.53 | 2262.35 | 4.57 | 42.02 | 43.80 |
| 本地 | 64 MiB | 2 MiB | 32 | 13131.98 | 8627.49 | 2261.71 | 4.87 | 75.31 | 79.70 |
| 本地 | 64 MiB | 4 MiB | 16 | 12527.76 | 8089.42 | 2223.34 | 5.11 | 145.07 | 150.67 |
| 远端公网 | 16 MiB | 2 MiB | 8 | 8294.14 | 5506.54 | 1259.68 | 1.93 | 499.45 | 540.79 |
| 远端公网 | 16 MiB | 4 MiB | 4 | 7300.70 | 4943.27 | 1120.56 | 2.19 | 872.39 | 894.34 |
| 远端公网 | 64 MiB | 2 MiB | 32 | 35883.90 | 26968.06 | 4471.81 | 1.78 | 643.82 | 1494.33 |
| 远端公网 | 64 MiB | 4 MiB | 16 | 29096.48 | 20090.99 | 4352.43 | 2.20 | 889.28 | 1156.38 |

关键观察：

- 本地环境中，Chunk 从 1 MiB 增大到 4 MiB 后，请求数下降，端到端吞吐从约 4.38 MiB/s 提升到约 5.11 MiB/s。
- 远端公网环境中，整体吞吐约 1.8-2.2 MiB/s，明显低于本地，主要体现公网传输和远端处理的端到端成本。
- 远端 4 MiB Chunk 整体优于 2 MiB Chunk，说明公网场景下减少 HTTP 请求次数能带来收益。
- `complete_ms` 随文件大小近似线性增长，本地 16 MiB 约 0.56s，64 MiB 约 2.25s；远端 16 MiB 约 1.1-1.26s，64 MiB 约 4.35-4.47s，符合顺序读取 chunk 并合并写最终文件的特征。

## 下一步

当前 benchmark 仍是单客户端顺序上传。后续可以继续增强：

- 增加并发上传参数，模拟多客户端写入。
- 把 `ScopedTimer` 从毫秒升级到微秒，提升小请求观测精度。
- 把日志从 `std::cout` 收口到 `Logger`，再决定是否做异步日志。
- 生成 `docs/performance-summary.md`，把多次 JSONL 结果聚合成表格。
- 在 V3.0 引入 `StorageBackend` 抽象，为本地磁盘、多副本、分片和对象存储后端做铺垫。
