# Handoff: Smart Home — Dashboard, System solarny, Czujnik klimatu

## Overview
Dashboard aplikacji do własnego systemu smart home (Android). Zawiera 3 okna:
1. **Dashboard** — ekran główny na żywo: szybkie sterowanie, kafle urządzeń „bez pokoju", pokoje, fotowoltaika.
2. **Czujnik klimatu** — szczegóły node'a bateryjnego mierzącego temperaturę i wilgotność (odczyty, wykresy 24 h / 7 dni / miesiąc / rok, ustawienie interwału pomiaru).
3. **System solarny** — schemat instalacji solarnej (kolektor + 2 zbiorniki + 2 pompy) z animacją pracy pomp oraz wykresy uzysku energii z zakresami dzień/miesiąc/rok/całkowite.

Docelowa platforma: **Android, Material Design**. Odbiorca: użytkownik domowy, każda grupa wiekowa.

## About the Design Files
Plik w tym pakiecie (`Smart Home App.dc.html`) to **referencja projektowa stworzona w HTML** — prototyp pokazujący docelowy wygląd i zachowanie, **a nie kod produkcyjny do skopiowania**. Zadaniem jest **odtworzenie tych ekranów w docelowym środowisku** (rekomendacja: Android + Jetpack Compose; alternatywnie Views/XML jeśli taka jest istniejąca apka) z użyciem natywnych komponentów i wzorców Material 3.

Mapa technologii (Compose): dolna nawigacja → `NavigationBar` + `NavHost`; karty → `Card`/`ElevatedCard` w `LazyColumn`/`LazyVerticalGrid`; wykresy → `Canvas` (opisane niżej — geometria gotowa) lub biblioteka typu Vico; schemat solarny → `Canvas` + animacja `rememberInfiniteTransition`; suwak → `Slider`; przełącznik → `Switch`; zapis ustawień → `DataStore`.

## Fidelity
**High-fidelity (hifi).** Finalne kolory, typografia, odstępy, animacje i interakcje. UI należy odtworzyć wiernie, używając bibliotek i wzorców docelowego kodu. Wartości liczbowe (temperatury, kWh, %) są przykładowe — docelowo pochodzą z bramki smart home / node'ów.

---

## Design Tokens

### Kolory — bazowe (ekran jasny / Dashboard)
| Rola | Hex |
|---|---|
| Tło aplikacji (ciepła biel) | `#F7F4EF` |
| Karta / powierzchnia | `#FFFFFF` |
| Tekst główny | `#201B13` |
| Tekst drugorzędny | `#6B675E` |
| Tekst wyciszony | `#8A857B` |
| Linia/separator | `#ECE6DA` |
| Aktywna pigułka nawigacji (tło) | `#FDECCB` |

### Kolory — semantyczne (kolor = charakter urządzenia)
| Domena | Akcent | Tło ikony (jasne) |
|---|---|---|
| System solarny / energia słońca | `#F6A21E` (głęboki `#E1850B`) | `#FDECCB` |
| Oświetlenie | `#C99400` (toggle ON `#E0A800`) | `#FBEFC7` |
| Ogrzewanie / termostat | `#D9542B` | `#FBE0D5` |
| Rolety | `#2F8F83` | `#D8ECE8` |
| Fotowoltaika (prąd) | `#2F6FB0` (na ciemnym `#6FA8DC`) | `#DCE9F6` |
| Klimat (temp+wilgotność) | `#0E7E95` | `#DBEEF4` |
| Status online / pompa pracuje | `#2E9E6B` | — |

### Kolory — ekrany szczegółów (pełny ekran, tekst biały `#FFFFFF`)
- **Czujnik klimatu**: tło jednolite `#0E7E95` (turkus). Kafel na dashboardzie: gradient `135deg, #22B0C6 → #0E7E95`.
- **System solarny**: tło jednolite `#E1850B` (pomarańcz). Kafel na dashboardzie: gradient `135deg, #F6A21E → #E1850B`.
- Na tych ekranach: separatory `rgba(255,255,255,.2)`; pigułka aktywna = białe tło + tekst w kolorze tła ekranu; elementy nieaktywne `rgba(255,255,255,.75)`; siatki wykresu `rgba(255,255,255,.16)`; etykiety osi `rgba(255,255,255,.7)`.

