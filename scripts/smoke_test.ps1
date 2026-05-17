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
$deleteName = "smoke-delete-$suffix.txt"
$missingName = "smoke-missing-$suffix.txt"
$uploadDir = Join-Path $ProjectRoot "data\uploads"

Write-Host "[1/8] health check"
$health = Invoke-GetText "$BaseUrl/health"
Assert-Contains $health "OK" "Health check failed."

Write-Host "[2/8] upload file for delete flow: $deleteName"
$uploadDeleteUrl = New-ApiUrl "/api/upload" @{ filename = $deleteName }
Invoke-PostText $uploadDeleteUrl "delete smoke payload" | Out-Null

Write-Host "[3/8] verify uploaded file is listed"
$files = Invoke-GetText (New-ApiUrl "/api/files")
Assert-Contains $files $deleteName "Uploaded delete test file was not returned by /api/files."

Write-Host "[4/8] delete file and verify it disappears from list"
$deleteUrl = New-ApiUrl "/api/files/delete" @{ filename = $deleteName }
$deleteResponse = Invoke-PostText $deleteUrl
Assert-Contains $deleteResponse "File deleted" "Delete endpoint did not return success."
$filesAfterDelete = Invoke-GetText (New-ApiUrl "/api/files")
Assert-NotContains $filesAfterDelete $deleteName "Deleted file is still visible in /api/files."

Write-Host "[5/8] upload file for MissingFile repair flow: $missingName"
$uploadMissingUrl = New-ApiUrl "/api/upload" @{ filename = $missingName }
Invoke-PostText $uploadMissingUrl "missing smoke payload" | Out-Null

Write-Host "[6/8] simulate MissingFile by removing physical upload"
$missingPath = Join-Path $uploadDir $missingName
if (-not (Test-Path -LiteralPath $missingPath)) {
    throw "Expected uploaded file not found at $missingPath. Make sure the server runs from ProjectRoot: $ProjectRoot"
}
Remove-Item -LiteralPath $missingPath -Force

$auditBeforeRepair = Invoke-GetText (New-ApiUrl "/api/metadata/audit")
Assert-Contains $auditBeforeRepair "MissingFile" "Audit did not report MissingFile after physical file removal."
Assert-Contains $auditBeforeRepair $missingName "Audit did not include the simulated missing file."

Write-Host "[7/8] run automatic repair and verify MissingFile is gone"
$repairResponse = Invoke-PostText (New-ApiUrl "/api/metadata/repair")
Assert-Contains $repairResponse "repaired_missing" "Repair endpoint did not return repair summary."
$auditAfterRepair = Invoke-GetText (New-ApiUrl "/api/metadata/audit")
Assert-NotContains $auditAfterRepair $missingName "Repaired missing file is still reported by audit."

if (-not $SkipCompact) {
    Write-Host "[8/8] compact metadata log"
    $compactResponse = Invoke-PostText (New-ApiUrl "/api/metadata/compact")
    Assert-Contains $compactResponse "Metadata compacted" "Compact endpoint did not return success."
} else {
    Write-Host "[8/8] compact skipped by -SkipCompact"
}

Write-Host ""
Write-Host "Smoke test passed."
