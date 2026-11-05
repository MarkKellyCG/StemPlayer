<#
 StemPlayer helper installer (Windows)

 Creates %APPDATA%\StemPlayer with:
   stemsplit.py   the splitter script
   venv\          a private Python environment with PyTorch + Demucs

 Requirements: Python 3.10, 3.11 or 3.12 (python.org installer; tick "Add to PATH" or use the py launcher).
 Run from PowerShell:   powershell -ExecutionPolicy Bypass -File .\install_helper_windows.ps1
 NVIDIA GPU users can add -Cuda for a much faster split.

 NOTE: this script has not been run by its author on a real Windows machine yet - if a step fails,
 the message printed is the pip/python error; paste it back and it can be fixed.
#>
param([switch]$Cuda)

$ErrorActionPreference = 'Stop'
$dest = Join-Path $env:APPDATA 'StemPlayer'
$venv = Join-Path $dest 'venv'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Find-Python {
    foreach ($v in '3.12', '3.11', '3.10') {
        try { $p = & py "-$v" -c "import sys;print(sys.executable)" 2>$null; if ($LASTEXITCODE -eq 0 -and $p) { return $p.Trim() } } catch {}
    }
    try {
        $ver = & python -c "import sys;print('%d.%d' % sys.version_info[:2])" 2>$null
        if ($ver -in '3.10', '3.11', '3.12') { return (& python -c "import sys;print(sys.executable)").Trim() }
    } catch {}
    return $null
}

$py = Find-Python
if (-not $py) { throw "Python 3.10-3.12 not found. Install it from python.org (not the Microsoft Store build) and re-run." }
Write-Host "Using Python: $py"

New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item (Join-Path $here 'stemsplit.py') (Join-Path $dest 'stemsplit.py') -Force

if (-not (Test-Path (Join-Path $venv 'Scripts\python.exe'))) {
    Write-Host "Creating virtual environment..."
    & $py -m venv $venv
}
$vpy = Join-Path $venv 'Scripts\python.exe'

function Run-Pip { param([string[]]$a)
    & $vpy -m pip @a
    if ($LASTEXITCODE -ne 0) { throw "pip failed: pip $($a -join ' ')" }
}

Run-Pip @('install', '--upgrade', 'pip')

Write-Host "Installing PyTorch (this is the big download)..."
if ($Cuda) {
    Run-Pip @('install', 'torch>=2.1,<2.6', 'torchaudio>=2.1,<2.6', '--index-url', 'https://download.pytorch.org/whl/cu121')
} else {
    Run-Pip @('install', 'torch>=2.1,<2.6', 'torchaudio>=2.1,<2.6')   # PyPI wheels on Windows are CPU builds
}

Write-Host "Installing audio + Demucs dependencies..."
Run-Pip @('install', 'numpy', 'scipy', 'soundfile', 'omegaconf', 'einops', 'pyyaml', 'tqdm', 'retrying', 'submitit', 'treetable')
# --no-deps: demucs 4.0.1 pins old torchaudio versions; dora-search would otherwise drag in a second torch
Run-Pip @('install', '--no-deps', 'dora-search', 'julius==0.2.7', 'openunmix==1.3.0', 'demucs==4.0.1')

Write-Host "Checking imports..."
& $vpy -c "import torch, torchaudio, soundfile, scipy; import demucs.pretrained, demucs.apply; print('imports OK, torch', torch.__version__, 'cuda', torch.cuda.is_available())"
if ($LASTEXITCODE -ne 0) { throw "Import check failed - see the error above." }

Write-Host "Downloading the htdemucs_6s model (first time only)..."
& $vpy (Join-Path $dest 'stemsplit.py') --download-model
if ($LASTEXITCODE -ne 0) { throw "Model download failed - check your internet connection / proxy and re-run." }

Write-Host ""
Write-Host "Done. Helper installed to: $dest"
Write-Host "StemPlayer finds it automatically. Test from a console:"
Write-Host "  `"$vpy`" `"$dest\stemsplit.py`" --input song.wav --output song_stems"
