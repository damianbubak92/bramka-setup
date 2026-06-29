# Shared/KiCadLib — współdzielona biblioteka KiCad (SmartHome)

Wspólne **custom / vendor** symbole, footprinty i modele 3D dla wszystkich projektów
hardware w monorepo (`Gateway/Hardware`, `Nodes/*/Hardware`).

## Co tu trzymamy (a czego NIE)
- ✅ Części **niestandardowe / vendor / specyficzne**: CC1310 (QFN RGZ 7×7), balun,
  holder 18500, MCP73123 / TPS63900 / MCP3421, nietypowe złącza, antena.
- ❌ **Generyczne pasywy** (R/C/L SMD, diody ESD, generyczny QFN) — bierz z
  **wbudowanych bibliotek KiCada** (`Resistor_SMD`, `Capacitor_SMD`,
  `Package_DFN_QFN`…). Nie kopiuj ich tutaj — biblioteka ma być chuda.

## Struktura
```
Shared/KiCadLib/
├── SmartHome.pretty/      # footprinty (1 plik .kicad_mod = 1 footprint)
├── SmartHome.kicad_sym    # symbole (utwórz w Symbol Editor → New Library, gdy rysujesz własne)
└── 3dmodels/              # modele 3D (.step / .wrl)
```

## Rejestracja w KiCad (raz na maszynę)
1. **Preferences → Configure Paths** → dodaj zmienną:
   - `SMARTHOME_LIB` = `C:/SmartHome/Shared/KiCadLib`
2. **Preferences → Manage Footprint Libraries → Project Specific Libraries** → **Add**:
   - Nickname: `SmartHome`
   - Path: `${SMARTHOME_LIB}/SmartHome.pretty`
3. (Symbole, jeśli są) **Manage Symbol Libraries → Project Specific** → **Add**:
   - Nickname: `SmartHome`
   - Path: `${SMARTHOME_LIB}/SmartHome.kicad_sym`

> Zakładka **Project Specific** = tabela (`fp-lib-table` / `sym-lib-table`) jedzie z repo
> → odtwarzalne. Każda nowa maszyna ustawia tylko `SMARTHOME_LIB` (krok 1) i działa.
> Modele 3D w `.kicad_mod` referencjonuj przez `${SMARTHOME_LIB}/3dmodels/...`.

## Użycie
Przy przypisywaniu footprintu/symbolu wybierasz bibliotekę **`SmartHome:`**
(np. `SmartHome:CC1310_RGZ_VQFN48`).

## Git
- `.kicad_mod` / `.kicad_sym` / `.pretty` = tekst → commituj normalnie.
- `.step` / `.wrl` = binarne → commituj jeśli małe; duże/pobieralne lepiej pominąć.

## Uwaga RF (CC1310 QFN)
Sprawdź land-pattern z datasheet TI — **thermal pad + raster vias pod padem masy**
są krytyczne dla RF i termiki. Generyczny `QFN-48-1EP_7x7mm_P0.5mm` może wystarczyć,
ale porównaj z zaleceniem TI; jak się różni → zrób custom footprint tutaj.
