# Run against a disposable PoB copy: the engine may write its Settings.xml.
# Example (PowerShell 7): ./tests/craft_preview_test.ps1 -ExePath <pob-zh.exe>
#   -PobDir <test-POE1-folder> -OutputDir <test-output-folder>
param(
    [Parameter(Mandatory)][string]$ExePath,
    [Parameter(Mandatory)][string]$PobDir,
    [Parameter(Mandatory)][string]$OutputDir
)
$ErrorActionPreference = 'Stop'
$ExePath = (Resolve-Path -LiteralPath $ExePath).Path
$PobDir = (Resolve-Path -LiteralPath $PobDir).Path
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$OutputDir = (Resolve-Path -LiteralPath $OutputDir).Path
$calls = [Collections.Generic.List[string]]::new()
function Add-Call($Method, $Params = @{}) {
    $path = Join-Path $OutputDir ('params-' + $calls.Count + '.json')
    $Params | ConvertTo-Json -Depth 8 -Compress | Set-Content -LiteralPath $path -Encoding utf8NoBOM
    $calls.Add($Method + ':@' + $path)
}
Add-Call new_build @{ name = 'Craft preview regression' }
Add-Call save_xml
Add-Call craft_item_options
Add-Call craft_item_tooltip @{ rarity = 'RARE'; type = 'Tincture'; base = 'Ashbark Tincture' }
Add-Call craft_item_tooltip @{ rarity = 'RARE'; type = 'Tincture'; base = 'Fulgurite Tincture' }
Add-Call craft_item_tooltip @{ rarity = 'NORMAL'; type = 'Wand'; base = 'Convoking Wand' }
Add-Call craft_item_tooltip @{ rarity = 'RARE'; type = 'Amulet'; base = 'Blue Pearl Amulet'; title = 'Preview title' }
Add-Call save_xml
Add-Call craft_item_options
Add-Call item_edit_begin @{ craft = @{ rarity = 'RARE'; type = 'Wand'; base = 'Convoking Wand'; title = 'Keep this edit' } }
Add-Call item_edit_state
Add-Call craft_item_tooltip @{ rarity = 'RARE'; type = 'Tincture'; base = 'Fulgurite Tincture' }
Add-Call craft_item_tooltip @{ rarity = 'RARE'; type = 'Tincture'; base = 'Invalid base' }
Add-Call item_edit_state
Add-Call list_items
Add-Call self_check

$oldBridge = $env:POB_ZH_BRIDGE
$env:POB_ZH_BRIDGE = (Resolve-Path (Join-Path $PSScriptRoot '..\host\data\bridge\bridge.lua')).Path
$report = Join-Path $OutputDir 'craft-preview-results.json'
try {
    $process = Start-Process -FilePath $ExePath -WorkingDirectory (Split-Path $ExePath) -WindowStyle Hidden -PassThru -ArgumentList @(
        '--bridge-call', ('"' + $PobDir + '"'), ('"' + ($calls -join '|') + '"'), ('"' + $report + '"')
    )
    if (!$process.WaitForExit(60000)) { Stop-Process -Id $process.Id; throw 'Craft preview regression timed out' }
    # One deliberately invalid base must produce a bridge error / exit code 1.
    if ($process.ExitCode -ne 1) { throw "Unexpected probe exit code: $($process.ExitCode)" }
} finally { $env:POB_ZH_BRIDGE = $oldBridge }
$results = @(Get-Content -LiteralPath $report -Raw | ConvertFrom-Json)
$script:checks = 0
function Assert($Condition, $Label) {
    if (!$Condition) { throw "FAIL: $Label (report: $report)" }
    $script:checks++
    "PASS: $Label"
}
function Tip-Text($Index) { ($results[$Index].result.lines | ForEach-Object text) -join "`n" }
Assert ($results.Count -eq $calls.Count) 'every bridge request answered'
foreach ($i in 0..($results.Count - 1)) {
    if ($i -ne 12) { Assert (!$results[$i].error) "request $i succeeded" }
}
Assert ($results[3].result.header -eq 'MAGIC') 'tincture rarity follows the classic normalisation'
Assert ((Tip-Text 3) -match '灰樹皮萃取物' -and (Tip-Text 3) -match '點燃') 'Ashbark name and ignite implicit translated'
Assert ((Tip-Text 4) -match '電熔岩萃取物' -and (Tip-Text 4) -match '感電') 'Fulgurite name and shock implicit translated'
Assert ((Tip-Text 4) -match '20%' -and (Tip-Text 4) -match '7\.00') 'normalised quality and cooldown included'
Assert ($results[5].result.header -eq 'NORMAL' -and $results[5].result.lines.Count -gt 3) 'normal weapon base has a detailed preview'
Assert ((Tip-Text 6) -match 'Preview title') 'rare preview uses the entered title'
Assert ($results[1].result.xml -ceq $results[7].result.xml) 'hovering does not alter the saved build'
$optionsBefore = $results[2].result | ConvertTo-Json -Depth 20 -Compress
$optionsAfter = $results[8].result | ConvertTo-Json -Depth 20 -Compress
Assert ($optionsBefore -ceq $optionsAfter) 'hovering does not change remembered craft selections'
Assert ($results[10].result.raw -ceq $results[13].result.raw) 'preview and failed preview preserve the current edit'
Assert ($results[12].error.message -match 'unknown craft item base') 'invalid base reports an error'
Assert ($results[14].result.items.Count -eq 0) 'preview never adds an item'
Assert ($results[15].result.ok) 'bridge compatibility gate passes after previews'
"$script:checks checks passed"