### Typografia
- Rodzina: **Roboto** (Google Font). Waga 200 i 300 dla dużych liczb, 400 tekst, 500/600 etykiety/nagłówki, 700 tytuły. `Roboto Mono` sporadycznie dla wartości technicznych.
- Duża wartość (np. `2,84 kW`, `21,8°C`): `font-weight 200`, `~46px`, `line-height 1`; jednostka `20px`, waga 400.
- Nagłówek ekranu (top bar): `500 20px`.
- Etykieta sekcji: `400 13px`, `opacity .8`.
- Wartość statystyki (runtime/yield): `300 26px`.
- Etykieta drugorzędna: `400 12–13px`.

### Promienie / cienie / odstępy
- Promienie: karty `20–26px`, mniejsze kafle `18–22px`, pigułki/taby `13–18px`, pełne pigułki/toggle `okrągłe`.
- Cień karty (jasny ekran): `0 2px 10px rgba(80,60,20,.06)`; hero/akcent: `0 14px 30px rgba(225,133,11,.32)` (solar) / `rgba(14,126,149,.30)` (klimat).
- Padding ekranu: `20–24px` po bokach. Odstępy między kartami: `12–14px`.
- Minimalny hit-target interaktywnych elementów: `44px`.

### Ramka urządzenia (tylko makieta)
Telefon: szerokość ekranu wewn. **398px**, wysokość **812px**, promień ~37px. Status bar: `20:34`, ikony + `51%`. Dolny pasek gestów (pigułka). W realnej apce to natywny chrome systemu — nie odwzorowywać.

---

## Screen 1 — Dashboard (ekran główny, tło `#F7F4EF`)

**Cel:** podgląd na żywo i szybkie sterowanie.

**Układ (pionowy scroll):**
1. **Nagłówek**: „Dzień dobry, Piotr" (`14px`, muted) + „Dom · Wszystko OK" (`500 26px`, `#201B13`); po prawej okrągły awatar 44px (tło `#EFE7D8`).
2. **Szybkie sterowanie** — 2 karty w rzędzie (`flex, gap 12`), każda białe tło, radius 22, padding 14:
   - **Światła**: ikona żarówki (`#C99400` na `#FBEFC7`, chip 40px r14), tytuł „Światła", podpis „3 włączone", **toggle ON** (tor `#E0A800`, 38×22).
   - **Rolety**: ikona (`#2F8F83` na `#D8ECE8`), „Rolety", „Otwarte 60%", **toggle OFF** (tor `#D9D3C7`).
3. **Sekcja „BEZ POKOJU"** (etykieta uppercase, muted) — urządzenia nieprzypisane do pokoju:
   - **Kafel „System solarny"** — gradient amber, biały tekst; badge „Sieć" (ikona wtyczki, tło `rgba(255,255,255,.2)`); duża wartość `23,7 kWh` + „Uzysk dzienny"; mini-wykres słupkowy (8 słupków, biały); wiersz: `73,2°C Zbiornik T4 · 62,3°C Bufor · 10h34 Praca pompy`. **Klik → ekran System solarny.**
   - **Kafel „Temperatura i wilgotność"** — gradient turkus, biały tekst; ikona termometr+kropla; badge baterii `87%`; dwie wartości: `21,8°C Temperatura`, `48% Wilgotność`; stopka „Pomiar 2 min temu · interwał 5 min". **Klik → ekran Czujnik klimatu.**
4. **„Pokoje"** — nagłówek + link „Wszystkie" (`#E1850B`); grid 2 kolumny:
   - **Salon**: ikona termostatu `#D9542B`, `22,4°`, „Grzeje · 3 światła".
   - **Sypialnia**: ikona termostatu `#D9542B`, `19,8°`, „Eco · rolety zam.".
5. **Pasek Fotowoltaiki** — ciemna karta (`#201B13`, radius 20): ikona pioruna `#6FA8DC` na `rgba(47,111,176,.20)`, „Fotowoltaika / Produkcja teraz"; po prawej `2,84 kW` (`#6FA8DC`) + „z 6,0 kWp".
6. **Dolna nawigacja** (białe tło, górna linia `#ECE6DA`): **Dashboard** (aktywny — ikona domu, pigułka `#FDECCB`), **Automatyzacje** (ikona pioruna), **Urządzenia** (ikona monitora). Etykiety `11px`. Pod nią pigułka gestów.

> Uwaga: „Automatyzacje" i „Urządzenia" to kolejne destynacje nawigacji (istnieją w prototypie, poza zakresem tego handoffu — do dokumentacji w następnej iteracji).

