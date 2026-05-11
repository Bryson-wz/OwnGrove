# PhotoBridge

PhotoBridge 是一个面向家庭照片流转场景的私有文件中转服务。第一版目标很小：把照片放到一台 Linux 服务器上，让另一台设备可以通过浏览器访问服务、查看文件列表，后续再逐步加入上传、下载、鉴权、日志和元数据管理。

## 当前进度

- C++20 + CMake 项目骨架。
- 使用 `cpp-httplib` 提供最小 HTTP 服务。
- `GET /health` 返回健康检查结果。
- `GET /` 返回 `web/index.html` 首页。
- `GET /api/files` 遍历 `data/uploads` 并返回 JSON 文件列表。
- `GET /api/files/{filename}/download` 下载指定文件。
- `POST /api/upload` 支持 raw body 上传文件。
- 文件列表、下载、上传接口已经接入最小 token 鉴权。
- 已在 Alibaba Cloud Linux 3 上完成公网烟雾测试。

## 目录结构

```text
PhotoBridge/
├── CMakeLists.txt
├── include/photobridge/
├── src/
├── web/
├── config/
├── data/uploads/
└── data/logs/
```

`data/uploads/` 和 `data/logs/` 不提交到 Git，用于本地或服务器运行时数据。

## Windows 本地构建

```powershell
cd D:\PhotoBridge
cmake -S . -B build
cmake --build build
$env:PHOTO_BRIDGE_TOKEN="your-token"
.\build\PhotoBridge.exe
```

访问：

```text
http://localhost:8080/health
http://localhost:8080/
http://localhost:8080/api/files?token=your-token
```

Windows 下建议使用 `curl.exe`，避免 PowerShell 的 `curl` 别名干扰。

## Linux 构建

以 Alibaba Cloud Linux 3 / RHEL 系为例：

```bash
dnf install -y gcc gcc-c++ make cmake git tar gzip
```

构建并运行：

```bash
cd /root/PhotoBridge
cmake -S . -B build
cmake --build build
PHOTO_BRIDGE_TOKEN=your-token ./build/PhotoBridge
```

服务默认监听：

```text
0.0.0.0:8080
```

如果 `build/` 是从 Windows 复制过来的，会出现 CMakeCache 路径错误。Linux 上应删除后重新生成：

```bash
rm -rf build
cmake -S . -B build
cmake --build build
```

## 跨平台构建说明

- Windows 下 `cpp-httplib` 需要链接 `ws2_32`。
- Linux 下 `std::thread` / `cpp-httplib` 需要链接 `Threads::Threads`。
- 当前 `CMakeLists.txt` 已包含这两类平台差异处理。

相关 CMake 片段：

```cmake
find_package(Threads REQUIRED)
target_link_libraries(PhotoBridge PRIVATE Threads::Threads)

if(WIN32)
    target_compile_definitions(PhotoBridge PRIVATE
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
    )

    target_link_libraries(PhotoBridge PRIVATE ws2_32)
endif()
```

## 远程访问

服务器运行后，可以通过公网短时访问：

```text
http://<服务器IP>:8080/health
http://<服务器IP>:8080/
http://<服务器IP>:8080/api/files?token=your-token
```

如果不想开放公网端口，也可以使用 SSH 隧道：

```powershell
ssh -L 18080:127.0.0.1:8080 root@<服务器IP>
```

然后本机浏览器访问：

```text
http://localhost:18080/
```

## API 示例

### 健康检查

```bash
curl http://127.0.0.1:8080/health
```

### 文件列表

```bash
curl "http://127.0.0.1:8080/api/files?token=your-token"
```

### 下载文件

```bash
curl -OJ "http://127.0.0.1:8080/api/files/test.jpg/download?token=your-token"
```

浏览器下载时服务端会通过 `Content-Disposition` 保留原始文件名。

### Raw body 上传文本

```bash
curl -X POST "http://127.0.0.1:8080/api/upload?token=your-token&filename=hello.txt" \
  --data-binary "hello from upload"
```

PowerShell 写成一行：

```powershell
curl.exe -X POST "http://127.0.0.1:8080/api/upload?token=your-token&filename=hello.txt" --data-binary "hello from upload"
```

### Raw body 上传图片

```powershell
curl.exe -X POST "http://<服务器IP>:8080/api/upload?token=your-token&filename=test.jpg" --data-binary "@C:\path\to\test.jpg"
```

上传后验证：

```text
http://<服务器IP>:8080/api/files?token=your-token
http://<服务器IP>:8080/api/files/test.jpg/download?token=your-token
```

## Git 同步

GitHub 仓库：

```text
https://github.com/xuan1031-0/PhotoBridge
```

常用流程：

```powershell
git -C D:\PhotoBridge status
git -C D:\PhotoBridge add .
git -C D:\PhotoBridge commit -m "feat: describe change"
git -C D:\PhotoBridge push
```

服务器侧如果已经配置好 GitHub 访问：

```bash
cd /root/PhotoBridge
git pull
cmake --build build
PHOTO_BRIDGE_TOKEN=your-token ./build/PhotoBridge
```

## 安全提醒

当前阶段只适合短时开发测试。虽然文件接口已有最小 token 鉴权，但仍不建议长期对公网开放 `8080`。

后续需要继续补：

- token 从配置文件或更安全的环境管理中读取。
- 请求日志和错误日志。
- 上传文件大小限制。
- 更完整的文件名安全检查。
- Nginx 反代与 HTTPS。
- 后台运行方式，例如 `systemd`。
