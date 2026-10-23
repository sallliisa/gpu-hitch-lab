#Requires -Version 5.1
#Requires -RunAsAdministrator
# GPU Hitch Lab supervisor: runs DX12 renderer, PresentMon, and WPR circular ETW.

[CmdletBinding()]
param(
 [ValidateSet('off','on')][string]$Vsync='off',
 [ValidateRange(0,1000)][int]$Fps=60,
 [ValidateRange(5,1000)][double]$HitchMs=30,
 [ValidateRange(0,168)][double]$Hours=0,
 [ValidateRange(0,10080)][double]$Minutes=30,
 [ValidateRange(0,604800)][double]$Seconds=0,
 [ValidateSet('off','on','boost')][string]$Reflex='on',
 [ValidateRange(128,2048)][int]$BudgetMB=2048,
 [ValidateRange(1,120)][int]$PostHitchSeconds=10,
 [switch]$SkipEtw,
 [switch]$SkipPresentMon,
 [switch]$Windowed
)
$ErrorActionPreference='Stop'
$duration=$Hours*3600.0+$Minutes*60.0+$Seconds
if ($duration -le 0 -or $duration -gt 604800) { throw 'Supervised runs need a positive duration of at most seven days.' }
$root=$PSScriptRoot
$logDir=Join-Path $root 'logs'
New-Item -Path $logDir -ItemType Directory -Force | Out-Null
$startedUtc=[DateTime]::UtcNow
$runId=$startedUtc.ToString('yyyyMMdd-HHmmss-fff')
$runDir=Join-Path $logDir "diagnostics-$runId"
New-Item -Path $runDir -ItemType Directory -Force | Out-Null
$notes=Join-Path $runDir 'trace-notes.txt'
function Note([string]$message) {
 $line='{0} {1}' -f ([DateTime]::UtcNow.ToString('o')), $message
 Add-Content -LiteralPath $notes -Value $line -Encoding UTF8
 Write-Host $line
}
function UsedBytes {
 [long]$sum=0
 Get-ChildItem -LiteralPath $runDir -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object { $sum += $_.Length }
 Get-ChildItem -LiteralPath $logDir -File -ErrorAction SilentlyContinue |
   Where-Object { $_.CreationTimeUtc -ge $startedUtc.AddSeconds(-2) } |
   ForEach-Object { $sum += $_.Length }
 return $sum
}
$budget=[long]$BudgetMB*1024L*1024L
$exeName=if (Test-Path (Join-Path $root 'GpuHitchLabReflex.exe')) { 'GpuHitchLabReflex.exe' }
         elseif (Test-Path (Join-Path $root 'GpuHitchLab.exe')) { 'GpuHitchLab.exe' }
         else { throw 'GpuHitchLab.exe / GpuHitchLabReflex.exe missing.' }
$isReflex=$exeName -eq 'GpuHitchLabReflex.exe'
$exe=Join-Path $root $exeName
$argsForApp=@('--vsync',$Vsync,'--fps',"$Fps",'--hitch-ms',"$HitchMs",
  '--hours',"$Hours",'--minutes',"$Minutes",'--seconds',"$Seconds",'--max-log-mb','384')
if (-not $Windowed) { $argsForApp += '--borderless' }
if ($isReflex) { $argsForApp += @('--reflex',$Reflex) }
$wpr=Get-Command wpr.exe -ErrorAction SilentlyContinue
$session="GpuHitchLab-$PID"
$wprRunning=$false
$wprDisabled=$false
$wprIndex=0
$pmProcess=$null
$app=$null
$pmExe=Join-Path $root 'PresentMon-2.6.0-x64.exe'
$seen=0
$pending=$null
$cooldown=[DateTime]::MinValue

function StartWpr {
 if ($SkipEtw -or -not $wpr -or $script:wprDisabled) { return }
 try {
   $output=& $wpr.Source -start CPU.light -start GPU -instancename $session 2>&1 | Out-String
   if ($LASTEXITCODE -eq 0) {
      $script:wprRunning=$true
      Note 'WPR CPU.light + GPU started in circular memory mode.'
   } else {
      $script:wprDisabled=$true
      Note "WPR unavailable (exit $LASTEXITCODE): $output"
   }
 } catch { $script:wprDisabled=$true; Note "WPR failed: $_" }
}
function CancelWpr {
 if (-not $script:wprRunning) { return }
 try { $output=& $wpr.Source -cancel -instancename $session 2>&1 | Out-String; Note "WPR cancelled: $output" }
 catch { Note "WPR cancel error: $_" }
 $script:wprRunning=$false
}
function SaveWpr {
 if (-not $script:wprRunning) { return }
 $script:wprIndex++
 $etl=Join-Path $runDir ('hitch-{0:D3}.etl' -f $script:wprIndex)
 Note 'Saving WPR buffer: export can itself perturb frame timings.'
 try {
   $output=& $wpr.Source -stop $etl -instancename $session 2>&1 | Out-String
   $script:wprRunning=$false
   if ($LASTEXITCODE -ne 0) { Note "WPR save failed: $output"; return }
   Note "Saved trace: $etl"
   if ((UsedBytes) -gt $budget) {
     Remove-Item -LiteralPath $etl -Force -ErrorAction SilentlyContinue
     $script:wprDisabled=$true
     Note 'Disk budget exceeded while writing the ETL; newest trace discarded.'
   }
 } catch { $script:wprRunning=$false; Note "WPR stop failed: $_" }
}

