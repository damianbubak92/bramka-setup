# server — skrypty PHP na hostingu

Wszystkie skrypty serwerowe gen2 w jednym miejscu (na hostingu i tak leżą w jednym
katalogu). Dwie bazy, **jeden `secrets.php`**:

| skrypt | baza | rola |
|---|---|---|
| `solar-export.php` (GET) | gen1 (`baza23202_esp32`) | import historii solara do bramki gen2 (**dev-only**, później wypadnie) |
| `gw-backup.php` (POST) | gen2 (`baza23202_gen2`) | live backup bramki (upsert/delete per wiersz) |
| `gw-restore.php` (GET) | gen2 | świeża bramka odtwarza się z mirrora |
| `schema.sql` | gen2 | tabele `gw_*` mirrora |

## Sekrety — `secrets.php` (gitignored)

Wszystkie skrypty robią `require __DIR__.'/secrets.php'`. Plik trzyma creds do **obu**
baz + oba klucze, i **nie trafia do repo** (`.gitignore`). Raz, na serwerze:
```bash
cp secrets.example.php secrets.php
nano secrets.php     # uzupełnij $GEN1_* + $EXPORT_KEY oraz $GW_* + $BACKUP_KEY
```
Na produkcji (gdy import gen1 już niepotrzebny): usuń blok `$GEN1_*`/`$EXPORT_KEY`
z `secrets.php` i skasuj `solar-export.php`.

## Deploy

1. Utwórz bazę gen2, odpal `schema.sql`.
2. `cp secrets.example.php secrets.php` + uzupełnij.
3. `scp *.php secrets.php` na serwer (secrets.php ręcznie — nie ma go w repo).
4. Testy:
   ```
   curl "http://host/gw-restore.php?key=BACKUP_KEY"        # -> {config:[],node:[],...}
   curl "http://host/solar-export.php?since=0&limit=3&key=EXPORT_KEY"
   ```

## Kontrakty

**gw-backup.php** (POST): `{"key","items":[{"kind","op":"upsert|delete","data":{...}}]}` →
`{"ok":true,"applied":N}`. `kind` ∈ config|node|node_param|solar_hourly|solar_daily|solar_monthly (+ `purge_node`).

**gw-restore.php** (GET `?key=`): wszystkie tabele `gw_*` jako JSON.

**solar-export.php** (GET `?since=&limit=&key=`): surowa telemetria gen1 (`extraTemp`/`pumpRuntime` kumulatywnie).

## Bramka

Backup: `backup.go` — triggery SQLite → `backup_queue` → worker z retry (`-backup-url`/`-backup-key`).
Restore: `-test restore -restore-url http://host/gw-restore.php -backup-key ...`.
Import gen1: `-test import-gen1 -gen1-url http://host/solar-export.php -gen1-key ...`.
Szczegóły: [[gen1-server-scripts]], [[gen2-backup-mirror]].
