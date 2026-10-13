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
$stream = $null
try {
	Write-Host 'Legacy Evolve'
	$folder = (Read-Host 'Directory containing Evolve.exe').Trim().Trim('"')
	if ([string]::IsNullOrWhiteSpace($folder)) { throw 'No directory entered.' }
	$folder = [IO.Path]::GetFullPath($folder)
	$exe = Join-Path $folder 'Evolve.exe'
	Write-Host '1 - Patch'
	Write-Host '2 - Restore'
	$choice = (Read-Host 'Choose 1 or 2').Trim()
	if ($choice -notin '1', '2') { throw 'Choose 1 or 2.' }
	$patch = $choice -eq '1'
	$stream = [IO.File]::Open($exe, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
	if ($stream.Length -ne 51468800) { throw 'Unsupported executable. Expected Evolve Legacy 1.0.252019.252019.' }
	$image = New-Object byte[] ([int]$stream.Length)
	$read = 0
	while ($read -lt $image.Length) {
		$count = $stream.Read($image, $read, $image.Length - $read)
		if ($count -eq 0) { throw 'Could not read the complete executable.' }
		$read += $count
	}
	$edits = @(
		@{ Offset = 0x2a6594e; Name = 'WINHTTP.dll' }
		@{ Offset = 0x2a62e04; Name = 'KERNEL32.dll' }
		@{ Offset = 0x2a627c6; Name = 'ADVAPI32.dll' }
		@{ Offset = 0x2a65ad2; Name = 'WINTRUST.dll' }
	)
	$originalCount = 0
	$patchedCount = 0
	$normalized = [byte[]]$image.Clone()
	foreach ($edit in $edits) {
		$edit.Original = [Text.Encoding]::ASCII.GetBytes($edit.Name + [char]0)
		$edit.Patched = New-Object byte[] $edit.Original.Length
		[Text.Encoding]::ASCII.GetBytes('Legacy.dll').CopyTo($edit.Patched, 0)
		$current = [Convert]::ToBase64String($image, $edit.Offset, $edit.Original.Length)
		if ($current -eq [Convert]::ToBase64String($edit.Original)) { $originalCount++ }
		elseif ($current -eq [Convert]::ToBase64String($edit.Patched)) { $patchedCount++ }
		else { throw 'Unexpected import bytes. Use the supported original Evolve.exe.' }
		$edit.Original.CopyTo($normalized, $edit.Offset)
	}
	if ($originalCount -ne 4 -and $patchedCount -ne 4) { throw 'The executable is only partially patched. Restore a clean copy.' }
	$sha = [Security.Cryptography.SHA256]::Create()
	try { $hash = [BitConverter]::ToString($sha.ComputeHash($normalized)).Replace('-', '') }
	finally { $sha.Dispose() }
	if ($hash -ne 'C2DC9E20073B7520C130D3060F4779C01B6BC35BC494DB441568C8965B1AF371') {
		throw 'The executable differs from the supported Legacy build. No changes made.'
	}
	try {
		foreach ($edit in $edits) {
			$bytes = if ($patch) { $edit.Patched } else { $edit.Original }
			[void]$stream.Seek($edit.Offset, [IO.SeekOrigin]::Begin)
			$stream.Write($bytes, 0, $bytes.Length)
		}
		$stream.Flush($true)
	} catch {
		foreach ($edit in $edits) {
			[void]$stream.Seek($edit.Offset, [IO.SeekOrigin]::Begin)
			$stream.Write($image, $edit.Offset, $edit.Original.Length)
		}
		$stream.Flush($true)
		throw
	}
	if ($patch) { Write-Host 'Patched. Start Evolve.exe to play.' }
	else { Write-Host 'Original executable restored.' }
} catch {
	Write-Host ('Error: ' + $_.Exception.Message) -ForegroundColor Red
	exit 1
} finally {
	if ($stream) { $stream.Dispose() }
}
