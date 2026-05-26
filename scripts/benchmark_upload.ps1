param(
    [string]$BaseUrl = "http://127.0.0.1:8080",
    [string]$Token = $env:PHOTO_BRIDGE_TOKEN,
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [int[]]$FileSizeMB = @(16, 64),
    [int[]]$ChunkSizeMB = @(1, 2, 4),
    [string]$OutputPath = "docs/performance-results.jsonl",
    [switch]$KeepUploadedFiles
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Token)) {
    throw "PHOTO_BRIDGE_TOKEN is not set. Pass -Token or set `$env:PHOTO_BRIDGE_TOKEN first."
}

function New-ApiUrl {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [hashtable]$Query = @{}
    )

    $queryParts = @("token=$([uri]::EscapeDataString($Token))")
    foreach ($key in $Query.Keys) {
        $queryParts += "$([uri]::EscapeDataString($key))=$([uri]::EscapeDataString([string]$Query[$key]))"
    }

    return "$BaseUrl$Path`?$($queryParts -join '&')"
}

function Invoke-GetMeasured {
    param([Parameter(Mandatory = $true)][string]$Url)

    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $content = (Invoke-WebRequest -Uri $Url -Method Get -UseBasicParsing).Content
    $timer.Stop()

    return [pscustomobject]@{
        Content = $content
        ElapsedMs = $timer.Elapsed.TotalMilliseconds
    }
}

function Invoke-PostMeasured {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [object]$Body = $null,
        [string]$ContentType = "text/plain"
    )

    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    if ($null -eq $Body) {
        $content = (Invoke-WebRequest -Uri $Url -Method Post -UseBasicParsing).Content
    } else {
        $content = (Invoke-WebRequest -Uri $Url -Method Post -Body $Body -ContentType $ContentType -UseBasicParsing).Content
    }
    $timer.Stop()

    return [pscustomobject]@{
        Content = $content
        ElapsedMs = $timer.Elapsed.TotalMilliseconds
    }
}

function Invoke-PostFileMeasured {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$ContentType = "application/octet-stream"
    )

    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $content = (Invoke-WebRequest -Uri $Url -Method Post -InFile $Path -ContentType $ContentType -UseBasicParsing).Content
    $timer.Stop()

    return [pscustomobject]@{
        Content = $content
        ElapsedMs = $timer.Elapsed.TotalMilliseconds
    }
}

function New-BenchmarkFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int64]$SizeBytes
    )

    $bufferSize = 1024 * 1024
    $remaining = $SizeBytes
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)

    try {
        while ($remaining -gt 0) {
            $writeSize = [int][Math]::Min($bufferSize, $remaining)
            $buffer = New-Object byte[] $writeSize
            $rng.GetBytes($buffer)
            $stream.Write($buffer, 0, $writeSize)
            $remaining -= $writeSize
        }
    } finally {
        $stream.Dispose()
        $rng.Dispose()
    }
}

function Read-ExactBytes {
    param(
        [Parameter(Mandatory = $true)][System.IO.Stream]$Stream,
        [Parameter(Mandatory = $true)][int]$Size
    )

    $buffer = New-Object byte[] $Size
    $offset = 0

    while ($offset -lt $Size) {
        $read = $Stream.Read($buffer, $offset, $Size - $offset)
        if ($read -le 0) {
            throw "Unexpected end of file while reading benchmark chunk."
        }

        $offset += $read
    }

    return $buffer
}

function Get-PercentileValue {
    param(
        [double[]]$Values,
        [double]$Percentile
    )

    if ($Values.Count -eq 0) {
        return 0.0
    }

    $sorted = @($Values | Sort-Object)
    $index = [int][Math]::Ceiling(($Percentile / 100.0) * $sorted.Count) - 1

    if ($index -lt 0) {
        $index = 0
    }

    if ($index -ge $sorted.Count) {
        $index = $sorted.Count - 1
    }

    return [double]$sorted[$index]
}

function Format-Number {
    param([double]$Value)
    return [Math]::Round($Value, 2)
}

if ([System.IO.Path]::IsPathRooted($OutputPath)) {
    $resolvedOutputPath = $OutputPath
} else {
    $resolvedOutputPath = Join-Path $ProjectRoot $OutputPath
}

