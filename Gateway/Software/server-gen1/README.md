# server-gen1 — skrypty na serwer gen1 (hosting)

Skrypty PHP wgrywane na serwer gen1 (`23202.m.tld.pl`, obok istniejących
`post-solar-data.php` itd.). Wersjonowane tu, bo są częścią systemu, choć żyją poza bramką.

## solar-export.php

Read-only endpoint JSON — gen2 dociąga z niego historię solara (2 lata) do własnej
`solar_history` z tagiem `source='gen1'`. Model: **gen1 = źródło prawdy**, wygrywa w każdej
godzinie, którą pokrywa (rollup na bramce sam preferuje gen1 → dziury z hangów gen2 znikają).

**Deploy:**
1. Zmień `EXPORT_KEY` na własny sekret (nie commituj prawdziwego).
2. `scp solar-export.php` na serwer, obok `post-solar-data.php`.
3. Test: `curl "https://23202.m.tld.pl/solar-export.php?since=0&limit=3&key=<KEY>"` → JSON 3 wierszy.

**Kontrakt:** `GET ?since=<unix_ts>&limit=<n>&key=<KEY>` → `[{ts,inputTemp,outputTemp,
bufor1..4Temp,collectorTemp,pwmValue,sPumpState,extraTemp}]`, ASC po `readingTime`, `ts>since`.

## Import na bramce

```bash
/opt/bramka/rpmsg-service/rpmsg-service -test import-gen1 \
  -db /var/lib/bramka/bramka.db \
  -gen1-url https://iot.aitronic.pl/solar-export.php \
  -gen1-key <KEY> \
  -gen1-insecure          # cert hosta nie pasuje do nazwy -> pomijamy walidacje TLS
```

**Cert TLS hosta gen1 nie pasuje do nazwy** (`SEC_E_WRONG_PRINCIPAL`). Opcje: `-gen1-insecure`
(pomija walidacje; dane solarne nietajne, operacja admina), albo `http://` (stary gen1 i tak
chodził po HTTP). curl do testu: `curl.exe -k "https://..."` lub `curl.exe "http://..."`.

- **Idempotentny**: wznawia od najnowszego wiersza gen1 już w bazie (`INSERT OR IGNORE`),
  więc kolejne uruchomienia dokładają tylko nowe rekordy. Można odpalać kiedy chcesz.
- **Nie rusza danych gen2** (live). Po imporcie przelicza rollup (`RebuildSolarRollup`).
- Rekonstruuje przyrost 2-min z narastającego `extraTemp` (spadek = reset doby).
- Nie wymaga M4F (czysta operacja DB+HTTP), można odpalić nawet gdy bramka nie gada z M4F.

Szczegóły mapowania kolumn (nazwy kłamią): [[gen1-server-scripts]] w pamięci.
