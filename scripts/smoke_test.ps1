param(
    [string]$BaseUrl = "http://127.0.0.1:8080",
    [string]$Token = $env:PHOTO_BRIDGE_TOKEN,
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [switch]$SkipCompact
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

$suffix = Get-Date -Format "yyyyMMddHHmmss"
$chunkInitName = "smoke-chunk-init-$suffix.bin"
$deleteName = "smoke-delete-$suffix.txt"
$missingName = "smoke-missing-$suffix.txt"
$uploadDir = Join-Path $ProjectRoot "data\uploads"
$uploadTmpDir = Join-Path $ProjectRoot "data\uploads_tmp"

Write-Host "[1/14] health check"
$health = Invoke-GetText "$BaseUrl/health"
Assert-Contains $health "OK" "Health check failed."

$chunk0 = "hello "
$chunk1 = "chunk "
$chunk2 = "test"
$chunkExpectedContent = "$chunk0$chunk1$chunk2"

Write-Host "[2/14] init chunk upload session: $chunkInitName"
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

$chunkSessionDir = Join-Path $uploadTmpDir ("upload_" + $chunkSession.session_id)
$chunkSessionFile = Join-Path $chunkSessionDir "session.json"
$chunkSessionChunksDir = Join-Path $chunkSessionDir "chunks"

if (-not (Test-Path -LiteralPath $chunkSessionFile)) {
    throw "Chunk upload init did not create session.json at $chunkSessionFile. Make sure the server runs from ProjectRoot: $ProjectRoot"
}

if (-not (Test-Path -LiteralPath $chunkSessionChunksDir -PathType Container)) {
    throw "Chunk upload init did not create chunks directory at $chunkSessionChunksDir. Make sure the server runs from ProjectRoot: $ProjectRoot"
}

Assert-Contains (Get-Content -LiteralPath $chunkSessionFile -Raw) $chunkInitName "Chunk upload session.json does not include filename."

Write-Host "[3/14] upload chunk 0 and 2 for resume status flow"
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

if (-not (Test-Path -LiteralPath (Join-Path $chunkSessionChunksDir "chunk_0.part"))) {
    throw "Chunk 0 file was not created."
}

if (-not (Test-Path -LiteralPath (Join-Path $chunkSessionChunksDir "chunk_2.part"))) {
    throw "Chunk 2 file was not created."
}

Write-Host "[4/14] status should report missing chunk 1"
$chunkStatusUrl = New-ApiUrl "/api/uploads/status" @{
    session_id = $chunkSession.session_id
}
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

Write-Host "[5/14] upload missing chunk 1"
$chunkUploadUrl1 = New-ApiUrl "/api/uploads/chunk" @{
    session_id = $chunkSession.session_id
    index = 1
}
Assert-Contains (Invoke-PostText $chunkUploadUrl1 $chunk1) '"result":"success"' "Chunk 1 upload failed."

if (-not (Test-Path -LiteralPath (Join-Path $chunkSessionChunksDir "chunk_1.part"))) {
    throw "Chunk 1 file was not created."
}

Write-Host "[6/14] status should report no missing chunks"
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

Write-Host "[7/14] complete chunk upload"
$chunkCompleteUrl = New-ApiUrl "/api/uploads/complete" @{
    session_id = $chunkSession.session_id
}
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

Write-Host "[8/14] verify chunk upload is indexed and clean it through API"
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

if (Test-Path -LiteralPath $chunkSessionDir) {
    Remove-Item -LiteralPath $chunkSessionDir -Recurse -Force
}

Write-Host "[9/14] upload file for delete flow: $deleteName"
$uploadDeleteUrl = New-ApiUrl "/api/upload" @{ filename = $deleteName }
Invoke-PostText $uploadDeleteUrl "delete smoke payload" | Out-Null

Write-Host "[10/14] verify uploaded file is listed"
$files = Invoke-GetText (New-ApiUrl "/api/files")
Assert-Contains $files $deleteName "Uploaded delete test file was not returned by /api/files."

Write-Host "[11/14] delete file and verify it disappears from list"
$deleteUrl = New-ApiUrl "/api/files/delete" @{ filename = $deleteName }
$deleteResponse = Invoke-PostText $deleteUrl
Assert-Contains $deleteResponse "File deleted" "Delete endpoint did not return success."
$filesAfterDelete = Invoke-GetText (New-ApiUrl "/api/files")
Assert-NotContains $filesAfterDelete $deleteName "Deleted file is still visible in /api/files."

Write-Host "[12/14] upload file for MissingFile repair flow: $missingName"
$uploadMissingUrl = New-ApiUrl "/api/upload" @{ filename = $missingName }
Invoke-PostText $uploadMissingUrl "missing smoke payload" | Out-Null

Write-Host "[13/14] simulate MissingFile by removing physical upload"
$missingPath = Join-Path $uploadDir $missingName
if (-not (Test-Path -LiteralPath $missingPath)) {
    throw "Expected uploaded file not found at $missingPath. Make sure the server runs from ProjectRoot: $ProjectRoot"
}
Remove-Item -LiteralPath $missingPath -Force

$auditBeforeRepair = Invoke-GetText (New-ApiUrl "/api/metadata/audit")
Assert-Contains $auditBeforeRepair "MissingFile" "Audit did not report MissingFile after physical file removal."
Assert-Contains $auditBeforeRepair $missingName "Audit did not include the simulated missing file."

Write-Host "[14/14] run automatic repair and verify MissingFile is gone"
$repairResponse = Invoke-PostText (New-ApiUrl "/api/metadata/repair")
Assert-Contains $repairResponse "repaired_missing" "Repair endpoint did not return repair summary."
$auditAfterRepair = Invoke-GetText (New-ApiUrl "/api/metadata/audit")
Assert-NotContains $auditAfterRepair $missingName "Repaired missing file is still reported by audit."

if (-not $SkipCompact) {
    Write-Host "[cleanup] compact metadata log"
    $compactResponse = Invoke-PostText (New-ApiUrl "/api/metadata/compact")
    Assert-Contains $compactResponse "Metadata compacted" "Compact endpoint did not return success."
} else {
    Write-Host "[cleanup] compact skipped by -SkipCompact"
}

Write-Host ""
Write-Host "Smoke test passed."
