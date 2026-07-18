<?php
// secrets.example.php - TEMPLATE (committed). Copy to secrets.php on the server and
// fill in real values. secrets.php is gitignored, so it never reaches the repo.
//
//   cp secrets.example.php secrets.php   &&   edit secrets.php
//
// One file, two databases - every script here requires it:
//   - solar-export.php            -> gen1 DB  ($GEN1_* + $EXPORT_KEY)
//   - gw-backup.php / gw-restore.php -> gen2 DB ($GW_*  + $BACKUP_KEY)

// --- gen1 (historical solar import; DEV ONLY - delete this block on production) ---
$GEN1_DB_HOST = '23202.m.tld.pl';
$GEN1_DB_NAME = 'baza23202_esp32';
$GEN1_DB_USER = 'CHANGE_ME_user';
$GEN1_DB_PASS = 'CHANGE_ME_password';
$EXPORT_KEY   = 'CHANGE_ME_export_key';       // must match the gateway's -gen1-key

// --- gen2 (gateway backup mirror; permanent) ---
$GW_DB_HOST = 'localhost';
$GW_DB_NAME = 'baza23202_gen2';               // separate gen2 DB
$GW_DB_USER = 'CHANGE_ME_user';
$GW_DB_PASS = 'CHANGE_ME_password';
$BACKUP_KEY = 'CHANGE_ME_long_random_secret'; // must match the gateway's -backup-key
