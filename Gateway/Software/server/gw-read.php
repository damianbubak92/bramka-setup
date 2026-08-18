<?php
// gw-read.php - READ-ONLY app fallback from the gen2 mirror, for when the gateway
// is unreachable. Speaks the SAME "command=" protocol as the gateway (the app POSTs
// a text body "command=X&...&authToken=KEY") and returns byte-compatible JSON, so
// the app parses the exact same DTOs (net/Dto.kt). READ subset only; any write is
// hard-refused. Uses its OWN key ($READ_KEY, distinct from $BACKUP_KEY) so a leaked
// app read key can only ever read the mirror, never push to it.
//
// Every response carries "X-Gateway-Last-Push: <unix>" (gw_config['last_push_at'],
// stamped by gw-backup.php on each push) = "last contact with the gateway", for the
// app's offline status ("Offline - dane z kopii: 24 min temu").
//
// The server computes nothing except the two solar-derived fields the gateway
// normally gets from its solar_state VIEW (powerKw, energyDayKwh) and the history
// series rebuilt from the aggregate tables. Everything else is a straight mirror read.
//
// Deploy on the gen2 server DB (same dir as gw-backup.php). Creds + key in secrets.php.

header('Content-Type: application/json; charset=utf-8');
require __DIR__ . '/secrets.php';

// The app posts text/plain "command=state&...&authToken=KEY". parse_str splits it,
// which conveniently also breaks "history&range=day&count=1&node=241" into fields.
parse_str(file_get_contents('php://input'), $P);
foreach ($_GET as $k => $v) { if (!isset($P[$k])) $P[$k] = $v; } // belt: also accept query

if (($P['authToken'] ?? '') !== $READ_KEY) {
    http_response_code(401);
    echo 'Odmowa';
    exit;
}
$cmd = (string)($P['command'] ?? '');

// Hard read-only: refuse every write command (the app never sends these to the
// mirror, but defence in depth - the mirror must have zero command surface).
$WRITE = ['PUMP_ON','PUMP_OFF','setrules','approvejoin','rejectjoin','removenode',
          'updatenode','replacenode','repairnode','restorenode'];
foreach ($WRITE as $w) {
    if (stripos($cmd, $w) !== false) {
        http_response_code(403);
        echo 'read-only (dane z kopii - bramka offline)';
        exit;
    }
}

$conn = @new mysqli($GW_DB_HOST, $GW_DB_USER, $GW_DB_PASS, $GW_DB_NAME);
if ($conn->connect_error) {
    http_response_code(500);
    echo json_encode(['error' => 'db connect']);
    exit;
}
$conn->set_charset('utf8mb4');

// "last contact with the gateway" header (best-effort).
$lastPush = mirror_config($conn, 'last_push_at');
if ($lastPush !== null) {
    header('X-Gateway-Last-Push: ' . (int)$lastPush);
}

$TZ = new DateTimeZone('Europe/Warsaw');

