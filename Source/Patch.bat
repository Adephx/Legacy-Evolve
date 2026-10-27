@echo off
setlocal
set "LE_PATCH_SCRIPT=%~f0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "$text = [IO.File]::ReadAllText($env:LE_PATCH_SCRIPT); $marker = ':legacy_evolve_script'; & ([scriptblock]::Create($text.Substring($text.LastIndexOf($marker) + $marker.Length)))"
set "LE_PATCH_RESULT=%errorlevel%"
echo.
pause
exit /b %LE_PATCH_RESULT%
:legacy_evolve_script
$ErrorActionPreference = 'Stop'
$targets = New-Object 'Collections.Generic.List[object]'

function Get-ImageHash([byte[]]$image) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($sha.ComputeHash($image)).Replace('-', '') }
    finally { $sha.Dispose() }
}

function Get-StreamHash($stream) {
    $stream.Position = 0
    $sha = [Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
    finally { $sha.Dispose() }
}

function Open-Target($path, $length, $originalHash, $patchedHash, $patch) {
    $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $target = [pscustomobject]@{
        Path = $path; Stream = $stream; Before = $null; After = $null
        BeforeHash = ''; ExpectedHash = ''; Edits = @(); Changed = $false
    }
    $targets.Add($target)
    $name = [IO.Path]::GetFileName($path)
    if ($stream.Length -ne $length) { throw "Unsupported $name. Expected Evolve Legacy 1.0.252019.252019." }
    $image = New-Object byte[] $length
    $read = 0
    while ($read -lt $image.Length) {
        $count = $stream.Read($image, $read, $image.Length - $read)
        if ($count -eq 0) { throw "Could not read $name." }
        $read += $count
    }
    $hash = Get-ImageHash $image
    if ($hash -ne $originalHash -and $hash -ne $patchedHash) {
        throw "Unsupported or modified $name. No changes made."
    }
    $target.Before = $image
    $target.BeforeHash = $hash
    $target.ExpectedHash = if ($patch) { $patchedHash } else { $originalHash }
    $target.Changed = $hash -ne $target.ExpectedHash
    if ($target.Changed) { $target.After = [byte[]]$image.Clone() }
    return $target
}

try {
    Write-Host 'Legacy Evolve'
    $folder = (Read-Host 'Game folder or directory containing Evolve.exe').Trim().Trim('"')
    if ([string]::IsNullOrWhiteSpace($folder)) { throw 'No directory entered.' }
    $folder = [IO.Path]::GetFullPath($folder)
    if ([IO.Path]::GetFileName($folder.TrimEnd('\')) -ieq 'Bin64_SteamRetail') {
        $gameFolder = [IO.Path]::GetDirectoryName($folder.TrimEnd('\'))
        $exe = Join-Path $folder 'Evolve.exe'
    } else {
        $gameFolder = $folder
        $exe = Join-Path $folder 'Evolve.exe'
        if (!(Test-Path -LiteralPath $exe -PathType Leaf)) {
            $exe = Join-Path $folder 'Bin64_SteamRetail\Evolve.exe'
        }
    }
    Write-Host '1 - Patch'
    Write-Host '2 - Unpatch'
    $choice = (Read-Host 'Choose 1 or 2').Trim()
    if ($choice -notin '1', '2') { throw 'Choose 1 or 2.' }
    if (Get-Process -Name Evolve -ErrorAction SilentlyContinue) { throw 'Close Evolve first.' }
    $patch = $choice -eq '1'
    $perks = !$patch
    if ($patch) {
        $answer = (Read-Host 'Enable access to all perks by default? (y/N)').Trim()
        if ($answer -notin '', 'y', 'yes', 'n', 'no') { throw 'Choose yes or no.' }
        $perks = $answer -in 'y', 'yes'
    }

    # Validate and lock every selected file before writing either patch.
    $target = Open-Target $exe 51468800 'C2DC9E20073B7520C130D3060F4779C01B6BC35BC494DB441568C8965B1AF371' '7D07D7BABE329CED34B52CA41D6D138BAF7C6F10BA2EA66ACC9F5F4B5FDA86FE' $patch
    if ($target.Changed) {
        $imports = @(
            @{ Offset = 0x2a6594e; Name = 'WINHTTP.dll' }
            @{ Offset = 0x2a62e04; Name = 'KERNEL32.dll' }
            @{ Offset = 0x2a627c6; Name = 'ADVAPI32.dll' }
            @{ Offset = 0x2a65ad2; Name = 'WINTRUST.dll' }
        )
        foreach ($import in $imports) {
            $bytes = [Text.Encoding]::ASCII.GetBytes($import.Name + [char]0)
            if ($patch) {
                $bytes = New-Object byte[] $bytes.Length
                [Text.Encoding]::ASCII.GetBytes('Legacy.dll').CopyTo($bytes, 0)
            }
            $bytes.CopyTo($target.After, $import.Offset)
            $target.Edits += @{ Offset = $import.Offset; Length = $bytes.Length }
        }
    }
    if ($perks) {
        $pak = Join-Path $gameFolder 'Game\Libs.pak'
        $target = Open-Target $pak 7166560 'D8BA7772F17F5491C8574D01F6DD470A177A452B03E91F934D2A1B0791144606' 'B451FFFD20DBA49AEFFAA79254598DA1D76FA0FC983BCDFA148E66F2EFD23C64' $patch
        if ($target.Changed) {
            # This reversible delta changes only the default-profile payload.
            $delta = [Convert]::FromBase64String(@'
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
'@)
            if ($delta.Length -ne 2214) { throw 'Invalid perk patch data.' }
            for ($i = 0; $i -lt $delta.Length; $i++) {
                $target.After[2838733 + $i] = $target.After[2838733 + $i] -bxor $delta[$i]
            }
            $target.Edits = @(@{ Offset = 2838733; Length = $delta.Length })
        }
    }
    $changes = @($targets | Where-Object { $_.Changed })
    foreach ($target in $changes) {
        if ((Get-ImageHash $target.After) -ne $target.ExpectedHash) {
            throw 'Patch verification failed. No changes made.'
        }
    }
    if ($changes.Count -eq 0) {
        Write-Host 'No changes needed.'
    } else {
        $written = New-Object 'Collections.Generic.List[object]'
        try {
            foreach ($target in $changes) {
                $written.Add($target)
                foreach ($edit in $target.Edits) {
                    $target.Stream.Position = $edit.Offset
                    $target.Stream.Write($target.After, $edit.Offset, $edit.Length)
                }
                $target.Stream.Flush($true)
                if ((Get-StreamHash $target.Stream) -ne $target.ExpectedHash) { throw 'Write verification failed.' }
            }
        } catch {
            $failure = $_.Exception.Message
            $rollbackFailed = @()
            for ($i = $written.Count - 1; $i -ge 0; $i--) {
                $target = $written[$i]
                try {
                    foreach ($edit in $target.Edits) {
                        $target.Stream.Position = $edit.Offset
                        $target.Stream.Write($target.Before, $edit.Offset, $edit.Length)
                    }
                    $target.Stream.Flush($true)
                    if ((Get-StreamHash $target.Stream) -ne $target.BeforeHash) { throw 'Restore verification failed.' }
                } catch { $rollbackFailed += [IO.Path]::GetFileName($target.Path) }
            }
            if ($rollbackFailed.Count) { throw "Update failed. Could not restore: $($rollbackFailed -join ', '). Use a clean copy." }
            throw "Update failed; changes reversed. $failure"
        }
        if ($patch) { Write-Host 'Patched.' }
        else { Write-Host 'Unpatched.' }
    }
} catch {
    Write-Host ('Error: ' + $_.Exception.Message) -ForegroundColor Red
    exit 1
} finally {
    foreach ($target in $targets) { $target.Stream.Dispose() }
}