Note "Run $exeName duration=$duration seconds fps=$Fps vsync=$Vsync"
Note 'ETL captures can contain private process information. Review files before sharing.'
Note 'WPR buffer is circular, but the length of pre-hitch history varies by event rate and profile.'
try {
 StartWpr
 $app=Start-Process -FilePath $exe -ArgumentList $argsForApp -WorkingDirectory $root -PassThru
 Note "Renderer PID $($app.Id)"
 if (-not $SkipPresentMon) {
   if (Test-Path -LiteralPath $pmExe) {
     try {
       $pmCsv=Join-Path $runDir 'presentmon-all-frames.csv'
       $pmArgs='--process_id {0} --output_file "{1}" --terminate_on_proc_exit --no_console_stats' -f $app.Id,$pmCsv
       $pmProcess=Start-Process -FilePath $pmExe -ArgumentList $pmArgs -WorkingDirectory $root -PassThru -WindowStyle Hidden
       Note "PresentMon process started ($($pmProcess.Id))"
     } catch { Note "PresentMon failed to start: $_" }
   } else { Note 'PresentMon executable not found in ZIP.' }
 }
 $nextBudgetCheck=[DateTime]::UtcNow
 $csv=$null
 while (-not $app.HasExited) {
   Start-Sleep -Milliseconds 400
   $app.Refresh()
   if (-not $csv) {
     $csv=Get-ChildItem -LiteralPath $logDir -Filter 'hitches-*.csv' -File -ErrorAction SilentlyContinue |
        Where-Object { $_.CreationTimeUtc -ge $startedUtc.AddSeconds(-2) } |
        Sort-Object CreationTimeUtc -Descending | Select-Object -First 1
   }
   if ($csv -and (Test-Path -LiteralPath $csv.FullName)) {
     $rows=[Math]::Max(0,@(Get-Content -LiteralPath $csv.FullName -ErrorAction SilentlyContinue).Count-1)
     if ($rows -gt $seen) {
       Note "New hitch rows: $($rows-$seen), total=$rows"
       $seen=$rows
       if ($wprRunning -and [DateTime]::UtcNow -ge $cooldown) { $pending=[DateTime]::UtcNow }
     }
   }
   if ($pending -and $wprRunning -and (([DateTime]::UtcNow-$pending).TotalSeconds -ge $PostHitchSeconds)) {
     SaveWpr
     $pending=$null
     $cooldown=[DateTime]::UtcNow.AddSeconds(15)
     if (-not $wprDisabled -and (UsedBytes) -lt ($budget*0.85)) { StartWpr }
   }
   if ([DateTime]::UtcNow -ge $nextBudgetCheck) {
     $nextBudgetCheck=[DateTime]::UtcNow.AddSeconds(3)
     if ((UsedBytes) -ge ($budget*0.92)) {
       Note 'Near disk budget; stopping external collectors, retaining renderer CSV.'
       $wprDisabled=$true
       CancelWpr
       if ($pmProcess -and -not $pmProcess.HasExited) {
         Stop-Process -Id $pmProcess.Id -Force -ErrorAction SilentlyContinue
       }
       $pmProcess=$null
     }
   }
 }
 Note "Renderer exit code: $($app.ExitCode)"
 if ($pending -and $wprRunning) { Note 'Saving final partial post-hitch buffer.'; SaveWpr }
} finally {
 CancelWpr
 if ($pmProcess) {
   try {
     if (-not $pmProcess.HasExited) {
       $pmProcess.WaitForExit(5000) | Out-Null
       if (-not $pmProcess.HasExited) {
          Stop-Process -Id $pmProcess.Id -Force -ErrorAction SilentlyContinue
       }
     }
   } catch { Note "PresentMon shutdown error: $_" }
 }
 Note "Finished. Total current-run bytes: $(UsedBytes). Logs: $logDir"
}