switch ($cmd) {

    case 'listnodes': {
        // NodeInfoDto[]: {id,address,type,name,factory,status,lastSeen,provisionedAt,room,capabilities}
        // Live nodes only (archived_at IS NULL); capabilities is not mirrored -> 0
        // (the automation editor is greyed out in read-only anyway).
        $out = [];
        $res = $conn->query("SELECT node_id,address,node_type,name,factory_id,status,last_seen,provisioned_at,room
                             FROM gw_node WHERE archived_at IS NULL ORDER BY node_id");
        while ($r = $res->fetch_assoc()) {
            $out[] = [
                'id'            => (int)$r['node_id'],
                'address'       => (int)($r['address'] ?? 0),
                'type'          => (int)$r['node_type'],
                'name'          => (string)($r['name'] ?? ''),
                'factory'       => (string)($r['factory_id'] ?? ''),
                'status'        => (string)($r['status'] ?? 'active'),
                'lastSeen'      => (int)($r['last_seen'] ?? 0),
                'provisionedAt' => (int)($r['provisioned_at'] ?? 0),
                'room'          => (string)($r['room'] ?? ''),
                'capabilities'  => 0,
            ];
        }
        echo json_encode($out);
        break;
    }

    case 'listtrash': {
        // TrashNodeDto[]: {id,type,name,room,factory,lastSeen,archivedAt}
        $out = [];
        $res = $conn->query("SELECT node_id,node_type,name,room,factory_id,last_seen,archived_at
                             FROM gw_node WHERE archived_at IS NOT NULL ORDER BY archived_at DESC");
        while ($r = $res->fetch_assoc()) {
            $out[] = [
                'id'         => (int)$r['node_id'],
                'type'       => (int)$r['node_type'],
                'name'       => (string)($r['name'] ?? ''),
                'room'       => (string)($r['room'] ?? ''),
                'factory'    => (string)($r['factory_id'] ?? ''),
                'lastSeen'   => (int)($r['last_seen'] ?? 0),
                'archivedAt' => (int)($r['archived_at'] ?? 0),
            ];
        }
        echo json_encode($out);
        break;
    }

    case 'getrules': {
        // Raw rules JSON, verbatim (the app parses it in RulesCodec). Stored by the
        // gateway in config['rules'] -> mirrored to gw_config.
        $v = mirror_config($conn, 'rules');
        echo ($v !== null && $v !== '') ? $v : '[]';
        break;
    }

    case 'state': {
        // NodeStateDto[]: {address,type,params:{k:v},ts,powerKw?,energyDayKwh?}
        // Last-known telemetry per node (gw_node_param), plus the solar-derived fields
        // the gateway computes from its solar_state VIEW.
        $nodes = []; // node_id -> [address,type]
        $res = $conn->query("SELECT node_id,address,node_type FROM gw_node WHERE archived_at IS NULL");
        while ($r = $res->fetch_assoc()) {
            $nodes[(int)$r['node_id']] = ['address' => (int)($r['address'] ?? 0), 'type' => (int)$r['node_type']];
        }
        $params = []; $ts = [];
        $res = $conn->query("SELECT node_id,param_key,value_num,ts FROM gw_node_param");
        while ($r = $res->fetch_assoc()) {
            $nid = (int)$r['node_id'];
            $params[$nid][(string)$r['param_key']] = (float)$r['value_num'];
            $t = (int)($r['ts'] ?? 0);
            if (!isset($ts[$nid]) || $t > $ts[$nid]) $ts[$nid] = $t;
        }
        $out = [];
        foreach ($params as $nid => $p) {
            if (!isset($nodes[$nid])) continue; // orphan param row -> skip (matches the live filter)
            $node = $nodes[$nid];
            $entry = [
                'id'      => $nid, // gateway emits it; app keys by address but keep it faithful
                'address' => $node['address'],
                'type'    => $node['type'],
                'params'  => empty($p) ? new stdClass() : $p, // {} not [] for an empty map
                'ts'      => $ts[$nid] ?? 0,
            ];
            if ($node['type'] === 0) { // SOLAR: rebuild powerKw + energyDayKwh
                if (isset($p['energyGain'])) {
                    $entry['powerKw'] = 30.0 * $p['energyGain'] / 10000.0;
                }
                $entry['energyDayKwh'] = solar_day_yield($conn, $nid, $TZ);
            }
            $out[] = $entry;
        }
        echo json_encode($out);
        break;
    }

    case 'climatehistory': {
        // Local-only on the gateway (climate_history is NOT mirrored) -> empty offline.
        echo '[]';
        break;
    }

    case 'listjoins': {
        // No live provisioning offline (JOIN is silenced in read-only) -> empty.
        echo '[]';
        break;
    }

    case 'history': {
        $range = (string)($P['range'] ?? 'day');
        $count = (int)($P['count'] ?? 0);
        $node  = (int)($P['node'] ?? 241); // solarDefaultNode
        echo json_encode(solar_history($conn, $node, $range, $count, $TZ));
        break;
    }

    default:
        http_response_code(400);
        echo 'unknown or unsupported command (read-only mirror)';
}

$conn->close();

// ---------------------------------------------------------------------------

function mirror_config(mysqli $conn, string $key): ?string {
    $s = $conn->prepare("SELECT `value` FROM gw_config WHERE `key` = ?");
    $s->bind_param('s', $key);
    $s->execute();
    $r = $s->get_result()->fetch_assoc();
    $s->close();
    return $r ? (string)$r['value'] : null;
}

// energyDayKwh: today's running total = day_yield of the newest hourly bucket that
// falls in today (Europe/Warsaw). 0 if no data yet today (honest for a copy).
function solar_day_yield(mysqli $conn, int $nid, DateTimeZone $tz): float {
    $today = (new DateTime('now', $tz))->setTime(0, 0, 0)->getTimestamp();
    $s = $conn->prepare("SELECT day_yield FROM gw_solar_hourly
                         WHERE node_id = ? AND bucket >= ? ORDER BY bucket DESC LIMIT 1");
    $s->bind_param('ii', $nid, $today);
    $s->execute();
    $r = $s->get_result()->fetch_assoc();
    $s->close();
    return $r ? (float)$r['day_yield'] : 0.0;
}

// solar_history rebuilds SolarSeriesDto[] from the aggregate tables. Not a byte-exact
// port of the gateway's SolarHistory (no live-day injection - the mirror only has what
// was pushed), but the DATA and JSON shape are correct and fully browsable: day ->
// hourly bars, month -> daily bars, year -> monthly bars, total -> yearly bars.
// count>0 keeps the newest N periods (day/month/year); count<=0 returns all with data.
function solar_history(mysqli $conn, int $nid, string $range, int $count, DateTimeZone $tz): array {
    switch ($range) {
        case 'day':   return build_series($conn, $nid, $tz, 'gw_solar_hourly', 'hour_yield', 'hour_pump', 'day',   $count);
        case 'month': return build_series($conn, $nid, $tz, 'gw_solar_daily',  'day_yield',  '',          'month', $count);
        case 'year':  return build_series($conn, $nid, $tz, 'gw_solar_monthly','month_yield','',          'year',  $count);
        case 'total': return build_total($conn, $nid, $tz);
        default:      return [];
    }
}

// Group the aggregate rows of one table into series by the given period, each series
// carrying the finer-grained rows as bars.
function build_series(mysqli $conn, int $nid, DateTimeZone $tz, string $table,
                      string $yieldCol, string $pumpCol, string $period, int $count): array {
    $pump = $pumpCol !== '' ? ",`$pumpCol`" : '';
    $s = $conn->prepare("SELECT bucket,`$yieldCol`$pump FROM `$table` WHERE node_id = ? ORDER BY bucket ASC");
    $s->bind_param('i', $nid);
    $s->execute();
    $res = $s->get_result();

    $series = []; // periodStartTs => ['bucket'=>ts,'label'=>..,'bars'=>[],'energyKwh'=>0,'pumpMinutes'=>0]
    while ($r = $res->fetch_assoc()) {
        $bucket = (int)$r['bucket'];
        $yield  = (float)$r[$yieldCol];
        $pmin   = $pumpCol !== '' ? (int)$r[$pumpCol] : 0;

        [$pStart, $label] = period_of($bucket, $period, $tz);
        if (!isset($series[$pStart])) {
            $series[$pStart] = ['bucket' => $pStart, 'label' => $label, 'bars' => [],
                                'energyKwh' => 0.0, 'pumpMinutes' => 0, 'samples' => 0, 'expected' => 0];
        }
        $series[$pStart]['bars'][] = ['bucket' => $bucket, 'energyKwh' => $yield,
                                      'pumpMinutes' => $pmin, 'samples' => 1, 'expected' => 1];
        $series[$pStart]['energyKwh']   += $yield;
        $series[$pStart]['pumpMinutes'] += $pmin;
        $series[$pStart]['samples']     += 1;
        $series[$pStart]['expected']    += 1;
    }
    $s->close();

    ksort($series);            // oldest -> newest
    $out = array_values($series);
    if ($count > 0 && count($out) > $count) {
        $out = array_slice($out, -$count); // newest N
    }
    return $out;
}

// build_total: one series over all years, bars = per-year totals (from monthly).
function build_total(mysqli $conn, int $nid, DateTimeZone $tz): array {
    $s = $conn->prepare("SELECT bucket,month_yield FROM gw_solar_monthly WHERE node_id = ? ORDER BY bucket ASC");
    $s->bind_param('i', $nid);
    $s->execute();
    $res = $s->get_result();

    $years = []; // yearStartTs => [yield, label]
    while ($r = $res->fetch_assoc()) {
        [$yStart, $label] = period_of((int)$r['bucket'], 'year', $tz);
        if (!isset($years[$yStart])) $years[$yStart] = ['yield' => 0.0, 'label' => $label];
        $years[$yStart]['yield'] += (float)$r['month_yield'];
    }
    $s->close();
    if (empty($years)) return [];

    ksort($years);
    $bars = []; $sum = 0.0;
    foreach ($years as $yStart => $y) {
        $bars[] = ['bucket' => $yStart, 'energyKwh' => $y['yield'], 'pumpMinutes' => 0,
                   'samples' => 1, 'expected' => 1];
        $sum += $y['yield'];
    }
    $firstYear = date('Y', array_key_first($years));
    $lastYear  = date('Y', array_key_last($years));
    $label = $firstYear === $lastYear ? "Łącznie $firstYear" : "Łącznie $firstYear-$lastYear";
    return [[
        'bucket' => (int)array_key_first($years), 'label' => $label, 'bars' => $bars,
        'energyKwh' => $sum, 'pumpMinutes' => 0, 'samples' => count($bars), 'expected' => count($bars),
    ]];
}

// period_of maps a bucket (unix, local-hour aligned) to its containing period's start
// timestamp and a Polish label.
function period_of(int $bucket, string $period, DateTimeZone $tz): array {
    $d = (new DateTime('@' . $bucket))->setTimezone($tz);
    $y = (int)$d->format('Y'); $m = (int)$d->format('n'); $day = (int)$d->format('j');
    switch ($period) {
        case 'day':
            $start = (clone $d)->setTime(0, 0, 0)->getTimestamp();
            return [$start, "$day " . pl_month($m) . " $y"];      // "12 lip 2026"
        case 'month':
            $start = (new DateTime('now', $tz))->setDate($y, $m, 1)->setTime(0, 0, 0)->getTimestamp();
            return [$start, pl_month($m) . " $y"];                 // "lip 2026"
        case 'year':
            $start = (new DateTime('now', $tz))->setDate($y, 1, 1)->setTime(0, 0, 0)->getTimestamp();
            return [$start, (string)$y];                            // "2026"
    }
    return [$bucket, ''];
}

function pl_month(int $m): string {
    static $a = [1 => 'sty','lut','mar','kwi','maj','cze','lip','sie','wrz','paź','lis','gru'];
    return $a[$m] ?? '';
}