$outputDir = Split-Path -Parent $resolvedOutputPath
if (-not [string]::IsNullOrWhiteSpace($outputDir)) {
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
}

$benchmarkDir = Join-Path $ProjectRoot "data\benchmark"
New-Item -ItemType Directory -Force -Path $benchmarkDir | Out-Null

Write-Host "PhotoBridge chunk upload benchmark"
Write-Host "BaseUrl: $BaseUrl"
Write-Host "Output : $resolvedOutputPath"
Write-Host ""

$health = Invoke-GetMeasured "$BaseUrl/health"
if (-not $health.Content.Contains("OK")) {
    throw "Health check failed. Actual: $($health.Content)"
}

$runId = Get-Date -Format "yyyyMMddHHmmss"
$results = @()

foreach ($fileSize in $FileSizeMB) {
    foreach ($chunkSize in $ChunkSizeMB) {
        if ($chunkSize -gt $fileSize) {
            Write-Host "Skip file=${fileSize}MiB chunk=${chunkSize}MiB because chunk is larger than file."
            continue
        }

        $fileSizeBytes = [int64]$fileSize * 1024 * 1024
        $chunkSizeBytes = [int64]$chunkSize * 1024 * 1024
        $chunkCount = [int][Math]::Ceiling($fileSizeBytes / [double]$chunkSizeBytes)
        $filename = "bench-$runId-${fileSize}m-${chunkSize}m.bin"
        $localFile = Join-Path $benchmarkDir $filename
        $chunkTempFile = Join-Path $benchmarkDir "$filename.chunk.tmp"
        $sessionId = $null
        $completed = $false

        Write-Host "Run file=${fileSize}MiB chunk=${chunkSize}MiB chunks=$chunkCount"

        try {
            New-BenchmarkFile -Path $localFile -SizeBytes $fileSizeBytes

            $totalTimer = [System.Diagnostics.Stopwatch]::StartNew()

            $initUrl = New-ApiUrl "/api/uploads/init" @{
                filename = $filename
                size = $fileSizeBytes
                chunk_size = $chunkSizeBytes
            }
            $initMeasured = Invoke-PostMeasured $initUrl
            $init = $initMeasured.Content | ConvertFrom-Json

            if ([string]::IsNullOrWhiteSpace($init.session_id)) {
                throw "Upload init did not return session_id. Actual: $($initMeasured.Content)"
            }

            $sessionId = $init.session_id
            $chunkElapsedMs = New-Object System.Collections.Generic.List[double]
            $uploadTimer = [System.Diagnostics.Stopwatch]::StartNew()
            $reader = [System.IO.File]::OpenRead($localFile)

            try {
                for ($index = 0; $index -lt $chunkCount; $index++) {
                    $remainingBytes = $reader.Length - $reader.Position
                    $currentChunkSize = [int][Math]::Min($chunkSizeBytes, $remainingBytes)
                    $chunkBody = Read-ExactBytes -Stream $reader -Size $currentChunkSize
                    $chunkUrl = New-ApiUrl "/api/uploads/chunk" @{
                        session_id = $sessionId
                        index = $index
                    }

                    [System.IO.File]::WriteAllBytes($chunkTempFile, $chunkBody)
                    $chunkMeasured = Invoke-PostFileMeasured -Url $chunkUrl -Path $chunkTempFile
                    $chunkResponse = $chunkMeasured.Content | ConvertFrom-Json

                    if ($chunkResponse.result -ne "success") {
                        throw "Chunk $index upload failed. Actual: $($chunkMeasured.Content)"
                    }

                    $chunkElapsedMs.Add([double]$chunkMeasured.ElapsedMs)
                }
            } finally {
                $reader.Dispose()
            }

            $uploadTimer.Stop()

            $statusUrl = New-ApiUrl "/api/uploads/status" @{
                session_id = $sessionId
            }
            $statusMeasured = Invoke-GetMeasured $statusUrl
            $status = $statusMeasured.Content | ConvertFrom-Json

            if ($status.missing_count -ne 0) {
                throw "Upload status still has missing chunks. Actual: $($statusMeasured.Content)"
            }

            $completeUrl = New-ApiUrl "/api/uploads/complete" @{
                session_id = $sessionId
            }
            $completeMeasured = Invoke-PostMeasured $completeUrl
            $complete = $completeMeasured.Content | ConvertFrom-Json

            if ($complete.result -ne "success") {
                throw "Complete failed. Actual: $($completeMeasured.Content)"
            }

            if ($complete.size -ne $fileSizeBytes) {
                throw "Complete returned unexpected size. Expected: $fileSizeBytes. Actual: $($complete.size)"
            }

            $completed = $true
            $totalTimer.Stop()

            $fileMiB = $fileSizeBytes / 1024.0 / 1024.0
            $totalSeconds = [Math]::Max($totalTimer.Elapsed.TotalSeconds, 0.001)
            $uploadSeconds = [Math]::Max($uploadTimer.Elapsed.TotalSeconds, 0.001)
            $completeSeconds = [Math]::Max($completeMeasured.ElapsedMs / 1000.0, 0.001)

            $result = [ordered]@{
                timestamp = (Get-Date).ToString("o")
                base_url = $BaseUrl
                filename = $filename
                file_size_mib = $fileSize
                chunk_size_mib = $chunkSize
                file_size_bytes = $fileSizeBytes
                chunk_size_bytes = $chunkSizeBytes
                chunk_count = $chunkCount
                init_ms = Format-Number $initMeasured.ElapsedMs
                upload_ms = Format-Number $uploadTimer.Elapsed.TotalMilliseconds
                status_ms = Format-Number $statusMeasured.ElapsedMs
                complete_ms = Format-Number $completeMeasured.ElapsedMs
                total_ms = Format-Number $totalTimer.Elapsed.TotalMilliseconds
                avg_chunk_ms = Format-Number (($chunkElapsedMs | Measure-Object -Average).Average)
                p95_chunk_ms = Format-Number (Get-PercentileValue -Values $chunkElapsedMs.ToArray() -Percentile 95)
                total_throughput_mib_s = Format-Number ($fileMiB / $totalSeconds)
                upload_throughput_mib_s = Format-Number ($fileMiB / $uploadSeconds)
                complete_throughput_mib_s = Format-Number ($fileMiB / $completeSeconds)
            }

            $resultObject = [pscustomobject]$result
            $results += $resultObject
            ($resultObject | ConvertTo-Json -Compress) | Add-Content -LiteralPath $resolvedOutputPath -Encoding UTF8

            Write-Host ("  total={0}ms upload={1}ms complete={2}ms throughput={3}MiB/s" -f `
                $result.total_ms, `
                $result.upload_ms, `
                $result.complete_ms, `
                $result.total_throughput_mib_s)
        } finally {
            if ((-not $completed) -and (-not [string]::IsNullOrWhiteSpace($sessionId))) {
                try {
                    $abortUrl = New-ApiUrl "/api/uploads/abort" @{
                        session_id = $sessionId
                    }
                    Invoke-PostMeasured $abortUrl | Out-Null
                } catch {
                    Write-Warning "Failed to abort unfinished benchmark session $sessionId`: $($_.Exception.Message)"
                }
            }

            if ($completed -and (-not $KeepUploadedFiles)) {
                try {
                    $deleteUrl = New-ApiUrl "/api/files/delete" @{
                        filename = $filename
                    }
                    Invoke-PostMeasured $deleteUrl | Out-Null
                } catch {
                    Write-Warning "Failed to delete benchmark output $filename`: $($_.Exception.Message)"
                }
            }

            if (Test-Path -LiteralPath $localFile) {
                Remove-Item -LiteralPath $localFile -Force
            }

            if (Test-Path -LiteralPath $chunkTempFile) {
                Remove-Item -LiteralPath $chunkTempFile -Force
            }
        }
    }
}

Write-Host ""
if ($results.Count -gt 0) {
    $results |
        Select-Object file_size_mib, chunk_size_mib, chunk_count, total_ms, upload_ms, complete_ms, total_throughput_mib_s, avg_chunk_ms, p95_chunk_ms |
        Format-Table -AutoSize
}

Write-Host "Benchmark finished. Results appended to $resolvedOutputPath"
