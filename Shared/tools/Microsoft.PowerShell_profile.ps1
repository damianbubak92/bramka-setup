# === Bramka deploy helpers ===

# Konfiguracja
$BRAMKA_HOST = "root@192.168.2.170"
$REPO = "C:\SmartHome"                          # monorepo root (po migracji z bramka-setup)
$PROJECT_NAME = "gateway_m4f"                    # M4F project name (= .out filename)
$M4F_DIR = "$REPO\Gateway\Firmware\M4F"          # M4F CCS project (build output w \Debug)

function Deploy-M4F {
    param(
        [string]$ProjectName = $PROJECT_NAME,
        [switch]$NoLogs,
        [switch]$LogForever,
        [int]$LogSeconds = 5
    )
    
    $fwPath = "$M4F_DIR\Debug\$ProjectName.out"
    
    if (-not (Test-Path $fwPath)) {
        Write-Host "ERROR: firmware not found: $fwPath" -ForegroundColor Red
        Write-Host "Make sure project is built in CCS first (Ctrl+Shift+B)" -ForegroundColor Yellow
        return
    }
    
    $fwSize = (Get-Item $fwPath).Length
    $fwTime = (Get-Item $fwPath).LastWriteTime
    Write-Host "=== Deploying $ProjectName ===" -ForegroundColor Cyan
    Write-Host "Source:   $fwPath" -ForegroundColor Gray
    Write-Host "Size:     $([math]::Round($fwSize/1KB, 1)) KB" -ForegroundColor Gray
    Write-Host "Modified: $fwTime" -ForegroundColor Gray
    Write-Host ""
    
    # Step 1: SCP firmware
    Write-Host "[1/3] Uploading to bramka..." -ForegroundColor Cyan
    scp $fwPath "${BRAMKA_HOST}:/tmp/my_fw.out"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: SCP failed" -ForegroundColor Red
        return
    }
    
    # Step 2: m4f-reload on bramka
    Write-Host "[2/3] Reloading M4F firmware..." -ForegroundColor Cyan
    ssh $BRAMKA_HOST "m4f-reload"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: m4f-reload failed" -ForegroundColor Red
        return
    }
    
    # Step 3: Show logs (3 modes)
    if ($NoLogs) {
        Write-Host ""
        Write-Host "=== Done (no logs) ===" -ForegroundColor Green
        return
    }
    
    if ($LogForever) {
        Write-Host ""
        Write-Host "[3/3] Watching logs forever (Ctrl+C to stop)..." -ForegroundColor Cyan
        Write-Host ""
        ssh $BRAMKA_HOST "m4f-watch"
    } else {
        Write-Host ""
        Write-Host "[3/3] Watching logs for $LogSeconds seconds..." -ForegroundColor Cyan
        Write-Host "(Ctrl+C to stop early)" -ForegroundColor Gray
        Write-Host ""
        ssh $BRAMKA_HOST "timeout $LogSeconds m4f-watch"
    }
    
    Write-Host ""
    Write-Host "=== Done ===" -ForegroundColor Green
}
function Deploy-Go {
    param(
        [string]$ServiceName = "protocol-test",
        [switch]$Build,
        [switch]$Run
    )
    
    $localDir = "$REPO\Gateway\Software\$ServiceName"
    $remoteDir = "/opt/bramka/$ServiceName"
    
    if (-not (Test-Path $localDir)) {
        Write-Host "ERROR: Local dir not found: $localDir" -ForegroundColor Red
        return
    }
    
    Write-Host "=== Deploying $ServiceName ===" -ForegroundColor Cyan
    
    # Step 1: Create remote dir
    ssh $BRAMKA_HOST "mkdir -p $remoteDir"
    
    # Step 2: Copy shared headers for cgo (protocol.h, automation.h, node_protocol.h, ...)
    $sharedDir = "$REPO\Shared\Protocol"
    $sharedHeaders = Get-ChildItem -Path $sharedDir -Filter *.h
    if ($sharedHeaders) {
        Write-Host "[1/3] Copying shared headers ($($sharedHeaders.Count))..." -ForegroundColor Cyan
        foreach ($h in $sharedHeaders) {
            scp $h.FullName "${BRAMKA_HOST}:$remoteDir/$($h.Name)"
        }
    }
    
    # Step 3: Copy all .go files
    Write-Host "[2/3] Copying Go source files..." -ForegroundColor Cyan
    $goFiles = Get-ChildItem "$localDir\*.go" -ErrorAction SilentlyContinue
    foreach ($f in $goFiles) {
        scp $f.FullName "${BRAMKA_HOST}:$remoteDir/"
    }
    
    # Step 4: Build (optional)
    if ($Build -or $Run) {
        Write-Host "[3/3] Building on bramka..." -ForegroundColor Cyan
        ssh $BRAMKA_HOST "cd $remoteDir && (test -f go.mod || go mod init $ServiceName) && go mod tidy && go build"
        if ($LASTEXITCODE -ne 0) {
            Write-Host "ERROR: Build failed" -ForegroundColor Red
            return
        }
    }
    
# Step 5: Run (optional)
    if ($Run) {
        # Kill any existing instance first (to avoid resource busy on rpmsg device)
        Write-Host "Killing any existing $ServiceName process..." -ForegroundColor Gray
        ssh $BRAMKA_HOST "pkill -f $ServiceName 2>/dev/null; sleep 0.3" 2>$null
        
        Write-Host "Running (Ctrl+C to stop)..." -ForegroundColor Cyan
        # -t forces pseudo-TTY so Ctrl+C properly propagates as SIGINT to remote process
        ssh -t $BRAMKA_HOST "cd $remoteDir && ./$ServiceName"
    }
    
    Write-Host "=== Done ===" -ForegroundColor Green
}

