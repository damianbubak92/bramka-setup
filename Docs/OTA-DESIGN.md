# OTA-DESIGN.md — Over-The-Air updates (DESIGN, not yet implemented)

> **Status: ZAPROJEKTOWANE 2026-07-27, NIEzaimplementowane.** Wdrożyć gdy flota urośnie
> (dużo nodów, grupy tego samego typu — wtedy broadcast się opłaca). Ten plik trzyma
> pełny projekt na każdym poziomie. Powiązane: `NODE-MANAGEMENT.md` (§14 = kanał downlink),
> `CLAUDE.md` (long-term). [[nack-downlink-pending-commands]] [[near-term-roadmap]]

## 0. Zasady przekrojowe (każdy poziom)
- **A/B + rollback** wszędzie gdzie się da — nieudany update NIGDY nie brickuje, zwłaszcza zdalnego noda i bramki-huba.
- **Power-fail-safe + atomowo** — zapis do nieaktywnego slotu, atomowy flip wskaźnika.
- **Podpisane (ECDSA P-256)** — autentyczność; klucz prywatny na HSM, nigdy na urządzeniu.
- **Poufność end-to-end dla nodów** — obraz zaszyfrowany, deszyfrowany TYLKO wewnątrz noda → koniec sniffu RF → reverse-engineeringu.
- **Macierz kompatybilności** — protokół spina warstwy (§14 spina 4); bundle niesie wersje + reguły kolejności.
- **Bramka (Go) = orkiestrator** — ciągnie bundle, weryfikuje, aplikuje warstwa po warstwie.

---

## 1. OTA nodów (trudna część)

### 1.1 Układ: CC1312R (migracja z CC1310) — ZAKLEPANE
CC1310 (128 KB flash) **nie mieści A/B** (solar 73 KB, T&H 65 KB → 2×73 = 146 KB > 128 KB — zmierzone z map-plików). **CC1312R:** 352 KB flash, 80 KB RAM, Cortex-M4F, **HW AES-128/256 + ECC/RSA PKA + SHA2 + TRNG** (secure OAD w sprzęcie), **cap-sense + Sensor Controller** (idealne pod LightSwitch), **ten sam package 7×7 + layout RF** co CC1310, tańszy (~$3 vs $3.8), więcej stocku LCSC. Ten sam SimpleLink CC13x2 SDK; regeneracja `smartrf_settings` pod PHY bramki CC1310 → **mieszana flota** (CC1310 bramka + CC1310 stare nody + CC1312 nowe) gada. **R7 (704 KB) niepotrzebny** — R1 ma ogromny zapas. Nowe nody (switch, presence) = CC1312; stare CC1310 = bez field-OTA (rzadki reflash po kablu) albo rev na CC1312.

### 1.2 Layout flasha (352 KB)
`BIM (~30 KB, write-protected, root of trust)` + `Slot A (~130 KB)` + `Slot B (~130 KB)` + `NVS/metadata (~16 KB)` + `CCFG (ostatnia strona)`. ~306 KB użyte, ~46 KB zapasu.

### 1.3 Crypto + klucze
**Format obrazu (encrypt-then-sign):**
```
[ header: wersja, typ, rozmiar, flagi ]
[ ciphertext: AES-CCM(firmware) ]                        ← poufność + integralność per-blok
[ signature: ECDSA-P256(SHA256(header‖ciphertext)) ]     ← autentyczność producenta
```
Weryfikacja (ECDSA + hash) PRZED deszyfracją → fail-fast; weryfikator nie potrzebuje klucza deszyfrującego.

**Dwa klucze:**
- **Podpis (ECDSA):** para producenta. Publiczny wypalony w KAŻDYM nodzie (BIM). Prywatny na HSM, nigdy na urządzeniu.
- **Szyfr (AES): per-device root + klucz grupowy per-update** (wzorzec LoRaWAN FUOTA):
  - **Root per-device (stały):** `klucz = KDF(master_secret, factory_id)`. Master na HSM. Nod ma tylko swój; KDF jednokierunkowy → jedna kompromitacja ≠ master ≠ inne nody.
  - **Klucz GRUPOWY/sesji per-update (rotujący):** losowy na update, szyfruje broadcastowany obraz. Dostarczony każdemu nodowi **owinięty jego kluczem per-device** (malutki unicast). Nod odwija → deszyfruje broadcast.
  - Daje: broadcast (obraz raz) + rotujący klucz grupowy + bezpieczną rotację + ograniczoną szkodę. **Bramka trzyma ZERO sekretów** (relayuje ciphertext) → kompromitacja bramki nic nie wycieka.
- **Anti-rollback:** monotoniczny licznik wersji; nod odrzuca wersję ≤ aktualnej.

