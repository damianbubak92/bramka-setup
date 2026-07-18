<?php
// solar-export.php - read-only JSON export of the gen1 solar telemetry, for the
// gen2 gateway to back-fill its own solar_history (source='gen1').
//
// Deploy next to the existing gen1 scripts on the gen1 server (same DB creds).
// Called by gen2: GET solar-export.php?since=<unix_ts>&limit=<n>&key=<KEY>
//   - since : return rows with UNIX_TIMESTAMP(readingTime) > since (0 = from start)
//   - limit : page size (default 2000, capped at 5000)
//   - key   : shared secret (must match $EXPORT_KEY in secrets.php)
// Returns a JSON array ordered by readingTime ASC. The gateway pages by advancing
// `since` to the last ts it received, so paging is contiguous.
//
// SECURITY: read-only (SELECT), key-gated, no user input in SQL except two ints
// (cast). Creds + key live in secrets.php (gitignored).

header('Content-Type: application/json; charset=utf-8');

// Credentials + key live in secrets.php (gitignored). Copy secrets.example.php ->
// secrets.php on the server and fill it in. Never commit secrets.php.
require __DIR__ . '/secrets.php';

if (($_GET['key'] ?? '') !== $EXPORT_KEY) {
    http_response_code(401);
    echo json_encode(['error' => 'bad key']);
    exit;
}

$since = isset($_GET['since']) ? (int)$_GET['since'] : 0;
$limit = isset($_GET['limit']) ? (int)$_GET['limit'] : 2000;
if ($limit < 1)    { $limit = 1; }
if ($limit > 5000) { $limit = 5000; }

$conn = new mysqli($GEN1_DB_HOST, $GEN1_DB_USER, $GEN1_DB_PASS, $GEN1_DB_NAME);
if ($conn->connect_error) {
    http_response_code(500);
    echo json_encode(['error' => 'db connect']);
    exit;
}

// readingTime is server-local; UNIX_TIMESTAMP interprets it in the session tz, i.e.
// the same tz it was written in, so the epoch round-trips correctly. The gateway
// buckets it back into Europe/Warsaw for the charts.
$sql = "SELECT UNIX_TIMESTAMP(readingTime) AS ts,
               inputTemp, outputTemp,
               bufor1Temp, bufor2Temp, bufor3Temp, bufor4Temp,
               collectorTemp, pwmValue, sPumpState, extraTemp, pumpRuntime
        FROM SolarSystem
        WHERE UNIX_TIMESTAMP(readingTime) > ?
        ORDER BY readingTime ASC
        LIMIT ?";

$stmt = $conn->prepare($sql);
$stmt->bind_param('ii', $since, $limit);
$stmt->execute();
$res = $stmt->get_result();

$out = [];
while ($row = $res->fetch_assoc()) {
    $out[] = [
        'ts'            => (int)$row['ts'],
        'inputTemp'     => (float)$row['inputTemp'],
        'outputTemp'    => (float)$row['outputTemp'],
        'bufor1Temp'    => (float)$row['bufor1Temp'],
        'bufor2Temp'    => (float)$row['bufor2Temp'],
        'bufor3Temp'    => (float)$row['bufor3Temp'],
        'bufor4Temp'    => (float)$row['bufor4Temp'],
        'collectorTemp' => (float)$row['collectorTemp'],
        'pwmValue'      => (int)$row['pwmValue'],
        'sPumpState'    => (int)$row['sPumpState'],
        'extraTemp'     => (int)$row['extraTemp'],
        'pumpRuntime'   => (int)$row['pumpRuntime'],
    ];
}
$stmt->close();
$conn->close();

echo json_encode($out);
