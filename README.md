# PhotoBridge

[English](README.md) | [简体中文](README.zh-CN.md)

PhotoBridge is a lightweight C++ HTTP storage service for private file transfer, resumable uploads, and storage-backend experiments.

It started as a small browser-based file bridge, then evolved into a compact backend project with metadata replay, chunk upload, CRC32C verification, sharding, replication, node availability, placement inspection, and replica repair.

## Features

- HTTP file upload, download, list, and delete.
- Append-only JSONL metadata log with replay, audit, repair, and compaction.
- Resumable chunk upload with session status, abort, cleanup, and CRC32C chunk verification.
- Object-style storage backend abstraction.
- Local, sharded, and replicated storage backend implementations.
- Storage node abstraction with availability controls.
- Placement, replica audit, and replica repair APIs.
- PowerShell smoke test and upload benchmark scripts.

## Tech Stack

- C++20
- CMake
- cpp-httplib
- JSONL metadata
- Local filesystem storage
- PowerShell scripts for smoke and benchmark testing

## Architecture

```text
Client / Browser / curl
        |
        v
HttpServer
        |
        +-- FileStore
        |
        +-- MetadataStore
        |
        +-- ChunkUploadStore
                |
                v
        ReplicaStorageBackend
                |
                v
        StorageNode -> LocalStorageBackend
```

Runtime data is stored under `data/` and is ignored by Git.

```text
data/
├── metadata/       # append-only metadata log
├── uploads/        # completed files
├── uploads_tmp/    # chunk upload sessions
└── shards/         # local shard / replica data
```

## Build

### Windows

```powershell
cd D:\PhotoBridge
cmake -S . -B build
cmake --build build
```

Run:

```powershell
$env:PHOTO_BRIDGE_TOKEN="change-me"
.\build\PhotoBridge.exe
```

### Linux

```bash
git clone https://github.com/Bryson-wz/PhotoBridge.git
cd PhotoBridge
cmake -S . -B build
cmake --build build
```

Run:

```bash
export PHOTO_BRIDGE_TOKEN="change-me"
./build/PhotoBridge
```

The service listens on `0.0.0.0:8080` by default.

For long-running Linux deployment, use a process manager such as `systemd` or `supervisor`.

## Quick Start

Health check:

```bash
curl "http://127.0.0.1:8080/health"
```

Upload a file:

```bash
curl -X POST \
  "http://127.0.0.1:8080/api/upload?token=change-me&filename=hello.txt" \
  --data-binary "hello photobridge"
```

List files:

```bash
curl "http://127.0.0.1:8080/api/files?token=change-me"
```

Download a file:

```bash
curl -OJ "http://127.0.0.1:8080/api/files/hello.txt/download?token=change-me"
```

Delete a file:

```bash
curl -X POST "http://127.0.0.1:8080/api/files/delete?token=change-me&filename=hello.txt"
```

## Resumable Upload

Initialize an upload session:

```bash
curl -X POST \
  "http://127.0.0.1:8080/api/uploads/init?token=change-me&filename=big.bin&size=11&chunk_size=6"
```

Upload chunks:

```bash
curl -X POST \
  "http://127.0.0.1:8080/api/uploads/chunk?token=change-me&session_id=<session_id>&index=0" \
  --data-binary "hello "

curl -X POST \
  "http://127.0.0.1:8080/api/uploads/chunk?token=change-me&session_id=<session_id>&index=1" \
  --data-binary "world"
```

Check upload status:

```bash
curl "http://127.0.0.1:8080/api/uploads/status?token=change-me&session_id=<session_id>"
```

Complete upload:

```bash
curl -X POST "http://127.0.0.1:8080/api/uploads/complete?token=change-me&session_id=<session_id>"
```

Abort upload:

```bash
curl -X POST "http://127.0.0.1:8080/api/uploads/abort?token=change-me&session_id=<session_id>"
```

## API Overview

| Method | Path | Description |
| --- | --- | --- |
| `GET` | `/health` | Health check |
| `GET` | `/` | Browser upload page |
| `POST` | `/api/upload` | Raw body upload |
| `POST` | `/api/upload-form` | Form upload |
| `GET` | `/api/files` | List visible files |
| `GET` | `/api/files/{filename}/download` | Download file |
| `POST` | `/api/files/delete` | Delete file and append tombstone |
| `GET` | `/api/metadata` | Read metadata log |
| `GET` | `/api/metadata/audit` | Audit metadata against uploads |
| `POST` | `/api/metadata/repair` | Repair safe metadata issues |
| `POST` | `/api/metadata/compact` | Compact metadata log |
| `POST` | `/api/uploads/init` | Create chunk upload session |
| `POST` | `/api/uploads/chunk` | Upload one chunk |
| `GET` | `/api/uploads/status` | Query uploaded and missing chunks |
| `POST` | `/api/uploads/complete` | Merge chunks into final object |
| `POST` | `/api/uploads/abort` | Remove an upload session |
| `POST` | `/api/uploads/cleanup-expired` | Cleanup expired sessions |
| `GET` | `/api/storage/nodes` | List storage nodes |
| `POST` | `/api/storage/node/availability` | Toggle node availability |
| `GET` | `/api/storage/placement` | Inspect object placement |
| `GET` | `/api/storage/replicas/audit` | Audit object replicas |
| `POST` | `/api/storage/replicas/repair` | Repair missing replicas |

Most mutating APIs require `token=<PHOTO_BRIDGE_TOKEN>`.

## Testing

Start the service first, then run:

```powershell
cd D:\PhotoBridge
$env:PHOTO_BRIDGE_TOKEN="change-me"
.\scripts\smoke_test.ps1
```

Run upload benchmarks:

```powershell
.\scripts\benchmark_upload.ps1 `
  -BaseUrl "http://127.0.0.1:8080" `
  -Token "change-me" `
  -FileSizeMB 16,64 `
  -ChunkSizeMB 1,2,4
```

## Configuration

The service currently uses environment variables and local runtime directories.

| Name | Description |
| --- | --- |
| `PHOTO_BRIDGE_TOKEN` | Shared token required by protected APIs |

Do not commit runtime data or private config files.

## Repository Layout

```text
include/photobridge/   Public headers
src/                   C++ implementation
web/                   Browser upload page
scripts/               Smoke and benchmark scripts
third_party/           Header-only dependencies
data/                  Runtime data, ignored by Git
```

## Roadmap

- Profiling with `perf`, `strace`, and flamegraph.
- More complete storage-node failure simulation.
- Stronger metadata recovery and compaction safeguards.
- Linux deployment assets such as `systemd` service templates.
- Optional protocol-layer experiments after the storage layer stabilizes.

## License

No license has been selected yet.