**Debug-lock (produkcja, OBOWIĄZKOWE — bez tego cała reszta fikcja):**
- CCFG: `CPU_DAP_ENABLE=0`, wszystkie test-TAP `=0`, `BOOTLOADER_ENABLE=0` → JTAG/cJTAG + ROM-bootloader zamknięte. Zablokowany chip: debugger nie odczyta, tylko mass-erase (kasuje sekret). Sektory BIM write-protected.
- **Uwaga:** CC13x2R = „secured MCU", NIE secure element. Zatrzymuje tanie ataki (JTAG dump, bootloader read). NIE zatrzymuje fault-injection / side-channel / decap-microprobe (flash niezaszyfrowany at-rest). Bardzo cenne IP → zewnętrzny SE (ATECC608/SE050). Per-device KDF ogranicza szkodę nawet fizycznego ataku (jeden nod, jeden update).

### 1.4 Transport (RF)
Trzy warstwy rozmiaru: chunk aplikacyjny (dowolny) / ramka RF (≤255 B, 1-bajtowa długość; realny limit z błędów bitowych + airtime) / ramka SPI (nasz wybór).
- **DECYZJA: podnieść ramkę SPI 128 B → 256 B** (stała) → RF (255 B) = SPI (256 B) **1:1** → M4F to czysty relay (bez składania), liczba ramek o połowę mniejsza (~325 vs 680 dla 73 KB). Koszt: +~1 ms SPI/mała wiadomość (pomijalne; podbić zegar SPI), **re-test stabilności collision-fixa**, +RAM. RF zostaje zmiennodługościowe (mała telemetria bez paddingu); tylko OAD używa pełnych 255 B.
- **ARQ: windowed broadcast** (burst okna ramek, bez ACK per ramka) + **poll każdego noda o bitmapę braków** (round-robin unicast, unika ACK-storm) + **retransmit UNII braków** + iteracja. Nod pisze każdą ramkę wprost do nieaktywnego slotu (flash = progres wznawiania). Deszyfracja per-blok AES-CCM przy odbiorze; **final ECDSA nad całym obrazem** przed swapem.
- **Broadcast floty:** obraz nadany RAZ dla N nodów tego typu → ~1,2-1,5× airtime jednego (nie N×). Maruder ze złym łączem → unicast-repair albo później.

### 1.5 Normy (EN 300 220, 868 MHz) — haczyk duty-cycle dla OAD
| Pasmo | Moc | Duty | TX/h |
|---|---|---|---|
| 868.0–868.6 (telemetria) | 25 mW | **1%** | ~36 s |
| **869.4–869.65 (OAD)** | **500 mW** | **10%** | **~360 s** |

OAD 73 KB = ~15-30 s TX bramki → **za ciasno na 1%** (wiele nodów/retry przekracza). **OAD → przełącz na 869.4-869.65 (10%)** — mieści się z zapasem. CC13xx przestraja trywialnie; „enter OAD" niesie kanał; retune → OAD → retune. Wymaga 2. `smartrf_settings` (kanał OAD). Alt: LBT+AFA. 500 mW wymaga **zewn. PA na bramce** (CC1312 sam = +14 dBm/25 mW) — opcjonalne (zasięg), nie konieczne dla 10% duty.

### 1.6 BIM + A/B (cykl życia per nod)
**BIM** = immutable root-of-trust (write-protected, **NIGDY nie OTA** — zrobić dobrze, zamrozić). Trzyma publiczny ECDSA + wybór slotu + verify. Na boocie: czyta nagłówki obu slotów, weryfikuje, wybiera valid+newest+confirmed, skacze (VTOR).
**Nagłówek slotu:** `{magic, wersja, rozmiar, SHA-256, podpis ECDSA, flagi VALID/CONFIRMED/PENDING, licznik prób}`.
**Cykl:** download→nieaktywny slot → verify (hash+podpis+wersja) → mark PENDING → reset → BIM bootuje PENDING (TRY, odnotowuje próbę) → nowy app self-check + **CONFIRM do bramki** → mark CONFIRMED → gotowe. Crash/no-confirm w oknie → **BIM wraca na drugi CONFIRMED slot**. Zero bricka.
**Uruchamianie z obu slotów:** (A) obraz relokowalny (BIM ustawia VTOR, jeden wariant) — elastyczne; lub **(B) dwa warianty A/B + lockstep** (broadcast wariantu pod nieaktywny slot, out-of-sync repair unicastem) — prostsze, pasuje do „flota update'owana razem". **Rekomendacja: B na teraz.**

### 1.7 Koordynacja broadcast + swap
Każdy nod swapuje **niezależnie** (jego BIM), ale bramka dyryguje w fazach:
1. **Dystrybucja:** broadcast + poll/retransmit aż WSZYSCY mają komplet (zweryfikowany, PENDING, nie zrebootowany).
2. **Commit:** bramka broadcastuje „commit/swap now" (ew. z timestampem) → wszyscy reboot ~razem → confirm.
3. **Maruderzy/porażki:** brak confirm → nod rolluje się sam (BIM); bramka retry indywidualnie albo zostawia na starym. **Rollback per-node.**
Nikt nie swapuje aż wszyscy MAJĄ obraz; minimalne okno mieszanych wersji; awaria jednego nie blokuje grupy.