---

## Screen 2 — Czujnik klimatu (pełny ekran, tło `#0E7E95`, tekst biały)

Otwierany z kafla „Temperatura i wilgotność". Górny pasek: strzałka wstecz (←) + ikona termometr+kropla + „Czujnik klimatu" (`500 20px`). Status bar biały. Trzy sekcje oddzielone liniami `rgba(255,255,255,.2)`:

**Sekcja 1 — Aktualny pomiar**
- Etykieta „Aktualny pomiar" (`400 13px`, opacity .8); po prawej badge baterii `87%`.
- Dwie duże wartości obok siebie: `21,8°C` i `48%` (`font-weight 200, 46px`), pod nimi „Temperatura" / „Wilgotność".
- Stopka z ikoną zegara: „Pomiar 2 min temu · interwał {N} min" (N = aktualny zapisany interwał).

**Sekcja 2 — Dane historyczne**
- Segmentowany przełącznik metryki: **Temperatura | Wilgotność** (aktywny: białe tło, tekst `#0E7E95`).
- Pigułki zakresu: **24 h · 7 dni · Miesiąc · Rok** (aktywna biała).
- Wykres liniowy z wypełnieniem (area `rgba(255,255,255,.14)`, linia biała `2.5px`):
  - Oś Y: 3 etykiety (góra/środek/dół) z jednostką `°C` (temperatura) lub `%` (wilgotność).
  - Dla **temperatury**: pozioma **linia przerywana „zadana 22°C"** (`rgba(255,255,255,.55)`, dash `5 4`). Dla wilgotności ukryta.
  - Oś X: etykiety zależne od zakresu — 24 h: `0:00, 3:00 … 24:00`; 7 dni: `Pn…Nd`; miesiąc: `1,5,10…30`; rok: `Sty,Kwi,Lip,Paź,Gru`.
- Dane wykresu (przykładowe serie hardkodowane w prototypie): patrz `CH_DATA` w pliku.

**Sekcja 3 — Interwał pomiaru**
- Etykieta „Interwał pomiaru" + opis „Ustaw jak często dokonywać pomiaru".
- Duża wartość `{N}` + „min" (`200 44px`).
- **Slider** 1–5, krok 1 (biały uchwyt r14, tor białe wypełnienie do wartości, reszta `rgba(255,255,255,.28)`). Pod nim znaczniki `1 2 3 4 5`.
- **Przycisk „Zapisz interwał"** (białe tło, tekst `#0E7E95`) — **widoczny tylko gdy wartość zmieniona** względem zapisanej.
- Po zapisaniu: przycisk znika, pojawia się potwierdzenie „**Zapisano ✓**" (tło `rgba(255,255,255,.18)`), które **samo znika po ~2,6 s** z animacją fade (`@keyframes`: 0% opacity0/translateY6px → 12% widoczne → 78% widoczne → 100% opacity0).
- Wartość jest **trwała** (w prototypie `localStorage['climateInterval']`; docelowo `DataStore`).

---

## Screen 3 — System solarny (pełny ekran, tło `#E1850B`, tekst biały)

Otwierany z kafla „System solarny". Górny pasek: ← + ikona słońca + „System solarny". Dwie sekcje.

