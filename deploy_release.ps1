param(
    [string]$ExePath = "D:\videoCourse\LanLinkChat\bin\release\LanLinkChat.exe",
    [string]$QtDeployPath = "D:\APP\Qt\6.11.0\mingw_64\bin\windeployqt.exe",
    [string]$ObjdumpPath = "D:\APP\Qt\Tools\mingw1310_64\bin\objdump.exe",
    [string]$MsysBinPath = "D:\APP\msys64\mingw64\bin"
)

$ErrorActionPreference = "Stop"

function Get-ImportedDllNames {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BinaryPath,

        [Parameter(Mandatory = $true)]
        [string]$ObjdumpExe
    )

    & $ObjdumpExe -p $BinaryPath 2>$null |
        Select-String "DLL Name:" |
        ForEach-Object {
            if ($_ -match "DLL Name:\s+(.+)$") {
                $matches[1].Trim()
            }
        } |
        Sort-Object -Unique
}

foreach ($path in @($ExePath, $QtDeployPath, $ObjdumpPath, $MsysBinPath)) {
    if (-not (Test-Path $path)) {
        throw "Path not found: $path"
    }
}

$exeFullPath = (Resolve-Path $ExePath).Path
$targetDir = Split-Path -Parent $exeFullPath
$qtDeployFullPath = (Resolve-Path $QtDeployPath).Path
$objdumpFullPath = (Resolve-Path $ObjdumpPath).Path
$msysBinFullPath = (Resolve-Path $MsysBinPath).Path

Write-Host "Running windeployqt for $exeFullPath"
& $qtDeployFullPath --release --compiler-runtime $exeFullPath

$queue = [System.Collections.Generic.Queue[string]]::new()
$queued = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$processed = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)

function Enqueue-IfNeeded {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $resolvedPath = (Resolve-Path $Path).Path
    if ($queued.Add($resolvedPath)) {
        $queue.Enqueue($resolvedPath)
    }
}

Enqueue-IfNeeded -Path $exeFullPath

Get-ChildItem -Path $targetDir -Filter *.dll -File -ErrorAction SilentlyContinue |
    ForEach-Object { Enqueue-IfNeeded -Path $_.FullName }

$copiedCount = 0

while ($queue.Count -gt 0) {
    $binaryPath = $queue.Dequeue()
    if (-not $processed.Add($binaryPath)) {
        continue
    }

    $dllNames = Get-ImportedDllNames -BinaryPath $binaryPath -ObjdumpExe $objdumpFullPath
    foreach ($dllName in $dllNames) {
        $targetDllPath = Join-Path $targetDir $dllName
        if (Test-Path $targetDllPath) {
            Enqueue-IfNeeded -Path $targetDllPath
            continue
        }

        $msysDllPath = Join-Path $msysBinFullPath $dllName
        if (Test-Path $msysDllPath) {
            Copy-Item -Path $msysDllPath -Destination $targetDllPath -Force
            $copiedCount++
            Write-Host "Copied $dllName"
            Enqueue-IfNeeded -Path $targetDllPath
        }
    }
}

$finalDllCount = (Get-ChildItem -Path $targetDir -Filter *.dll -File -ErrorAction SilentlyContinue).Count
Write-Host "Deployment complete. Copied $copiedCount MSYS2 DLL(s)."
Write-Host "Target directory: $targetDir"
Write-Host "DLL count in target directory: $finalDllCount"