---

## 2. OTA bramki (prostsza: osiągalna, zasobna, jedna sztuka)

### 2.1 Cała bramka = JEDNA jednostka RAUC
`rootfs + Go + M4F + gateway-CC1312` w JEDNYM podpisanym obrazie RAUC, po HTTPS (istniejąca infra + pinning). Swap całości (GB, ale download w tle w nocy). Bez broadcastu (jedna sztuka). Decyzje:
- **M4F: plik w rootfs, ładowany przez remoteproc** → jedzie z RAUC A/B. **Odrzucono OSPI A/B** (przerost formy; utrata niezależności M4F na zaplanowany reboot ~30-40 s (do 15-20 s po odchudzeniu) jest OK; recovery TI to i tak pełny reset).
- **Go: w rootfs** → A/B za darmo.
- Coupling Go↔M4F (`protocol.h`) rozwiązany: są w tym samym obrazie → atomowo.

### 2.2 RAUC A/B (rootfs)
Dwie partycje rootfs (A/B) + u-boot `bootcount`/`altbootcmd` (rollback). RAUC pisze nieaktywną → verify (hash+podpis) → try → reboot → zdrowy → `rauc mark-good` (health-check: Go serwuje, M4F żyje, CC RF up); N padów → `altbootcmd` wraca. **`/var/lib/bramka` (SQLite: config/nody/historia/klucze) na OSOBNEJ partycji** → przeżywa swap (NIE ruszać). Verdin: mechanizm Toradex (RAUC/Mender w BSP).

### 2.3 Gateway CC1312 — bundlowany, uproszczony (BIM + JEDEN slot, bez A/B)
W przeciwieństwie do zdalnych nodów, gateway CC jest **PO DRUCIE** (SPI z M4F) → **M4F jest jego recovery** (zawsze doflashuje) → **A/B niepotrzebne:**
- Gateway CC1312 = **BIM (immutable) + JEDEN slot app.** Golden image w rootfs (jak plik M4F) → wersja CC podąża za rootfs → **RAUC A/B na rootfs = rollback CC**.
- **Boot:** reboot w nowy rootfs → nowy M4F sprawdza wersję CC po SPI → mismatch → flashuje CC z golden w rootfs → CC BIM verify (podpis) → CC reboot → M4F confirm → serwuje → mark-good.
- **Rollback:** RAUC wraca na stary rootfs → stary M4F → CC mismatch → flashuje CC z powrotem na starą → spójny stary stack.
- **Power-fail w trakcie flashu CC:** slot app CC nieważny → BIM czeka → M4F doflashuje na następnym boocie. BIM immutable → brak bricka.
- **HW (carrier board): M4F GPIO → CC nRESET + boot-pin.**

### 2.4 Atomowy cutover
Stage wszystko (nowy rootfs z M4F+Go+CC-golden do nieaktywnej partycji, verify) → **jeden reboot flipuje cały stack** (u-boot → nowy rootfs → nowy M4F → flashuje CC pod siebie) → spójny nowy. Rollback flipuje wszystko z powrotem → spójny stary. **Bramka nigdy w niespójnym stanie protokołu.**

### 2.5 Poufność
Firmware bramki po HTTPS (szyfr w tranzycie) — **nie sniffowalny po RF** (obraz dla samej bramki, po IP). At-rest na eMMC opcjonalnie LUKS/dm-crypt (device-secret / AM62 key store) jeśli IP wrażliwe. Prościej niż nody. Ten sam podpis ECDSA weryfikuje bundle.

---

## 3. Orkiestracja + kolejność
Bramka (Go) dyryguje:
- **Stack bramki (rootfs+Go+M4F+gateway-CC) = jeden bundle RAUC, atomowy cutover.**
- **Nody = osobny RF OAD**, wg macierzy kompatybilności (bramka mówi wersją floty; wspiera N i N-1 podczas przejścia floty).
- Trigger: zaplanowany (noc) / z apki / auto z maintenance window.

---

## 4. Status + otwarte
- **Status: ZAPROJEKTOWANE, NIEzaimplementowane.** Wdrożyć gdy flota ma grupy same-type (broadcast się opłaca).
- **Otwarte decyzje (na czas implementacji):** relokowalny obraz vs dwa warianty (§1.6), rozmiar okna/bloku, dedykowany kanał OAD vs interleave, klucz-produktowy-MVP vs pełny per-device od startu (skłon: per-device), zewn. SE (tylko jeśli IP wymaga).
- **Prerekwizyt:** naprawa niezawodności radia CC1310/CC1312 („Kolizja/restart radia" backlog) — OAD amplifikuje problemy łącza.
- **Powiązany backlog:** szybki reset SAMEGO M4F (sekundy, bez pełnego reboota Linuxa) — optymalizacja na przyszłość, patrz CLAUDE.md.