function Watch-M4F {
    ssh $BRAMKA_HOST "m4f-watch"
}

function Test-Rpmsg {
    param([int]$N = 3)
    ssh $BRAMKA_HOST "timeout 10 rpmsg_char_simple -r 9 -n $N"
}

function Connect-Bramka {
    ssh $BRAMKA_HOST
}

function Get-M4FState {
    ssh $BRAMKA_HOST "echo 'M4F state:'; cat /sys/class/remoteproc/remoteproc0/state; echo ''; echo 'RPMsg devices:'; ls /dev/rpmsg*"
}

function Install-GoService {
    param(
        [string]$ServiceName = "rpmsg-service"
    )
    
    $repoPath = $REPO
    $unitFile = "$repoPath\Gateway\Setup\systemd\$ServiceName.service"
    
    if (-not (Test-Path $unitFile)) {
        Write-Host "ERROR: Unit file not found: $unitFile" -ForegroundColor Red
        return
    }
    
    Write-Host "=== Installing $ServiceName systemd unit ===" -ForegroundColor Cyan
    
    # Copy unit file to bramka
    Write-Host "[1/4] Copying unit file..." -ForegroundColor Cyan
    scp $unitFile "${BRAMKA_HOST}:/etc/systemd/system/${ServiceName}.service"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: scp failed" -ForegroundColor Red
        return
    }
    
    # Reload systemd
    Write-Host "[2/4] Reloading systemd..." -ForegroundColor Cyan
    ssh $BRAMKA_HOST "systemctl daemon-reload"
    
    # Enable + restart service
    Write-Host "[3/4] Enabling + restarting service..." -ForegroundColor Cyan
    ssh $BRAMKA_HOST "systemctl enable ${ServiceName}.service && systemctl restart ${ServiceName}.service"
    
    # Wait a moment then check status
    Write-Host "[4/4] Status (after 2s):" -ForegroundColor Cyan
    Start-Sleep -Seconds 2
    ssh $BRAMKA_HOST "systemctl status ${ServiceName}.service --no-pager -l"
    
    Write-Host ""
    Write-Host "=== Done ===" -ForegroundColor Green
    Write-Host "View logs:    ssh root@bramka 'journalctl -u $ServiceName -f'" -ForegroundColor Gray
    Write-Host "Stop:         ssh root@bramka 'systemctl stop $ServiceName'" -ForegroundColor Gray
    Write-Host "Restart:      ssh root@bramka 'systemctl restart $ServiceName'" -ForegroundColor Gray
}

Write-Host "Bramka helpers loaded: Deploy-M4F, Watch-M4F, Test-Rpmsg, Connect-Bramka, Get-M4FState" -ForegroundColor DarkGray

