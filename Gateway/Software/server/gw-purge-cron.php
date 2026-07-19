<?php
// gw-purge-cron.php - server-side retention. Permanently deletes trashed nodes (and
// their history) once they are past the retention window. This is the ONLY thing that
// hard-deletes a node from the mirror: there is deliberately no manual "empty trash"
// (a child playing with the phone must not be able to wipe years of a customer's
// history). Removal on the gateway only soft-deletes here (archived_at); time does
// the rest.
//
// Run from cron, e.g. daily:
//   0 4 * * *  php /home/USER/public_html/gw-purge-cron.php >> /home/USER/purge.log 2>&1
// or hit it as a scheduled HTTPS GET with ?key=<BACKUP_KEY> (key required over HTTP).

require __DIR__ . '/secrets.php';

$RETENTION_DAYS = 60;

$viaCli = (php_sapi_name() === 'cli');
if (!$viaCli) {
    header('Content-Type: application/json; charset=utf-8');
    if (($_GET['key'] ?? '') !== $BACKUP_KEY) {
        http_response_code(401);
        echo json_encode(['error' => 'bad key']);
        exit;
    }
}

$conn = new mysqli($GW_DB_HOST, $GW_DB_USER, $GW_DB_PASS, $GW_DB_NAME);
if ($conn->connect_error) {
    if (!$viaCli) http_response_code(500);
    echo $viaCli ? "db connect error\n" : json_encode(['error' => 'db connect']);
    exit;
}

$cutoff = time() - $RETENTION_DAYS * 86400;

$ids = [];
if ($s = $conn->prepare("SELECT node_id FROM gw_node WHERE archived_at IS NOT NULL AND archived_at < ?")) {
    $s->bind_param('i', $cutoff);
    $s->execute();
    $res = $s->get_result();
    while ($r = $res->fetch_assoc()) { $ids[] = (int)$r['node_id']; }
    $s->close();
}

$purged = 0;
foreach ($ids as $nid) {
    foreach (['gw_node','gw_node_param','gw_solar_hourly','gw_solar_daily','gw_solar_monthly'] as $t) {
        if ($s = $conn->prepare("DELETE FROM $t WHERE node_id = ?")) {
            $s->bind_param('i', $nid);
            $s->execute();
            $s->close();
        }
    }
    $purged++;
}
$conn->close();

$msg = "purged $purged node(s) past {$RETENTION_DAYS}d retention";
echo $viaCli ? (date('c') . " $msg\n") : json_encode(['ok' => true, 'purged' => $purged, 'ids' => $ids]);
