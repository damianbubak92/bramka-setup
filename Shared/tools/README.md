# Shared/tools

Dev-laptop helpers (PowerShell), wersjonowane dla odtwarzalności / disaster-recovery.

## `Microsoft.PowerShell_profile.ps1`

Snapshot profilu PowerShell z laptopa deweloperskiego. **To kopia** — żywy plik
to `$PROFILE` (`C:\Users\<user>\Documents\WindowsPowerShell\Microsoft.PowerShell_profile.ps1`).
Po zmianach w profilu odśwież tę kopię (`cp $PROFILE Shared/tools/`).

Zawiera:
- **Bramka deploy helpers**: `Deploy-Go`, `Deploy-M4F`, `Install-GoService`,
  `Watch-M4F`, `Test-Rpmsg`, `Connect-Bramka`, `Get-M4FState` (`$BRAMKA_HOST = root@192.168.2.170`).
- **KiCad**: `Add-LcscPart` (alias `lcsc`) — pobiera część z LCSC przez
  `easyeda2kicad` i wpina do bibliotek `Shared/KiCadLib` (symbol z auto-footprintem,
  footprint → `SmartHome.pretty`, model 3D → `3dmodels`). Idempotentny
  (wykrywa po property `"LCSC Part"`); `lcsc <id> -Force` = odśwież.
  Wymaga `pip install easyeda2kicad`.

### Przywrócenie na nowym laptopie
```powershell
mkdir (Split-Path $PROFILE) -Force
Copy-Item Shared/tools/Microsoft.PowerShell_profile.ps1 $PROFILE -Force
pip install easyeda2kicad   # dla helpera KiCad
. $PROFILE
```
