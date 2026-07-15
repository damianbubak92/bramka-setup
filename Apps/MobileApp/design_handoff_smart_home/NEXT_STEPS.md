# Notatka — plan i stan projektu

## Stan aktualny (najnowsza sesja) — wersja rozwijana dalej
Motyw **ciepły** ustalony jako główny (plik `Smart Home App.dc.html`):
- **Baza:** grafit `#201B13` = chrome (FAB, przycisk „Zapisz"), krem/biel = tła.
- **Pomarańcz `#E1850B`** używany oszczędnie tam, gdzie pasuje: aktywna pigułka nawigacji (lekki `#FDECCB`), aktywne segmenty tab-view (biała uniesiona pastylka z cieniem na ciemniejszym torze `#E8E1D4`), segmenty ON/OFF.
- **Zielony `#2E9E6B`** = przełączniki włącz/wyłącz + status online.
- **Kolor = typ urządzenia** (semantyczny akcent na kartach, pigułkach warunków/akcji).
- FAB grafitowy (pomarańczowy zbyt raził); „Zapisz" zawsze grafitowy; tab-view nie na wypełniony pomarańcz, tylko biała pastylka z cieniem.

### Dashboard
- Usunięte szybkie karty Światła/Rolety. Kolejność: najpierw **Bez pokoju** (System solarny, Czujnik klimatu, Fotowoltaika), potem **Pokoje**.
- Karta System solarny pod linią: **71,2°C Kolektor** (Tcol), **73,2°C Zbiornik główny** (T4), **62,3°C Zbiornik dodatkowy** (sBuforTemp).
- Fotowoltaika = pełna **czerwona** karta (bez czarnego tła), jako 3. w „Bez pokoju".

### Automatyzacje — karty reguł i podgląd
- Reguła czytana jako: **JEŚLI** + pigułki warunków (Czas = neutralny szary; parametr/delta = kolor urządzenia źródłowego) rozdzielone **„ORAZ"**, potem **TO** + pigułka akcji w kolorze urządzenia docelowego („Urządzenie · Przekaźnik ON/OFF").
- Podgląd reguły w edytorze używa tej samej konwencji pigułek (jasna karta, oś JEŚLI→TO).

## Warianty kolorystyczne (kopie w projekcie)
- `Smart Home App (pomarancz).dc.html` — pełny pomarańcz na akcentach chrome.
- `Smart Home App (turkus).dc.html` — turkus.
- `Smart Home App (grafit).dc.html` — czysta baza grafitowa (przed motywem ciepłym).
Zmiana motywu = podmiana jednej stałej koloru w skrypcie (proste).

## Do zrobienia dalej
- Ewentualne dostrojenie palety typów urządzeń (solar vs bufor mają zbliżony ciepły odcień — rozważyć mocniejsze rozróżnienie).
- Kolejne typy akcji automatyzacji (na razie tylko „Ustaw przekaźnik ON/OFF") i warunków — model jest rozszerzalny (`A_DEVICES`, `A_PARAMS`, `A_COLORS`, `DEV_TYPES`).

## Zrobione wcześniej
- **Stany puste** — Automatyzacje (ikona + tekst + „Nowa automatyzacja"); Urządzenia (brak urządzeń → podpowiedź JOIN; pusty filtr pokoju → „Pokaż wszystkie").
- **Potwierdzanie usuwania** — wspólny dialog (backdrop + karta) dla reguł i urządzeń.
- Ekrany: Dashboard, Czujnik klimatu (motyw turkus), System solarny (motyw pomarańcz, schemat instalacji + wykresy), Automatyzacje (lista + edytor + walidacja + podgląd + synchronizacja z bramką/offline), Menadżer urządzeń (JOIN, pokoje).

## Nazewnictwo
Pełnoekranowy kolorowy ekran urządzenia = „motyw urządzenia / color-flooded surface" (NIE „hero").
