param(
    [string]$BaseUrl = $(if ($env:OWNGROVE_PORT) { "http://127.0.0.1:$($env:OWNGROVE_PORT)" } elseif ($env:PHOTO_BRIDGE_PORT) { "http://127.0.0.1:$($env:PHOTO_BRIDGE_PORT)" } else { "http://127.0.0.1:8787" }),
    [string]$Token,
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [switch]$SkipCompact
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Token)) {
    if (-not [string]::IsNullOrWhiteSpace($env:OWNGROVE_TOKEN)) {
        $Token = $env:OWNGROVE_TOKEN
    } elseif (-not [string]::IsNullOrWhiteSpace($env:PHOTO_BRIDGE_TOKEN)) {
        Write-Warning "PHOTO_BRIDGE_TOKEN is deprecated. Use OWNGROVE_TOKEN instead."
        $Token = $env:PHOTO_BRIDGE_TOKEN
    } else {
        throw "OWNGROVE_TOKEN is not set. Pass -Token or set `$env:OWNGROVE_TOKEN first."
    }
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

function Invoke-GetText {
    param([Parameter(Mandatory = $true)][string]$Url)
    return (Invoke-WebRequest -Uri $Url -Method Get -UseBasicParsing).Content
}

function Invoke-PostText {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [object]$Body = $null,
        [string]$ContentType = "text/plain"
    )

    if ($null -eq $Body) {
        return (Invoke-WebRequest -Uri $Url -Method Post -UseBasicParsing).Content
    }

    return (Invoke-WebRequest -Uri $Url -Method Post -Body $Body -ContentType $ContentType -UseBasicParsing).Content
}

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Expected,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Text.Contains($Expected)) {
        throw "$Message`nExpected to find: $Expected`nActual: $Text"
    }
}

function Assert-NotContains {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Unexpected,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ($Text.Contains($Unexpected)) {
        throw "$Message`nUnexpected value: $Unexpected`nActual: $Text"
    }
}

function Get-UploadObjectPaths {
    param([Parameter(Mandatory = $true)][string]$SessionId)

    $dataDir = Join-Path $ProjectRoot "data"
    if (-not (Test-Path -LiteralPath $dataDir -PathType Container)) {
        return @()
    }

    $needle = "uploads_tmp" + [System.IO.Path]::DirectorySeparatorChar + "upload_$SessionId" + [System.IO.Path]::DirectorySeparatorChar
    return @(
        Get-ChildItem -LiteralPath $dataDir -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName.IndexOf($needle, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 } |
            ForEach-Object { $_.FullName }
    )
}

