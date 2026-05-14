# PhotoBridge

PhotoBridge 是一个面向家庭照片流转场景的私有文件中转服务。最初的问题很具体：拍完照片后，希望另一台设备可以不用反复依赖隔空投送，也能通过浏览器访问、上传、下载和管理文件。

当前项目同时也是一个 C++ 后端学习项目：从能跑通的 HTTP 文件服务开始，逐步演进出对象存储里常见的元数据日志、状态 replay、tombstone、巡检修复和 compaction。

## 当前能力

- C++20 + CMake + `cpp-httplib` 实现 HTTP 服务。
- `GET /health` 健康检查。
- `GET /` 返回 `web/index.html` 上传页面。
- `POST /api/upload` 支持 raw body 上传。
- `POST /api/upload-form` 支持浏览器表单上传。
- `GET /api/files` 基于 metadata replay 返回当前可见文件。
- `GET /api/files/{filename}/download` 下载指定文件。
- `POST /api/files/delete` 删除真实文件，并写入 `delete/deleted` tombstone。
- `GET /api/metadata` 查看原始 JSONL metadata log。
- `GET /api/metadata/count` 返回 metadata 记录数。
- `GET /api/metadata/audit` 对比 metadata 与 `data/uploads`，发现 `MissingFile` / `OrphanFile`。
- `POST /api/metadata/repair` 自动修复低风险 `MissingFile`，再返回剩余问题数量。
- `POST /api/metadata/repair-missing` 单文件修复缺失状态。
- `POST /api/metadata/mark-missing` 手动标记文件缺失。
- `POST /api/metadata/compact` 将 append-only metadata log 压缩为每个文件最新状态。
- 文件接口使用 `PHOTO_BRIDGE_TOKEN` 做最小 token 鉴权。
- 已在 Windows 本地和 Alibaba Cloud Linux 3 上完成基础运行验证。

## 项目结构

```text
PhotoBridge/
├── CMakeLists.txt
├── include/photobridge/
│   ├── FileMetadata.h
│   ├── FileStore.h
│   ├── HttpServer.h
│   └── MetadataStore.h
├── src/
│   ├── FileStore.cpp
│   ├── HttpServer.cpp
│   ├── MetadataStore.cpp
│   └── main.cpp
├── web/
├── config/
├── data/
│   ├── uploads/
│   ├── metadata/
│   └── logs/
└── third_party/
```

`data/uploads/`、`data/metadata/`、`data/logs/` 是运行时数据目录，不提交到 Git。

## 构建运行

### Windows

```powershell
cd D:\PhotoBridge
cmake -S . -B build
cmake --build build
$env:PHOTO_BRIDGE_TOKEN="your-token"
.\build\PhotoBridge.exe
```

访问：

```text
http://127.0.0.1:8080/health
http://127.0.0.1:8080/
http://127.0.0.1:8080/api/files?token=your-token
```

Windows 下建议使用 `curl.exe`，避免 PowerShell 的 `curl` 别名干扰。

### Linux

以 Alibaba Cloud Linux 3 / RHEL 系为例：

```bash
dnf install -y gcc gcc-c++ make cmake git tar gzip
cd /root/PhotoBridge
cmake -S . -B build
cmake --build build
PHOTO_BRIDGE_TOKEN=your-token ./build/PhotoBridge
```

服务默认监听：

```text
0.0.0.0:8080
```

如果 `build/` 是从 Windows 复制过来的，需要删除后在 Linux 上重新生成：

```bash
rm -rf build
cmake -S . -B build
cmake --build build
```

## API 示例

### 健康检查

```bash
curl "http://127.0.0.1:8080/health"
```

### 文件列表

```bash
curl "http://127.0.0.1:8080/api/files?token=your-token"
```

### Raw body 上传

```powershell
curl.exe -X POST "http://127.0.0.1:8080/api/upload?token=your-token&filename=hello.txt" --data-binary "hello from upload"
```

### 表单上传

```powershell
curl.exe -X POST "http://127.0.0.1:8080/api/upload-form?token=your-token" -F "file=@D:\PhotoBridge\web\index.html"
```

### 下载

```bash
curl -OJ "http://127.0.0.1:8080/api/files/test.jpg/download?token=your-token"
```

### 删除文件并写入 tombstone

```powershell
curl.exe -X POST "http://127.0.0.1:8080/api/files/delete?token=your-token&filename=test.jpg"
```

### 查看 metadata log

```bash
curl "http://127.0.0.1:8080/api/metadata?token=your-token"
```

### 巡检 metadata 与真实文件

```bash
curl "http://127.0.0.1:8080/api/metadata/audit?token=your-token"
```

### 自动修复 MissingFile

```powershell
curl.exe -X POST "http://127.0.0.1:8080/api/metadata/repair?token=your-token"
```

### 压缩 metadata log

```powershell
curl.exe -X POST "http://127.0.0.1:8080/api/metadata/compact?token=your-token"
```

## Metadata 设计

PhotoBridge 当前使用 append-only JSONL 记录文件状态变化：

```json
{"schema_version":1,"op":"upload","filename":"a.jpg","content_type":"image/jpeg","size":12345,"uploaded_at":"2026-05-14T08:00:00Z","status":"completed"}
{"schema_version":1,"op":"delete","filename":"a.jpg","content_type":"","size":0,"uploaded_at":"2026-05-14T08:10:00Z","status":"deleted"}
```

核心思路：

- `appendFile()` 只追加新记录，不原地修改旧记录。
- `listLatestRecords()` 按 `filename` replay 出每个文件最新状态。
- `listFiles()` 只展示最终状态为 `completed` 的文件。
- `delete` 不只是删除真实文件，还追加 `deleted` tombstone。
- `repair` 会自动把 `MissingFile` 写成 `repair-missing/missing`。
- `compact()` 会把多条历史记录压缩为每个文件一条最新状态。

`compact()` 使用中间文件保护：

```text
files.jsonl.tmp  新压缩结果
files.jsonl.bak  旧 metadata 备份
```

流程是先写 `.tmp`，再把旧文件改名为 `.bak`，最后用 `.tmp` 替换正式文件；如果替换失败，会尽量恢复 `.bak`。

## 跨平台说明

- Windows 下 `cpp-httplib` 需要链接 `ws2_32`。
- Linux 下 `std::thread` / `cpp-httplib` 需要链接 `Threads::Threads`。
- 当前 `CMakeLists.txt` 已处理 `_WIN32_WINNT=0x0A00`、`WINVER=0x0A00`、`ws2_32` 和 `Threads::Threads`。

## 当前限制

- 这是学习与实验项目，不建议长期直接暴露公网 `8080`。
- token 鉴权仍是最小实现，不是完整用户系统。
- JSONL 解析器只面向本项目固定格式，不是通用 JSON parser。
- `compact()` 当前会跳过解析失败的记录，后续需要补 `skipped_records` 统计或失败保护。
- `remaining_issues` 目前返回数量，后续可升级为完整 JSON 数组。
- 还没有 HTTPS、Nginx 反代、systemd 后台服务和上传断点续传。

## 下一步

- 为 `compact()` 增加解析失败记录统计与保护策略。
- 将 `/api/metadata/repair` 的 `remaining_issues` 从数量升级为问题数组。
- 抽出公共文件名校验与 JSON 输出工具。
- 增加元数据修复/压缩的本地验收脚本。
- 继续推进分片上传与断点续传。
