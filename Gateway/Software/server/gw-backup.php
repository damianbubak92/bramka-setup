<?php
// gw-backup.php - the gateway pushes config/state/history changes here (live backup).
// Body: {"key":"<secret>","items":[{"kind":"<table>","op":"upsert|delete","data":{...}}]}
//   kind  = which mirror table (whitelist below)
//   op    = upsert (INSERT ... ON DUPLICATE KEY UPDATE) | delete (by primary key)
//           plus a special kind "purge_node" that wipes one node across all tables
//   data  = the row's columns (upsert) or just the PK columns (delete/purge)
// Response: {"ok":true,"applied":N}. Key-gated, prepared statements only.
//
// Deploy on the gen2 server DB (separate from gen1). FILL IN creds; do NOT commit them.

header('Content-Type: application/json; charset=utf-8');

// Credentials + key live in secrets.php (gitignored). Copy secrets.example.php ->
// secrets.php on the server and fill it in. Never commit secrets.php.
require __DIR__ . '/secrets.php';

// kind -> [table, [pk columns], [all columns]]
$SCHEMA = [
  'config'        => ['gw_config',        ['key'],            ['key','value']],
  'node'          => ['gw_node',          ['node_id'],        ['node_id','address','node_type','name','factory_id','status','provisioned_at','last_seen','room','archived_at']],
  'node_param'    => ['gw_node_param',     ['node_id','param_key'], ['node_id','param_key','value_num','ts']],
  'solar_hourly'  => ['gw_solar_hourly',  ['node_id','bucket'], ['node_id','bucket','hour_yield','hour_pump','day_yield','day_pump']],
  'solar_daily'   => ['gw_solar_daily',   ['node_id','bucket'], ['node_id','bucket','day_yield','month_yield','month_pump']],
  'solar_monthly' => ['gw_solar_monthly', ['node_id','bucket'], ['node_id','bucket','month_yield','year_yield','year_pump']],
];

$body = json_decode(file_get_contents('php://input'), true);
if (!is_array($body) || ($body['key'] ?? '') !== $BACKUP_KEY) {
    http_response_code(401);
    echo json_encode(['ok' => false, 'error' => 'bad key']);
    exit;
}
$items = $body['items'] ?? [];
if (!is_array($items)) { $items = []; }

$conn = @new mysqli($GW_DB_HOST, $GW_DB_USER, $GW_DB_PASS, $GW_DB_NAME);
if ($conn->connect_error) {
    http_response_code(500);
    echo json_encode(['ok' => false, 'error' => 'db connect']);
    exit;
}
// Force mysqli to THROW on any query/execute error (PHP < 8.1 otherwise returns
// false silently -> a bad row would be skipped and the server would falsely report
// ok). With this, every failure lands in the catch and returns the real cause.
mysqli_report(MYSQLI_REPORT_ERROR | MYSQLI_REPORT_STRICT);

$applied = 0;
$conn->begin_transaction();
try {
    foreach ($items as $it) {
        $kind = $it['kind'] ?? '';
        $op   = $it['op']   ?? 'upsert';
        $data = $it['data'] ?? [];

        // Special: soft-delete one node (node removal on the gateway). The row + its
        // history STAY (trash); only archived_at is set. A server cron purges rows past
        // the retention window (60d). Restore = the gateway re-inserts the node, whose
        // upsert carries archived_at=NULL and clears this.
        if ($kind === 'archive_node') {
            $nid = (int)($data['node_id'] ?? -1);
            $at  = (int)($data['archived_at'] ?? time());
            $s = $conn->prepare("UPDATE gw_node SET archived_at = ? WHERE node_id = ?");
            $s->bind_param('ii', $at, $nid); $s->execute(); $s->close();
            $applied++;
            continue;
        }

        // Special: hard-wipe one node everywhere (kept for disaster/manual use; the
        // gateway itself uses archive_node now, and the retention cron does the real
        // purge server-side).
        if ($kind === 'purge_node') {
            $nid = (int)($data['node_id'] ?? -1);
            foreach (['gw_node','gw_node_param','gw_solar_hourly','gw_solar_daily','gw_solar_monthly'] as $t) {
                $s = $conn->prepare("DELETE FROM $t WHERE node_id = ?");
                $s->bind_param('i', $nid); $s->execute(); $s->close();
            }
            $applied++;
            continue;
        }

        if (!isset($SCHEMA[$kind])) { continue; }         // ignore unknown kinds
        [$table, $pk, $cols] = $SCHEMA[$kind];

        if ($op === 'delete') {
            $where = implode(' AND ', array_map(fn($c) => "`$c` = ?", $pk));
            $s = $conn->prepare("DELETE FROM $table WHERE $where");
            bindDynamic($s, array_map(fn($c) => $data[$c] ?? null, $pk));
            $s->execute(); $s->close();
            $applied++;
            continue;
        }

        // upsert
        $colList = implode(',', array_map(fn($c) => "`$c`", $cols));
        $ph      = implode(',', array_fill(0, count($cols), '?'));
        $upd     = implode(',', array_map(fn($c) => "`$c`=VALUES(`$c`)", $cols));
        $s = $conn->prepare("INSERT INTO $table ($colList) VALUES ($ph) ON DUPLICATE KEY UPDATE $upd");
        bindDynamic($s, array_map(fn($c) => $data[$c] ?? null, $cols));
        $s->execute(); $s->close();
        $applied++;
    }
    $conn->commit();
} catch (Throwable $e) {
    $conn->rollback();
    http_response_code(500);
    // Return the real error - key-gated endpoint, so it only reaches the gateway.
    echo json_encode(['ok' => false, 'error' => $e->getMessage(), 'mysql' => $conn->error,
                      'last_kind' => $kind ?? null]);
    exit;
}
$conn->close();
echo json_encode(['ok' => true, 'applied' => $applied]);

// bindDynamic: bind a variable-length arg list, typing ints as 'i', doubles 'd', else 's'.
function bindDynamic(mysqli_stmt $s, array $vals): void {
    if (!$vals) return;
    $types = '';
    foreach ($vals as $v) {
        if (is_int($v))        $types .= 'i';
        elseif (is_float($v))  $types .= 'd';
        else                   $types .= 's';
    }
    $s->bind_param($types, ...$vals);
}
