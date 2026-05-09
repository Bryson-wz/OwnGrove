# PhotoBridge

PhotoBridge 是一个面向家庭照片流转场景的私有文件中转服务。第一版目标很小：把照片放到一台 Linux 服务器上，让另一台设备可以通过浏览器访问服务、查看文件列表，后续再逐步加入上传、下载、鉴权、日志和元数据管理。

## 当前进度

- C++20 + CMake 项目骨架。
- 使用 `cpp-httplib` 提供最小 HTTP 服务。
- `GET /health` 返回健康检查结果。
- `GET /` 返回 `web/index.html` 首页。
- `GET /api/files` 遍历 `data/uploads` 并返回 JSON 文件列表。
- 已在 Alibaba Cloud Linux 3 上完成公网烟雾测试。

## 本地构建

```powershell
cmake -S . -B build
cmake --build build
.\build\PhotoBridge.exe
```

访问：

```text
http://localhost:8080/health
http://localhost:8080/
http://localhost:8080/api/files
```

## Linux 构建

```bash
cmake -S . -B build
cmake --build build
./build/PhotoBridge
```

服务默认监听：

```text
0.0.0.0:8080
```

## 跨平台构建说明

- Windows 下 `cpp-httplib` 需要链接 `ws2_32`。
- Linux 下 `std::thread` / `cpp-httplib` 需要链接 `Threads::Threads`。
- 当前 `CMakeLists.txt` 已包含这两类平台差异处理。

## 安全提醒

当前阶段只适合短时开发测试。上传接口上线前，不建议长期对公网开放 `8080`。后续需要加入 token、日志、错误处理，并考虑 Nginx 反代或 HTTPS。
