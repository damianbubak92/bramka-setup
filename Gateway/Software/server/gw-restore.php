<?php
// gw-restore.php - the gateway pulls state back from the mirror. Three modes:
//   (default)          full rebuild after a hardware failure - LIVE nodes only
//                      (trash is skipped), plus rules + current state + solar history.
//   ?archived=1        the trash: soft-deleted nodes (archived_at set), row only,
//                      newest first - for the app's "Kosz" screen.
//   ?node_id=<id>      one node + its full history - for restoring a single node
//                      from the trash (brought back as `detached` on the gateway).
// GET, key-gated, read-only. Deploy on the gen2 server DB. Creds in secrets.php.

header('Content-Type: application/json; charset=utf-8');

// Credentials + key live in secrets.php (gitignored). Copy secrets.example.php ->
// secrets.php on the server and fill it in. Never commit secrets.php.
require __DIR__ . '/secrets.php';

if (($_GET['key'] ?? '') !== $BACKUP_KEY) {
    http_response_code(401);
    echo json_encode(['error' => 'bad key']);
    exit;
}

$conn = new mysqli($GW_DB_HOST, $GW_DB_USER, $GW_DB_PASS, $GW_DB_NAME);
if ($conn->connect_error) {
    http_response_code(500);
    echo json_encode(['error' => 'db connect']);
    exit;
}

// mysqli returns every column as a string; cast the numeric ones back to numbers so
// the gateway decodes them into typed fields. Text columns stay strings.
function castRow(array $r): array {
    static $text = ['name'=>1,'room'=>1,'value'=>1,'factory_id'=>1,'status'=>1,'param_key'=>1,'key'=>1];
    foreach ($r as $k => $v) {
        if ($v !== null && !isset($text[$k])) {
            $r[$k] = is_numeric($v) ? (0 + $v) : $v;
        }
    }
    return $r;
}

$historyTables = [
  'node_param'    => 'gw_node_param',
  'solar_hourly'  => 'gw_solar_hourly',
  'solar_daily'   => 'gw_solar_daily',
  'solar_monthly' => 'gw_solar_monthly',
];

// ---- mode: trash listing (archived nodes) ----
if (isset($_GET['archived'])) {
    $rows = [];
    if ($res = $conn->query("SELECT * FROM gw_node WHERE archived_at IS NOT NULL ORDER BY archived_at DESC")) {
        while ($r = $res->fetch_assoc()) { $rows[] = castRow($r); }
        $res->free();
    }
    $conn->close();
    echo json_encode(['node' => $rows]);
    exit;
}

// ---- mode: single node + its history (restore one from trash) ----
if (isset($_GET['node_id'])) {
    $nid = (int)$_GET['node_id'];
    $out = ['node' => []];
    if ($s = $conn->prepare("SELECT * FROM gw_node WHERE node_id = ?")) {
        $s->bind_param('i', $nid); $s->execute();
        $res = $s->get_result();
        while ($r = $res->fetch_assoc()) { $out['node'][] = castRow($r); }
        $s->close();
    }
    foreach ($historyTables as $key => $table) {
        $rows = [];
        if ($s = $conn->prepare("SELECT * FROM $table WHERE node_id = ?")) {
            $s->bind_param('i', $nid); $s->execute();
            $res = $s->get_result();
            while ($r = $res->fetch_assoc()) { $rows[] = castRow($r); }
            $s->close();
        }
        $out[$key] = $rows;
    }
    $conn->close();
    echo json_encode($out);
    exit;
}

// ---- mode: full restore (LIVE nodes only - trash is skipped) ----
$out = [];
// config is not node-scoped.
$rows = [];
if ($res = $conn->query("SELECT * FROM gw_config")) {
    while ($r = $res->fetch_assoc()) { $rows[] = castRow($r); }
    $res->free();
}
$out['config'] = $rows;
// live nodes
$rows = [];
if ($res = $conn->query("SELECT * FROM gw_node WHERE archived_at IS NULL")) {
    while ($r = $res->fetch_assoc()) { $rows[] = castRow($r); }
    $res->free();
}
$out['node'] = $rows;
// their history only (a trashed node's history is not restored into a live gateway)
foreach ($historyTables as $key => $table) {
    $rows = [];
    if ($res = $conn->query(
        "SELECT * FROM $table WHERE node_id IN (SELECT node_id FROM gw_node WHERE archived_at IS NULL)")) {
        while ($r = $res->fetch_assoc()) { $rows[] = castRow($r); }
        $res->free();
    }
    $out[$key] = $rows;
}
$conn->close();
echo json_encode($out);
