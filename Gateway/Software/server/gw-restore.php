<?php
// gw-restore.php - a fresh gateway pulls the whole mirror here to rebuild itself
// after a hardware failure (registered nodes, rules, current state, solar history).
// GET gw-restore.php?key=<secret> -> {config:[...], node:[...], node_param:[...],
//   solar_hourly:[...], solar_daily:[...], solar_monthly:[...]}.
// Read-only, key-gated. Deploy on the gen2 server DB. FILL IN creds; do NOT commit them.

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

$tables = [
  'config'        => 'gw_config',
  'node'          => 'gw_node',
  'node_param'    => 'gw_node_param',
  'solar_hourly'  => 'gw_solar_hourly',
  'solar_daily'   => 'gw_solar_daily',
  'solar_monthly' => 'gw_solar_monthly',
];

$out = [];
foreach ($tables as $key => $table) {
    $rows = [];
    if ($res = $conn->query("SELECT * FROM $table")) {
        while ($r = $res->fetch_assoc()) {
            // numeric columns come back as strings from mysqli; cast the known ones
            foreach ($r as $k => $v) {
                if ($v !== null && $k !== 'name' && $k !== 'room' && $k !== 'value'
                    && $k !== 'factory_id' && $k !== 'status' && $k !== 'param_key' && $k !== 'key') {
                    $r[$k] = is_numeric($v) ? (0 + $v) : $v;
                }
            }
            $rows[] = $r;
        }
        $res->free();
    }
    $out[$key] = $rows;
}
$conn->close();

echo json_encode($out);