**Sekcja 1 — Aktualnie generowana moc + schemat**
- Wiersz: po lewej etykieta „Aktualnie generowana moc" + duża `2,84 kW` (`200 46px`); po prawej kolumna: podpis „Pompa dodatkowa", pod nim etykieta stanu **`ON`/`OFF`** (`600 14px`) + **duży przełącznik** (tor 64×36, uchwyt 28px; ON: biały tor + uchwyt `#E1850B`; OFF: tor `rgba(255,255,255,.30)` + biały uchwyt). Steruje drugą pompą (kolektor↔zbiornik / sBuf).
- **Schemat instalacji** (SVG, białe elementy na pomarańczowym). Układ poziomy, lewa→prawa:
  - **Kolektor słoneczny (heat-pipe)**: górny kolektor zbiorczy (belka), pod nią 10 pionowych rurek próżniowych, cienka dolna poprzeczka. Nad kolektorem wartość **Tcol `71,2°C`**, pod spodem podpis „Kolektor".
  - **Zbiornik główny** (duży, wyższy): 4 wartości temperatur od góry: `73,2°C, 64,1°C, 58,1°C, 51,5°C` (bez etykiet T1–T4 — pozycja = poziom). Podpis „Zbiornik główny".
  - **Zbiornik dodatkowy** (mniejszy): wartość `62,3°C`. Podpis „Zbiornik dodatkowy".
  - **Rury**: między kolektorem a zbiornikiem głównym oraz między głównym a dodatkowym — **po dwie rury** (górna „gorąca" = oś na wysokości kolektora zbiorczego; dolna „powrót" = oś na wysokości dolnej poprzeczki). Grubość ~5px, `rgba(255,255,255,.4)`.
  - **Pompy** (na górnych rurach): symbol inżynierski **trójkąt w kółku** (białe kółko, pomarańczowy trójkąt wpisany). **Praca = trójkąt obraca się wokół środka ciężkości** (`@keyframes spin`, ~1.15 s, `transform-box: fill-box`, `transform-origin` w centroidzie). Pompa kolektora: podpis „78%" (pompa o zmiennej prędkości), zawsze pracuje w tym stanie. Pompa dodatkowa: pracuje/stoi zależnie od przełącznika (stój = trójkąt nieruchomy, przygaszony `#C77D3A`).
  - Geometria SVG (viewBox `0 0 344 224`) jest w metodzie `buildSolarSchema()` — do przeniesienia na `Canvas`.