# ============================================================
#  KiCad: dodawanie czesci z LCSC (easyeda2kicad -> biblioteki SmartHome)
#  Uzycie:  lcsc C2765186        (alias dla Add-LcscPart)
#  Efekt:   symbol -> SmartHome.kicad_sym (footprint podpiety),
#           footprint -> SmartHome.pretty, model 3D -> 3dmodels
# ============================================================
function Add-LcscPart {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true, Position=0)]
        [string]$LcscId,
        [switch]$Force
    )

    $libDir   = "C:/SmartHome/Shared/KiCadLib"
    $libBase  = "$libDir/SmartHome"          # -> SmartHome.kicad_sym / .pretty / .3dshapes
    $modelDir = "$libDir/3dmodels"
    $tmp3d    = "$libDir/SmartHome.3dshapes"
    $symFile  = "$libDir/SmartHome.kicad_sym"

    # Normalizacja: dopusc "2765186" lub "C2765186"
    if ($LcscId -match '^\d+$') { $LcscId = "C$LcscId" }
    if ($LcscId -notmatch '^[Cc]\d+$') {
        Write-Host "ERROR: nieprawidlowy numer LCSC '$LcscId' (oczekiwano np. C2765186)" -ForegroundColor Red
        return
    }
    $LcscId = $LcscId.ToUpper()

    # Idempotencja: numer LCSC jest zapisany w property "LCSC Part" symbolu -> przetrwa zmiane nazwy.
    # Jesli juz jest w bibliotece -> nic nie rob (chyba ze -Force = odswiez/nadpisz).
    if (-not $Force -and (Test-Path $symFile) -and (Select-String -Path $symFile -Pattern $LcscId -SimpleMatch -Quiet)) {
        Write-Host "[SKIP] $LcscId juz istnieje w bibliotece SmartHome - nic nie robie. (odswiez: lcsc $LcscId -Force)" -ForegroundColor Yellow
        return
    }

    if (Get-Process -Name kicad,pcbnew,eeschema -ErrorAction SilentlyContinue) {
        Write-Host "UWAGA: KiCad dziala - po dodaniu przeladuj biblioteki lub zrestartuj KiCad, zeby zobaczyc nowy symbol." -ForegroundColor Yellow
    }
    if (-not (Test-Path $modelDir)) { New-Item -ItemType Directory -Path $modelDir | Out-Null }
    if (Test-Path $symFile) { Copy-Item $symFile "$symFile.bak" -Force }   # insurance przed dopisaniem

    Write-Host "[easyeda2kicad] Pobieram $LcscId -> biblioteki SmartHome ..." -ForegroundColor Cyan
    python -m easyeda2kicad --full --lcsc_id=$LcscId --output $libBase --overwrite
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: easyeda2kicad zwrocil blad ($LASTEXITCODE). Backup: SmartHome.kicad_sym.bak" -ForegroundColor Red
        return
    }

    # Modele 3D -> 3dmodels/ (Twoja konwencja) + przekierowanie sciezki (model ...) w footprintach
    if (Test-Path $tmp3d) {
        Get-ChildItem $tmp3d -File | ForEach-Object { Move-Item $_.FullName -Destination $modelDir -Force }
        Remove-Item $tmp3d -Recurse -Force -ErrorAction SilentlyContinue
    }
    Get-ChildItem "$libDir/SmartHome.pretty" -Filter *.kicad_mod | ForEach-Object {
        $txt = [System.IO.File]::ReadAllText($_.FullName)
        if ($txt.Contains("SmartHome.3dshapes")) {
            [System.IO.File]::WriteAllText($_.FullName, $txt.Replace("SmartHome.3dshapes", "3dmodels"))  # UTF-8 bez BOM
        }
    }

    Write-Host "[OK] $LcscId dodany: symbol -> SmartHome.kicad_sym | footprint -> SmartHome.pretty | 3D -> 3dmodels" -ForegroundColor Green
    Write-Host "     W schemacie wstaw symbol z biblioteki 'SmartHome' - footprint i model 3D sa juz podpiete." -ForegroundColor Green
}
Set-Alias lcsc Add-LcscPart
Write-Host "KiCad helper loaded: Add-LcscPart (alias: lcsc <nr_LCSC>)" -ForegroundColor DarkGray