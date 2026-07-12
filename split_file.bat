@echo off
setlocal

REM === CONFIG ===
set FILE=main.cpp
set LINES_PER_CHUNK=9000

REM === Delete old chunks ===
del "%FILE%_b64_part*.txt" 2>nul

REM === Encode whole file to Base64 ===
powershell -NoLogo -NoProfile -Command ^
  "$path = '%FILE%';" ^
  "$bytes = [System.IO.File]::ReadAllBytes($path);" ^
  "$b64 = [System.Convert]::ToBase64String($bytes);" ^
  "[System.IO.File]::WriteAllText($path + '_b64.txt', $b64);"

REM === Split Base64 into chunks ===
set B64FILE=%FILE%_b64.txt

powershell -NoLogo -NoProfile -Command ^
  "$file = '%B64FILE%';" ^
  "$chunkSize = %LINES_PER_CHUNK%;" ^
  "$text = Get-Content $file -Raw;" ^
  "$len = $text.Length;" ^
  "$chunk = 1;" ^
  "for ($i = 0; $i -lt $len; $i += $chunkSize) {" ^
  "  $outFile = \"$file`_part$chunk.txt\";" ^
  "  $end = [Math]::Min($i + $chunkSize, $len);" ^
  "  $segment = $text.Substring($i, $end - $i);" ^
  "  Set-Content $outFile $segment;" ^
  "  Write-Host \"Created $outFile\";" ^
  "  $chunk++;" ^
  "}"

echo Done.
endlocal