**Sekcja 2 — Uzyski energii**
- Etykieta sekcji „Uzyski energii".
- **Taby (wyśrodkowane)**: **Dzień · Miesiąc · Rok · Całkowite** (aktywny: biała pigułka, tekst `#E1850B`).
- **Nawigator okresu**: `‹` [etykieta okresu, np. „12 lip 2026"] `›` (okrągłe przyciski `rgba(255,255,255,.14)`). Zmienia oglądany dzień/miesiąc/rok w obrębie wybranego zakresu.
- **Wykres słupkowy** (słupki kremowe/białe `rgba(255,255,255,.72)`, najwyższy `#fff`; oś Y z jednostką kWh; oś X zależna od zakresu).
- **Podsumowanie** (górna linia separatora): po lewej „Czas pracy pompy" + wartość (`300 26px`); po prawej **wyrównane do prawej** „Uzysk energii" + wartość. **Oba synchronizują się z wybranym okresem** i zmieniają przy zmianie tabu/nawigacji.
- Dane per okres (przykładowe): patrz `SOLAR` w pliku (`day/month/year/total`, każdy z `bars`, `label`, `run`, `yield`).

---

## Interactions & Behavior
- **Nawigacja**: kafle „System solarny" i „Temperatura i wilgotność" na dashboardzie otwierają pełnoekranowe szczegóły; strzałka wstecz wraca. Dolna nawigacja przełącza Dashboard / Automatyzacje / Urządzenia (na ekranach szczegółów status bar jest biały, dolna nawigacja ukryta).
- **Toggle Pompa dodatkowa**: natychmiast zmienia stan pompy na schemacie (obrót/stop + kolor + etykieta ON/OFF).
- **Czujnik klimatu**: zmiana metryki/zakresu przerysowuje wykres; slider zmienia wartość na żywo; „Zapisz" pojawia się tylko po zmianie; po zapisie toast „Zapisano ✓" z auto-zanikiem ~2,6 s; wartość trwała.
- **System solarny**: taby + nawigator zmieniają wykres oraz zsynchronizowane „Czas pracy pompy" i „Uzysk energii".
- **Animacje**: obrót trójkątów pomp (spin, liniowy, ciągły); fade toastu; przejścia toggle/slider (`transition .2s`).

## State Management (stan potrzebny w apce)
- `activeTab`: dashboard | automatyzacje | urządzenia
- `openDetail`: none | climate | solar (który pełny ekran otwarty)
- Klimat: `metric` (temp|hum), `range` (24h|7d|month|year), `interval` (1–5), `savedInterval`, `toastVisible`
- Solar: `solarTab` (day|month|year|total), `periodIndex` (nawigator okresu), `sbufPumpOn` (bool)
- Dane realne (docelowo z bramki): temperatury zbiorników/kolektora, stany i % pomp, generowana moc, serie historyczne temp/wilgotności i uzysku, czas pracy pompy, uzysk per okres, stan baterii node'a i timestamp pomiaru.

## Assets
Brak plików graficznych. Wszystkie ikony i wykresy są rysowane inline (SVG/ścieżki) — w apce użyć **Material Symbols** / `Icons` i natywnego rysowania (`Canvas`). Ikony użyte: dom, piorun (PV/automatyzacje), monitor (urządzenia), żarówka, rolety, termometr, termometr+kropla, słońce, wtyczka, zegar, bateria, chevrony.

## Files
- `Smart Home App.dc.html` — kompletny prototyp wszystkich ekranów (+ nawigacja). Logika (dane wykresów `CH_DATA`, `SOLAR`, geometria schematu `buildSolarSchema`, wykresy `buildChart`/`buildBar`, automatyzacje `A_RULES`/`buildAuto`, urządzenia `A_DEVLIST`/`DEV_TYPES`/`buildDevices`) znajduje się w bloku `<script data-dc-script>` na końcu pliku — to najlepsze źródło dokładnych danych i geometrii.
- `NEXT_STEPS.md` — plan kolejnych iteracji (eksperyment z jednolitym motywem, stany puste, potwierdzanie usuwania).

---

## Screen 4 — Automatyzacje (lista, tło `#F7F4EF`)

Trzeci filar nawigacji. **Chrome neutralny (grafit `#201B13`)**; kolor akcentu dziedziczy się z **typu urządzenia docelowego** reguły.

**Lista:**
- Nagłówek „Automatyzacje" + wiersz statusu połączenia: **zielona kropka = „Połączono na żywo · N reguł, M aktywne"**, czerwona = „Brak połączenia z bramką" (dotknięcie w prototypie przełącza stan — realnie steruje tym WebSocket).
- Karta reguły: ikona akcji (kolor typu urządzenia docelowego), nazwa, streszczenie warunków, chip akcji „Urządzenie · Przekaźnik ON/OFF", przełącznik aktywności (**zielony = aktywna**, wyłączona = wyszarzona karta), akcje Edytuj / Usuń.
- Stan synchronizacji per reguła: spinner „Zapisywanie do bramki…" po zmianie; „Niezsynchronizowana · Ponów" (czerwony) gdy offline.
- **FAB „Nowa"** (grafit) → edytor.

**Model danych (3 typy warunków + 1 typ akcji, rozszerzalny):**
- Warunek **time**: `{ start, end }` (HH:MM).
- Warunek **param**: `{ device, param, op: gt|lt, value }`.
- Warunek **delta**: `{ device1, param1, device2, param2, op, min }` (różnica dwóch parametrów).
- Akcja: `{ target (urządzenie), type: relay, value: 0|1 }` (Ustaw przekaźnik ON/OFF).
- Reguła = wiele warunków połączonych logicznym **ORAZ** (wszystkie muszą być spełnione).

**Synchronizacja (WebSocket, realtime):** auto-zapis po zatwierdzeniu w edytorze i po przełączeniu — bez osobnego „Wyślij". Cykl: mutacja → `syncing`; ACK z bramki → `ok`; timeout/rozłączenie → `error`; reconnect → automatyczny retry wszystkich `error`. Optimistic UI.

## Screen 5 — Edytor automatyzacji (tło `#F7F4EF`)

- **Nazwa** (pole tekstowe).
- **Warunki** — lista kart, każda z segmentowanym przełącznikiem typu **Czas / Parametr / Delta** (aktywny = grafit); pola zależne od typu (time-pickery; wybory urządzenia/parametru przez bottom-sheet; operator Większe/Mniejsze niż; wartość/różnica liczbowa). Dodawanie/usuwanie warunków.
- **Walidacja**: błędny warunek → czerwona ramka + komunikat (pusta/niepoprawna wartość, ujemna różnica, identyczne parametry w Delcie, te same godziny). Zapis zablokowany (przycisk wyszarzony) dopóki są błędy.
- **Podgląd reguły** — jasna karta z osią **JEŚLI → TO**: warunki (z etykietą „ORAZ") i akcja z kolorową ikoną urządzenia + plakietka „Przekaźnik ON/OFF". Aktualizuje się na żywo.
- **Akcja**: urządzenie docelowe (bottom-sheet), typ „Ustaw przekaźnik", stan ON/OFF (segment).
- Footer: Anuluj / Zapisz.

## Screen 6 — Urządzenia (menadżer, tło `#F7F4EF`)

- Nagłówek + licznik „N urządzeń · M online".
- **Chipy filtra pokoi** (Wszystkie + istniejące pokoje; aktywny = grafit).
- Lista **pogrupowana po pokojach**; wiersz urządzenia: ikona w kolorze typu, nazwa, „Typ · online/offline", kropka statusu (zielona/czerwona), strzałka. Ten sam stan synchronizacji co reguły.
- **Dodawanie = przez JOIN**, nie z aplikacji: użytkownik wciska JOIN na urządzeniu → bramka przez WebSocket wypycha zdarzenie → aplikacja **sama otwiera edytor** z **typem wypełnionym i zablokowanym** (wykryty automatycznie, z kłódką). Użytkownik ustala tylko **nazwę** i opcjonalnie **pokój**; Zapisz / Odrzuć. (W prototypie odpala to przycisk „Symuluj urządzenie w trybie parowania (JOIN)".)
- **Edytor urządzenia**: podgląd ikony w kolorze typu, nazwa, typ (zablokowany), pokój (bottom-sheet), Usuń urządzenie (przy edycji).
- **Zarządzanie pokojami** — w bottom-sheet „Pokój": kosz przy każdym pokoju (poza stałym „Bez pokoju"), pole „Nazwa nowego pokoju" + „Dodaj". Usunięcie pokoju przenosi jego urządzenia do „Bez pokoju". Urządzenie „Bez pokoju" pokazuje się na Dashboardzie; przypisane do pokoju widać po wejściu w pokój.

## Typy urządzeń i kolory (semantyczne)
> **Aktualizacja motywu (najnowsza wersja):** apka używa **ciepłego** motywu — grafit = chrome (FAB, „Zapisz"), pomarańcz `#E1850B` oszczędnie na akcentach (aktywna pigułka nawigacji `#FDECCB`, aktywne segmenty tab-view = biała uniesiona pastylka z cieniem na torze `#E8E1D4`, segmenty ON/OFF), zielony `#2E9E6B` = przełączniki/status online. Karty reguł i podgląd czytają regułę jako pigułki: **JEŚLI** (Czas neutralny, parametry w kolorze urządzenia) „ORAZ" … **TO** pigułka akcji. Szczegóły i warianty (pomarańcz/turkus/grafit) w `NEXT_STEPS.md`.

Kolor = typ urządzenia (stała mapa w kodzie: `A_COLORS`):
- solar `#E1850B`, buffer `#A15C2B`, pv `#C0392B`, climate `#0E7E95`, light `#C99400`, blind `#2F8F83`, heating `#D9542B`, hub `#2E9E6B`.
Baza aplikacji: grafit `#201B13` (chrome/nawigacja/FAB/przyciski), zielony `#2E9E6B` (stany włącz/wyłącz, status online).

## Uwaga o kierunku wizualnym
Obecnie: **grafit = chrome, zielony = stany, kolor = typ urządzenia**. Pełnoekranowe ekrany szczegółów (Solar/Klimat) używają **motywu urządzenia** (całe tło w kolorze tożsamości, biała treść) — nazywane „color-flooded / device-themed surface" (NIE „hero"). Planowany eksperyment (patrz `NEXT_STEPS.md`): wariant z jednolitym motywem marki (pomarańcz lub turkus) do porównania.

## Nowy stan aplikacji (poza wcześniejszym)
- Auto: `rules[]` (z `_sync`), `autoView` (list|editor), `editing`, `picker`, `online`.
- Urządzenia: `devices[]` (z `_sync`), `rooms[]`, `devView` (list|editor), `devEditing`, `devFilter`.
- Wspólny: `confirm` (dialog usuwania: `{ title, msg, label, onOk }`).

## Stany puste (empty states)
- **Automatyzacje** — gdy brak reguł: ikona pioruna (bursztyn) + „Brak automatyzacji" + opis + przycisk „Nowa automatyzacja" (grafit).
- **Urządzenia** — gdy brak urządzeń: ikona + „Brak urządzeń" + podpowiedź o dodawaniu przez JOIN. Osobny stan gdy aktywny filtr pokoju nie ma urządzeń: „Brak urządzeń w tym pokoju" + „Pokaż wszystkie".

## Potwierdzanie usuwania (dialog)
Wspólny modal (backdrop `rgba(32,27,19,.45)` + biała karta wyśrodkowana): ikona kosza, tytuł, opis z nazwą obiektu, przyciski **Anuluj** (outline) / **Usuń** (czerwony `#C0392B`). Kliknięcie w tło = anuluj. Używany przy usuwaniu reguły automatyzacji (z listy) i urządzenia (z edytora). Usunięcie urządzenia ostrzega, że powiązane automatyzacje mogą przestać działać.