function Get-UploadObjectPath {
    param(
        [Parameter(Mandatory = $true)][string]$SessionId,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    $matches = Get-UploadObjectReplicaPaths $SessionId $RelativePath

    if ($matches.Count -eq 0) {
        return $null
    }

    return $matches[0]
}

function Get-UploadObjectReplicaPaths {
    param(
        [Parameter(Mandatory = $true)][string]$SessionId,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    $dataDir = Join-Path $ProjectRoot "data"
    if (-not (Test-Path -LiteralPath $dataDir -PathType Container)) {
        return @()
    }

    $suffix = Join-Path ("uploads_tmp\upload_$SessionId") $RelativePath
    return @(
        Get-ChildItem -LiteralPath $dataDir -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName.EndsWith($suffix, [System.StringComparison]::OrdinalIgnoreCase) } |
            ForEach-Object { $_.FullName } |
            Sort-Object
    )
}

function Require-UploadObjectReplicas {
    param(
        [Parameter(Mandatory = $true)][string]$SessionId,
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [int]$MinimumCount = 1,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $paths = @(Get-UploadObjectReplicaPaths $SessionId $RelativePath)
    if ($paths.Count -lt $MinimumCount) {
        throw "$Message Expected at least $MinimumCount replica(s), actual: $($paths.Count)"
    }

    return $paths
}

function Require-UploadObjectPath {
    param(
        [Parameter(Mandatory = $true)][string]$SessionId,
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $path = Get-UploadObjectPath $SessionId $RelativePath
    if ([string]::IsNullOrWhiteSpace($path)) {
        throw $Message
    }

    return $path
}

function Get-StorageNodes {
    return @((Invoke-GetText (New-ApiUrl "/api/storage/nodes")) | ConvertFrom-Json)
}

function Get-StorageNode {
    param([Parameter(Mandatory = $true)][string]$NodeId)

    $matches = @(Get-StorageNodes | Where-Object { $_.id -eq $NodeId })
    if ($matches.Count -ne 1) {
        throw "Expected exactly one storage node with id=$NodeId. Actual matches: $($matches.Count)"
    }

    return $matches[0]
}

function Set-StorageNodeAvailability {
    param(
        [Parameter(Mandatory = $true)][string]$NodeId,
        [Parameter(Mandatory = $true)][bool]$Available
    )

    $availableText = "false"
    if ($Available) {
        $availableText = "true"
    }

    $url = New-ApiUrl "/api/storage/node/availability" @{
        node_id = $NodeId
        available = $availableText
    }

    $response = Invoke-PostText $url
    Assert-Contains $response '"result":"success"' "Setting storage node availability failed."
}

function Assert-StorageNodeAvailability {
    param(
        [Parameter(Mandatory = $true)][string]$NodeId,
        [Parameter(Mandatory = $true)][bool]$Expected
    )

    $node = Get-StorageNode $NodeId
    if ([bool]$node.available -ne $Expected) {
        throw "Storage node $NodeId returned unexpected availability. Expected: $Expected. Actual: $($node.available)"
    }
}

function Get-NodeIdFromObjectPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ($Path -like "*\shard_0\*") { return "node-0" }
    if ($Path -like "*\shard_1\*") { return "node-1" }
    if ($Path -like "*\shard_2\*") { return "node-2" }

    throw "Cannot infer storage node id from object path: $Path"
}

function Get-PlacementNodes {
    param([Parameter(Mandatory = $true)][string]$Key)

    $placementUrl = New-ApiUrl "/api/storage/placement" @{
        key = $Key
    }
    $placement = Invoke-GetText $placementUrl | ConvertFrom-Json
    return @($placement.nodes)
}

function Get-ReplicaAudit {
    param([Parameter(Mandatory = $true)][string]$Key)

    $auditUrl = New-ApiUrl "/api/storage/replicas/audit" @{
        key = $Key
    }
    return (Invoke-GetText $auditUrl | ConvertFrom-Json)
}

$suffix = Get-Date -Format "yyyyMMddHHmmss"
$abortChunkName = "smoke-chunk-abort-$suffix.bin"
$chunkInitName = "smoke-chunk-init-$suffix.bin"
$fallbackChunkName = "smoke-node-fallback-$suffix.bin"
$quorumChunkName = "smoke-node-quorum-$suffix.bin"
$repairChunkName = "smoke-replica-repair-$suffix.bin"
$deleteName = "smoke-delete-$suffix.txt"
$missingName = "smoke-missing-$suffix.txt"
$uploadDir = Join-Path $ProjectRoot "data\uploads"

Write-Host "[1/26] health check"
$health = Invoke-GetText "$BaseUrl/health"
Assert-Contains $health "OK" "Health check failed."

Write-Host "[node] verify storage node availability API"
Set-StorageNodeAvailability -NodeId "node-0" -Available $true
foreach ($nodeId in @("node-0", "node-1", "node-2")) {
    Get-StorageNode $nodeId | Out-Null
}
Assert-StorageNodeAvailability -NodeId "node-0" -Expected $true

Write-Host "[node] mark node-0 unavailable"
Set-StorageNodeAvailability -NodeId "node-0" -Available $false
Assert-StorageNodeAvailability -NodeId "node-0" -Expected $false

Write-Host "[node] restore node-0 availability"
Set-StorageNodeAvailability -NodeId "node-0" -Available $true
Assert-StorageNodeAvailability -NodeId "node-0" -Expected $true

Write-Host "[node] verify replica read fallback when one node is unavailable"
$fallbackChunk = "node!!"
$fallbackInitUrl = New-ApiUrl "/api/uploads/init" @{
    filename = $fallbackChunkName
    size = $fallbackChunk.Length
    chunk_size = $fallbackChunk.Length
}
$fallbackInitResponse = Invoke-PostText $fallbackInitUrl
$fallbackSession = $fallbackInitResponse | ConvertFrom-Json

if ([string]::IsNullOrWhiteSpace($fallbackSession.session_id)) {
    throw "Fallback upload init did not return session_id. Actual: $fallbackInitResponse"
}

$fallbackChunkUrl = New-ApiUrl "/api/uploads/chunk" @{
    session_id = $fallbackSession.session_id
    index = 0
}
Assert-Contains (Invoke-PostText $fallbackChunkUrl $fallbackChunk) '"result":"success"' "Fallback test chunk upload failed."

$fallbackPartPaths = @(Require-UploadObjectReplicas -SessionId $fallbackSession.session_id -RelativePath "chunks\chunk_0.part" -MinimumCount 2 -Message "Fallback test chunk was not replicated.")
$fallbackDownNode = Get-NodeIdFromObjectPath $fallbackPartPaths[0]

Set-StorageNodeAvailability -NodeId $fallbackDownNode -Available $false
try {
    $fallbackStatusUrl = New-ApiUrl "/api/uploads/status" @{
        session_id = $fallbackSession.session_id
    }
    $fallbackStatus = Invoke-GetText $fallbackStatusUrl | ConvertFrom-Json

    if ($fallbackStatus.uploaded_count -ne 1 -or $fallbackStatus.missing_count -ne 0) {
        throw "Replica read fallback failed when $fallbackDownNode is unavailable. Uploaded: $($fallbackStatus.uploaded_count), missing: $($fallbackStatus.missing_count)"
    }
}
finally {
    Set-StorageNodeAvailability -NodeId $fallbackDownNode -Available $true
}

$fallbackAbortUrl = New-ApiUrl "/api/uploads/abort" @{
    session_id = $fallbackSession.session_id
}
Assert-Contains (Invoke-PostText $fallbackAbortUrl) '"result":"success"' "Fallback test cleanup failed."

Write-Host "[node] verify write quorum fails when a placement node is unavailable"
$quorumChunk = "quorum"
$quorumInitUrl = New-ApiUrl "/api/uploads/init" @{
    filename = $quorumChunkName
    size = $quorumChunk.Length
    chunk_size = $quorumChunk.Length
}
$quorumInitResponse = Invoke-PostText $quorumInitUrl
$quorumSession = $quorumInitResponse | ConvertFrom-Json

if ([string]::IsNullOrWhiteSpace($quorumSession.session_id)) {
    throw "Quorum upload init did not return session_id. Actual: $quorumInitResponse"
}

$quorumChunkKey = "uploads_tmp/upload_$($quorumSession.session_id)/chunks/chunk_0.part"
$quorumPlacementNodes = @(Get-PlacementNodes $quorumChunkKey)
if ($quorumPlacementNodes.Count -lt 2) {
    throw "Placement endpoint returned fewer than 2 nodes for quorum test key. Actual: $($quorumPlacementNodes -join ',')"
}

$quorumDownNode = $quorumPlacementNodes[0]
$quorumChunkUrl = New-ApiUrl "/api/uploads/chunk" @{
    session_id = $quorumSession.session_id
    index = 0
}

Set-StorageNodeAvailability -NodeId $quorumDownNode -Available $false
try {
    try {
        $quorumUploadResponse = Invoke-WebRequest `
            -Uri $quorumChunkUrl `
            -Method Post `
            -Body $quorumChunk `
            -ContentType "text/plain" `
            -UseBasicParsing

        throw "Expected quorum-protected chunk upload to fail while $quorumDownNode is unavailable, but request succeeded. Response: $($quorumUploadResponse.Content)"
    } catch {
        if ($null -eq $_.Exception.Response) {
            throw "Expected 500 quorum failure while $quorumDownNode is unavailable. Actual: $($_.Exception.Message)"
        }

        if ($_.Exception.Response.StatusCode.value__ -ne 500) {
            throw "Expected 500 quorum failure while $quorumDownNode is unavailable. Actual: $($_.Exception.Response.StatusCode.value__)"
        }
    }
}
finally {
    Set-StorageNodeAvailability -NodeId $quorumDownNode -Available $true
}

$quorumAbortUrl = New-ApiUrl "/api/uploads/abort" @{
    session_id = $quorumSession.session_id
}
Assert-Contains (Invoke-PostText $quorumAbortUrl) '"result":"success"' "Quorum test cleanup failed."

Write-Host "[node] verify replica audit and repair for a missing object replica"
$repairChunk = "repair"
$repairInitUrl = New-ApiUrl "/api/uploads/init" @{
    filename = $repairChunkName
    size = $repairChunk.Length
    chunk_size = $repairChunk.Length
}
$repairInitResponse = Invoke-PostText $repairInitUrl
$repairSession = $repairInitResponse | ConvertFrom-Json

if ([string]::IsNullOrWhiteSpace($repairSession.session_id)) {
    throw "Replica repair upload init did not return session_id. Actual: $repairInitResponse"
}

$repairChunkUrl = New-ApiUrl "/api/uploads/chunk" @{
    session_id = $repairSession.session_id
    index = 0
}
Assert-Contains (Invoke-PostText $repairChunkUrl $repairChunk) '"result":"success"' "Replica repair test chunk upload failed."

$repairKey = "uploads_tmp/upload_$($repairSession.session_id)/chunks/chunk_0.part"
$repairPartPaths = @(Require-UploadObjectReplicas -SessionId $repairSession.session_id -RelativePath "chunks\chunk_0.part" -MinimumCount 2 -Message "Replica repair test chunk was not replicated.")

$repairAuditBeforeDelete = Get-ReplicaAudit $repairKey
if (@($repairAuditBeforeDelete.replicas).Count -ne 2) {
    throw "Replica audit returned unexpected replica count before delete. Expected: 2. Actual: $(@($repairAuditBeforeDelete.replicas).Count)"
}
foreach ($replica in @($repairAuditBeforeDelete.replicas)) {
    if (-not [bool]$replica.exists -or [int64]$replica.size -ne $repairChunk.Length) {
        throw "Replica audit before delete returned unexpected item. Actual: $($repairAuditBeforeDelete | ConvertTo-Json -Compress)"
    }
}

Remove-Item -LiteralPath $repairPartPaths[0] -Force

$repairAuditAfterDelete = Get-ReplicaAudit $repairKey
$missingReplicas = @($repairAuditAfterDelete.replicas | Where-Object { -not [bool]$_.exists })
if ($missingReplicas.Count -ne 1) {
    throw "Replica audit did not report exactly one missing replica after delete. Actual: $($repairAuditAfterDelete | ConvertTo-Json -Compress)"
}

$repairUrl = New-ApiUrl "/api/storage/replicas/repair" @{
    key = $repairKey
}
$repairResponse = Invoke-PostText $repairUrl | ConvertFrom-Json
if ($repairResponse.result -ne "success" -or -not [bool]$repairResponse.repaired -or [int64]$repairResponse.repaired_count -ne 1) {
    throw "Replica repair endpoint returned unexpected response. Actual: $($repairResponse | ConvertTo-Json -Compress)"
}

$repairAuditAfterRepair = Get-ReplicaAudit $repairKey
foreach ($replica in @($repairAuditAfterRepair.replicas)) {
    if (-not [bool]$replica.exists -or [int64]$replica.size -ne $repairChunk.Length) {
        throw "Replica audit after repair did not show all replicas restored. Actual: $($repairAuditAfterRepair | ConvertTo-Json -Compress)"
    }
}

$repairAbortUrl = New-ApiUrl "/api/uploads/abort" @{
    session_id = $repairSession.session_id
}
Assert-Contains (Invoke-PostText $repairAbortUrl) '"result":"success"' "Replica repair test cleanup failed."

$abortChunk = "abort "
$chunk0 = "hello "
$chunk1 = "chunk "
$chunk2 = "test"
$chunkExpectedContent = "$chunk0$chunk1$chunk2"

Write-Host "[2/26] init abort upload session: $abortChunkName"
$abortInitUrl = New-ApiUrl "/api/uploads/init" @{
    filename = $abortChunkName
    size = $abortChunk.Length
    chunk_size = $abortChunk.Length
}
$abortInitResponse = Invoke-PostText $abortInitUrl
$abortSession = $abortInitResponse | ConvertFrom-Json

if ([string]::IsNullOrWhiteSpace($abortSession.session_id)) {
    throw "Abort upload init did not return session_id. Actual: $abortInitResponse"
}

$abortUploadUrl0 = New-ApiUrl "/api/uploads/chunk" @{
    session_id = $abortSession.session_id
    index = 0
}

Write-Host "[3/26] upload one chunk before abort"
Assert-Contains (Invoke-PostText $abortUploadUrl0 $abortChunk) '"result":"success"' "Abort test chunk upload failed."
Require-UploadObjectReplicas -SessionId $abortSession.session_id -RelativePath "session.json" -MinimumCount 2 -Message "Abort test session object was not replicated." | Out-Null
Require-UploadObjectReplicas -SessionId $abortSession.session_id -RelativePath "chunks\chunk_0.part" -MinimumCount 2 -Message "Abort test chunk object was not replicated." | Out-Null
Require-UploadObjectReplicas -SessionId $abortSession.session_id -RelativePath "chunks\chunk_0.meta" -MinimumCount 2 -Message "Abort test chunk meta object was not replicated." | Out-Null

Write-Host "[4/26] abort upload session"
$abortUrl = New-ApiUrl "/api/uploads/abort" @{
    session_id = $abortSession.session_id
}
$abortResponse = Invoke-PostText $abortUrl
Assert-Contains $abortResponse '"result":"success"' "Abort endpoint did not return success."

if (@(Get-UploadObjectPaths $abortSession.session_id).Count -ne 0) {
    throw "Abort endpoint did not remove all upload objects for session: $($abortSession.session_id)"
}

Write-Host "[5/26] aborted session should be unavailable"
$abortStatusUrl = New-ApiUrl "/api/uploads/status" @{
    session_id = $abortSession.session_id
}
try {
    $abortStatusResponse = Invoke-WebRequest `
        -Uri $abortStatusUrl `
        -Method Get `
        -UseBasicParsing

    throw "Expected aborted session status to fail, but request succeeded. Response: $($abortStatusResponse.Content)"
} catch {
    if ($null -eq $_.Exception.Response) {
        throw "Expected 400 status response for aborted session. Actual: $($_.Exception.Message)"
    }

    if ($_.Exception.Response.StatusCode.value__ -ne 400) {
        throw "Expected 400 status response for aborted session. Actual: $($_.Exception.Response.StatusCode.value__)"
    }
}

Write-Host "[6/26] init chunk upload session: $chunkInitName"
$chunkInitUrl = New-ApiUrl "/api/uploads/init" @{
    filename = $chunkInitName
    size = $chunkExpectedContent.Length
    chunk_size = 6
}
$chunkInitResponse = Invoke-PostText $chunkInitUrl
$chunkSession = $chunkInitResponse | ConvertFrom-Json

if ([string]::IsNullOrWhiteSpace($chunkSession.session_id)) {
    throw "Chunk upload init did not return session_id. Actual: $chunkInitResponse"
}

if ($chunkSession.chunk_count -ne 3) {
    throw "Chunk upload init returned unexpected chunk_count. Expected: 3. Actual: $($chunkSession.chunk_count). Response: $chunkInitResponse"
}

if ($chunkSession.status -ne "pending") {
    throw "Chunk upload init returned unexpected status. Expected: pending. Actual: $($chunkSession.status). Response: $chunkInitResponse"
}

$chunkSessionFiles = @(Require-UploadObjectReplicas -SessionId $chunkSession.session_id -RelativePath "session.json" -MinimumCount 2 -Message "Chunk upload init did not replicate session object. Make sure the server runs from ProjectRoot: $ProjectRoot")
$chunkSessionFile = $chunkSessionFiles[0]

Assert-Contains (Get-Content -LiteralPath $chunkSessionFile -Raw) $chunkInitName "Chunk upload session.json does not include filename."

Write-Host "[7/26] upload chunk 0 and 2 for resume status flow"
$chunkUploadUrl0 = New-ApiUrl "/api/uploads/chunk" @{
    session_id = $chunkSession.session_id
    index = 0
}
$chunkUploadUrl2 = New-ApiUrl "/api/uploads/chunk" @{
    session_id = $chunkSession.session_id
    index = 2
}

Assert-Contains (Invoke-PostText $chunkUploadUrl0 $chunk0) '"result":"success"' "Chunk 0 upload failed."
Assert-Contains (Invoke-PostText $chunkUploadUrl2 $chunk2) '"result":"success"' "Chunk 2 upload failed."

$chunk0PartPaths = @(Require-UploadObjectReplicas -SessionId $chunkSession.session_id -RelativePath "chunks\chunk_0.part" -MinimumCount 2 -Message "Chunk 0 file was not replicated.")
$chunk0MetaPaths = @(Require-UploadObjectReplicas -SessionId $chunkSession.session_id -RelativePath "chunks\chunk_0.meta" -MinimumCount 2 -Message "Chunk 0 meta file was not replicated.")
$chunk2PartPaths = @(Require-UploadObjectReplicas -SessionId $chunkSession.session_id -RelativePath "chunks\chunk_2.part" -MinimumCount 2 -Message "Chunk 2 file was not replicated.")
$chunk2MetaPaths = @(Require-UploadObjectReplicas -SessionId $chunkSession.session_id -RelativePath "chunks\chunk_2.meta" -MinimumCount 2 -Message "Chunk 2 meta file was not replicated.")
$chunk0PartPath = $chunk0PartPaths[0]
$chunk0MetaPath = $chunk0MetaPaths[0]
$chunk2PartPath = $chunk2PartPaths[0]
$chunk2MetaPath = $chunk2MetaPaths[0]

$chunkStatusUrl = New-ApiUrl "/api/uploads/status" @{
    session_id = $chunkSession.session_id
}

Remove-Item -LiteralPath $chunk0PartPaths[0] -Force
$chunkStatusAfterSingleReplicaDelete = Invoke-GetText $chunkStatusUrl | ConvertFrom-Json

if ($chunkStatusAfterSingleReplicaDelete.uploaded_count -ne 2 -or $chunkStatusAfterSingleReplicaDelete.missing_count -ne 1) {
    throw "Chunk status did not fall back to the remaining replica after deleting one chunk 0 part replica. Uploaded: $($chunkStatusAfterSingleReplicaDelete.uploaded_count), missing: $($chunkStatusAfterSingleReplicaDelete.missing_count)"
}

Write-Host "[8/26] same-size different chunk content should conflict"
try {
    $conflictResponse = Invoke-WebRequest `
        -Uri $chunkUploadUrl0 `
        -Method Post `
        -Body "HELLO " `
        -ContentType "text/plain" `
        -UseBasicParsing

    throw "Expected chunk conflict, but request succeeded. Response: $($conflictResponse.Content)"
} catch {
    if ($null -eq $_.Exception.Response) {
        throw "Expected 409 conflict for same-size different chunk content. Actual: $($_.Exception.Message)"
    }

    if ($_.Exception.Response.StatusCode.value__ -ne 409) {
        throw "Expected 409 conflict for same-size different chunk content. Actual: $($_.Exception.Response.StatusCode.value__)"
    }
}

Write-Host "[9/26] corrupt chunk 0 meta checksum should be treated as missing"
$chunk0MetaPaths = @(Require-UploadObjectReplicas -SessionId $chunkSession.session_id -RelativePath "chunks\chunk_0.meta" -MinimumCount 2 -Message "Chunk 0 meta replicas were not found before corruption.")
foreach ($path in $chunk0MetaPaths) {
    $chunk0MetaText = Get-Content -LiteralPath $path -Raw
    $chunk0CorruptMetaText = $chunk0MetaText -replace '"checksum":"[^"]+"', '"checksum":"deadbeef"'
    [System.IO.File]::WriteAllText($path, $chunk0CorruptMetaText, [System.Text.Encoding]::UTF8)
}

$chunkStatusAfterMetaCorrupt = Invoke-GetText $chunkStatusUrl | ConvertFrom-Json

if ($chunkStatusAfterMetaCorrupt.uploaded_count -ne 1) {
    throw "Chunk status returned unexpected uploaded_count after corrupting meta. Expected: 1. Actual: $($chunkStatusAfterMetaCorrupt.uploaded_count)"
}

if ($chunkStatusAfterMetaCorrupt.missing_count -ne 2) {
    throw "Chunk status returned unexpected missing_count after corrupting meta. Expected: 2. Actual: $($chunkStatusAfterMetaCorrupt.missing_count)"
}

if (($chunkStatusAfterMetaCorrupt.uploaded_indexes -join ",") -ne "2") {
    throw "Chunk status returned unexpected uploaded_indexes after corrupting meta. Expected: 2. Actual: $($chunkStatusAfterMetaCorrupt.uploaded_indexes -join ',')"
}

if (($chunkStatusAfterMetaCorrupt.missing_indexes -join ",") -ne "0,1") {
    throw "Chunk status returned unexpected missing_indexes after corrupting meta. Expected: 0,1. Actual: $($chunkStatusAfterMetaCorrupt.missing_indexes -join ',')"
}

Write-Host "[10/26] re-upload chunk 0 should repair corrupt meta"
Assert-Contains (Invoke-PostText $chunkUploadUrl0 $chunk0) '"result":"success"' "Re-uploading chunk 0 should repair corrupt meta."

Write-Host "[11/26] missing meta should make chunk 0 incomplete"
$chunk0MetaPaths = @(Require-UploadObjectReplicas -SessionId $chunkSession.session_id -RelativePath "chunks\chunk_0.meta" -MinimumCount 2 -Message "Chunk 0 meta replicas were not rebuilt before delete test.")
foreach ($path in $chunk0MetaPaths) {
    Remove-Item -LiteralPath $path -Force
}
$chunkStatusAfterMetaDelete = Invoke-GetText $chunkStatusUrl | ConvertFrom-Json

if ($chunkStatusAfterMetaDelete.uploaded_count -ne 1) {
    throw "Chunk status returned unexpected uploaded_count after deleting meta. Expected: 1. Actual: $($chunkStatusAfterMetaDelete.uploaded_count)"
}

if ($chunkStatusAfterMetaDelete.missing_count -ne 2) {
    throw "Chunk status returned unexpected missing_count after deleting meta. Expected: 2. Actual: $($chunkStatusAfterMetaDelete.missing_count)"
}

if (($chunkStatusAfterMetaDelete.uploaded_indexes -join ",") -ne "2") {
    throw "Chunk status returned unexpected uploaded_indexes after deleting meta. Expected: 2. Actual: $($chunkStatusAfterMetaDelete.uploaded_indexes -join ',')"
}

if (($chunkStatusAfterMetaDelete.missing_indexes -join ",") -ne "0,1") {
    throw "Chunk status returned unexpected missing_indexes after deleting meta. Expected: 0,1. Actual: $($chunkStatusAfterMetaDelete.missing_indexes -join ',')"
}

Write-Host "[12/26] re-upload chunk 0 should rebuild missing meta"
Assert-Contains (Invoke-PostText $chunkUploadUrl0 $chunk0) '"result":"success"' "Re-uploading chunk 0 should rebuild missing meta."
$chunk0MetaPaths = @(Require-UploadObjectReplicas -SessionId $chunkSession.session_id -RelativePath "chunks\chunk_0.meta" -MinimumCount 2 -Message "Chunk 0 meta replicas were not rebuilt by idempotent re-upload.")
$chunk0MetaPath = $chunk0MetaPaths[0]

Write-Host "[13/26] corrupt chunk 0 part should be treated as missing and fail complete"
$chunk0PartPaths = @(Require-UploadObjectReplicas -SessionId $chunkSession.session_id -RelativePath "chunks\chunk_0.part" -MinimumCount 1 -Message "Chunk 0 part was not found before corruption.")
foreach ($path in $chunk0PartPaths) {
    [System.IO.File]::WriteAllText($path, "HELLO ", [System.Text.Encoding]::ASCII)
}
$chunkStatusAfterPartCorrupt = Invoke-GetText $chunkStatusUrl | ConvertFrom-Json

if ($chunkStatusAfterPartCorrupt.uploaded_count -ne 1) {
    throw "Chunk status returned unexpected uploaded_count after corrupting part. Expected: 1. Actual: $($chunkStatusAfterPartCorrupt.uploaded_count)"
}

if ($chunkStatusAfterPartCorrupt.missing_count -ne 2) {
    throw "Chunk status returned unexpected missing_count after corrupting part. Expected: 2. Actual: $($chunkStatusAfterPartCorrupt.missing_count)"
}

if (($chunkStatusAfterPartCorrupt.uploaded_indexes -join ",") -ne "2") {
    throw "Chunk status returned unexpected uploaded_indexes after corrupting part. Expected: 2. Actual: $($chunkStatusAfterPartCorrupt.uploaded_indexes -join ',')"
}

if (($chunkStatusAfterPartCorrupt.missing_indexes -join ",") -ne "0,1") {
    throw "Chunk status returned unexpected missing_indexes after corrupting part. Expected: 0,1. Actual: $($chunkStatusAfterPartCorrupt.missing_indexes -join ',')"
}

$chunkCompleteUrl = New-ApiUrl "/api/uploads/complete" @{
    session_id = $chunkSession.session_id
}
try {
    $corruptCompleteResponse = Invoke-WebRequest `
        -Uri $chunkCompleteUrl `
        -Method Post `
        -UseBasicParsing

    throw "Expected chunk complete to fail for corrupt part, but request succeeded. Response: $($corruptCompleteResponse.Content)"
} catch {
    if ($null -eq $_.Exception.Response) {
        throw "Expected complete failure for corrupt part. Actual: $($_.Exception.Message)"
    }

    if ($_.Exception.Response.StatusCode.value__ -ne 500) {
        throw "Expected 500 complete failure for corrupt part. Actual: $($_.Exception.Response.StatusCode.value__)"
    }
}

Write-Host "[14/26] remove corrupt chunk 0 and re-upload clean chunk"
$chunk0PartPaths = @(Get-UploadObjectReplicaPaths $chunkSession.session_id "chunks\chunk_0.part")
$chunk0MetaPaths = @(Get-UploadObjectReplicaPaths $chunkSession.session_id "chunks\chunk_0.meta")
foreach ($path in $chunk0PartPaths + $chunk0MetaPaths) {
    Remove-Item -LiteralPath $path -Force
}
Assert-Contains (Invoke-PostText $chunkUploadUrl0 $chunk0) '"result":"success"' "Re-uploading chunk 0 after corrupt part cleanup failed."
$chunk0PartPaths = @(Require-UploadObjectReplicas -SessionId $chunkSession.session_id -RelativePath "chunks\chunk_0.part" -MinimumCount 2 -Message "Chunk 0 replicas were not rebuilt after corrupt part cleanup.")
$chunk0MetaPaths = @(Require-UploadObjectReplicas -SessionId $chunkSession.session_id -RelativePath "chunks\chunk_0.meta" -MinimumCount 2 -Message "Chunk 0 meta replicas were not rebuilt after corrupt part cleanup.")
$chunk0PartPath = $chunk0PartPaths[0]
$chunk0MetaPath = $chunk0MetaPaths[0]

Write-Host "[15/26] status should report missing chunk 1"
$chunkStatusBeforeRepair = Invoke-GetText $chunkStatusUrl | ConvertFrom-Json

if ($chunkStatusBeforeRepair.uploaded_count -ne 2) {
    throw "Chunk status returned unexpected uploaded_count. Expected: 2. Actual: $($chunkStatusBeforeRepair.uploaded_count)"
}

if ($chunkStatusBeforeRepair.missing_count -ne 1) {
    throw "Chunk status returned unexpected missing_count. Expected: 1. Actual: $($chunkStatusBeforeRepair.missing_count)"
}

if (($chunkStatusBeforeRepair.uploaded_indexes -join ",") -ne "0,2") {
    throw "Chunk status returned unexpected uploaded_indexes. Expected: 0,2. Actual: $($chunkStatusBeforeRepair.uploaded_indexes -join ',')"
}

if (($chunkStatusBeforeRepair.missing_indexes -join ",") -ne "1") {
    throw "Chunk status returned unexpected missing_indexes. Expected: 1. Actual: $($chunkStatusBeforeRepair.missing_indexes -join ',')"
}

Write-Host "[16/26] upload missing chunk 1"
$chunkUploadUrl1 = New-ApiUrl "/api/uploads/chunk" @{
    session_id = $chunkSession.session_id
    index = 1
}
Assert-Contains (Invoke-PostText $chunkUploadUrl1 $chunk1) '"result":"success"' "Chunk 1 upload failed."
$chunk1PartPaths = @(Require-UploadObjectReplicas -SessionId $chunkSession.session_id -RelativePath "chunks\chunk_1.part" -MinimumCount 2 -Message "Chunk 1 file was not replicated.")
$chunk1MetaPaths = @(Require-UploadObjectReplicas -SessionId $chunkSession.session_id -RelativePath "chunks\chunk_1.meta" -MinimumCount 2 -Message "Chunk 1 meta file was not replicated.")
$chunk1PartPath = $chunk1PartPaths[0]
$chunk1MetaPath = $chunk1MetaPaths[0]

Write-Host "[17/26] status should report no missing chunks"
$chunkStatusAfterRepair = Invoke-GetText $chunkStatusUrl | ConvertFrom-Json

if ($chunkStatusAfterRepair.uploaded_count -ne 3) {
    throw "Chunk status returned unexpected uploaded_count after resume. Expected: 3. Actual: $($chunkStatusAfterRepair.uploaded_count)"
}

if ($chunkStatusAfterRepair.missing_count -ne 0) {
    throw "Chunk status returned unexpected missing_count after resume. Expected: 0. Actual: $($chunkStatusAfterRepair.missing_count)"
}

if (($chunkStatusAfterRepair.uploaded_indexes -join ",") -ne "0,1,2") {
    throw "Chunk status returned unexpected uploaded_indexes after resume. Expected: 0,1,2. Actual: $($chunkStatusAfterRepair.uploaded_indexes -join ',')"
}

Write-Host "[18/26] complete chunk upload"
$chunkCompleteResponse = Invoke-PostText $chunkCompleteUrl
$chunkComplete = $chunkCompleteResponse | ConvertFrom-Json

if ($chunkComplete.result -ne "success") {
    throw "Chunk upload complete returned unexpected result. Actual: $chunkCompleteResponse"
}

if ($chunkComplete.filename -ne $chunkInitName) {
    throw "Chunk upload complete returned unexpected filename. Expected: $chunkInitName. Actual: $($chunkComplete.filename)"
}

if ($chunkComplete.size -ne $chunkExpectedContent.Length) {
    throw "Chunk upload complete returned unexpected size. Expected: $($chunkExpectedContent.Length). Actual: $($chunkComplete.size)"
}

$chunkOutputPath = Join-Path $uploadDir $chunkInitName
if (-not (Test-Path -LiteralPath $chunkOutputPath)) {
    throw "Chunk upload complete did not create final file at $chunkOutputPath"
}

$chunkActualContent = Get-Content -LiteralPath $chunkOutputPath -Raw
if ($chunkActualContent -ne $chunkExpectedContent) {
    throw "Chunk upload merged content mismatch. Expected: $chunkExpectedContent Actual: $chunkActualContent"
}

Write-Host "[19/26] verify chunk upload is indexed and clean it through API"
$filesAfterChunkComplete = Invoke-GetText (New-ApiUrl "/api/files")
Assert-Contains $filesAfterChunkComplete $chunkInitName "Chunk-completed file was not returned by /api/files."

$deleteChunkUrl = New-ApiUrl "/api/files/delete" @{ filename = $chunkInitName }
$deleteChunkResponse = Invoke-PostText $deleteChunkUrl
Assert-Contains $deleteChunkResponse "File deleted" "Delete endpoint did not clean chunk-completed file."

$filesAfterChunkDelete = Invoke-GetText (New-ApiUrl "/api/files")
Assert-NotContains $filesAfterChunkDelete $chunkInitName "Chunk-completed file is still visible in /api/files after delete."

if (Test-Path -LiteralPath $chunkOutputPath) {
    throw "Chunk-completed physical file still exists after delete endpoint: $chunkOutputPath"
}

if (@(Get-UploadObjectPaths $chunkSession.session_id).Count -ne 0) {
    throw "Chunk upload temporary objects still exist after successful complete for session: $($chunkSession.session_id)"
}

Write-Host "[20/26] upload file for delete flow: $deleteName"
$uploadDeleteUrl = New-ApiUrl "/api/upload" @{ filename = $deleteName }
Invoke-PostText $uploadDeleteUrl "delete smoke payload" | Out-Null

Write-Host "[21/26] verify uploaded file is listed"
$files = Invoke-GetText (New-ApiUrl "/api/files")
Assert-Contains $files $deleteName "Uploaded delete test file was not returned by /api/files."

Write-Host "[22/26] delete file and verify it disappears from list"
$deleteUrl = New-ApiUrl "/api/files/delete" @{ filename = $deleteName }
$deleteResponse = Invoke-PostText $deleteUrl
Assert-Contains $deleteResponse "File deleted" "Delete endpoint did not return success."
$filesAfterDelete = Invoke-GetText (New-ApiUrl "/api/files")
Assert-NotContains $filesAfterDelete $deleteName "Deleted file is still visible in /api/files."

Write-Host "[23/26] upload file for MissingFile repair flow: $missingName"
$uploadMissingUrl = New-ApiUrl "/api/upload" @{ filename = $missingName }
Invoke-PostText $uploadMissingUrl "missing smoke payload" | Out-Null

Write-Host "[24/26] simulate MissingFile by removing physical upload"
$missingPath = Join-Path $uploadDir $missingName
if (-not (Test-Path -LiteralPath $missingPath)) {
    throw "Expected uploaded file not found at $missingPath. Make sure the server runs from ProjectRoot: $ProjectRoot"
}
Remove-Item -LiteralPath $missingPath -Force

$auditBeforeRepair = Invoke-GetText (New-ApiUrl "/api/metadata/audit")
Assert-Contains $auditBeforeRepair "MissingFile" "Audit did not report MissingFile after physical file removal."
Assert-Contains $auditBeforeRepair $missingName "Audit did not include the simulated missing file."

Write-Host "[25/26] run automatic repair and verify MissingFile is gone"
$repairResponse = Invoke-PostText (New-ApiUrl "/api/metadata/repair")
Assert-Contains $repairResponse "repaired_missing" "Repair endpoint did not return repair summary."
$auditAfterRepair = Invoke-GetText (New-ApiUrl "/api/metadata/audit")
Assert-NotContains $auditAfterRepair $missingName "Repaired missing file is still reported by audit."

if (-not $SkipCompact) {
    Write-Host "[26/26] compact metadata log"
    $compactResponse = Invoke-PostText (New-ApiUrl "/api/metadata/compact")
    Assert-Contains $compactResponse "Metadata compacted" "Compact endpoint did not return success."
} else {
    Write-Host "[26/26] compact skipped by -SkipCompact"
}

Write-Host ""
Write-Host "Smoke test passed."
