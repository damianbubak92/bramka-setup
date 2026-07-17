package main

// SQLite-backed persistence (mattn/go-sqlite3, cgo). Phase 2 stores only the
// rules ruleset (source of truth for getrules + the on-connect engine push).
// Future phases add node-state and per-node time-series tables in the same DB
// (the solar controller streams ~10 params/2min -> energy-yield aggregation).
//
// On the bramka the module must be fetched once (internet present):
//   cd <service dir> && go get github.com/mattn/go-sqlite3 && go build
//
// WAL + synchronous=NORMAL: survives power-loss with far fewer fsyncs (kinder to
// eMMC/SD flash). DB lives under /var/lib/bramka (writable by the bramka user).

import (
	"database/sql"
	"fmt"
	"log"
	"strings"
	"time"

	_ "github.com/mattn/go-sqlite3"
)

type Store struct {
	db *sql.DB
	// loc is the wall-clock zone for the solar daily-accumulation reset boundary
	// (energy_gain / pump_runtime restart at local midnight).
	loc *time.Location
}

// OpenStore opens (creating if needed) the SQLite DB and ensures the schema. loc
// is the daily-rollover zone for solar accumulation (nil -> Europe/Warsaw).
func OpenStore(path string, loc *time.Location) (*Store, error) {
	if loc == nil {
		var err error
		if loc, err = time.LoadLocation("Europe/Warsaw"); err != nil {
			loc = time.Local
		}
	}
	dsn := path + "?_journal_mode=WAL&_synchronous=NORMAL&_busy_timeout=5000"
	// driverWithUpdateHook (dbmonitor.go) is plain sqlite3 + an update hook, so the
	// DB monitor can journal every write. With no hook installed the cost is nil.
	db, err := sql.Open(driverWithUpdateHook, dsn)
	if err != nil {
		return nil, fmt.Errorf("open db %s: %w", path, err)
	}
	if err := db.Ping(); err != nil {
		db.Close()
		return nil, fmt.Errorf("ping db %s: %w", path, err)
	}
	// Single connection: SQLite is not for concurrent writers; serialize via Go.
	db.SetMaxOpenConns(1)

	if _, err := db.Exec(`CREATE TABLE IF NOT EXISTS config (
		key   TEXT PRIMARY KEY,
		value TEXT NOT NULL
	)`); err != nil {
		db.Close()
		return nil, fmt.Errorf("init schema: %w", err)
	}
	if err := initNodeSchema(db); err != nil {
		db.Close()
		return nil, fmt.Errorf("init node schema: %w", err)
	}
	return &Store{db: db, loc: loc}, nil
}

// initNodeSchema creates the node-state model + the dedicated SolarController
// tables and seeds the param_def catalog for the node types in service today.
//
// Design (heterogeneous nodes, multiple instances of a type):
//   - node        : the physical nodes (provisioning registry).
//   - param_def   : per-node-TYPE catalog of params (label/unit/value_type +
//                   UI order). The app reads this to render any node type;
//                   provisioning (step #3) fills it. New node type = add rows
//                   here, NO schema change.
//   - node_param  : current state, one row per (node, param), UPSERTed each read
//                   (the engine/app reads "what is node X doing now"). This is
//                   the single generic current-state table for ALL node types.
//
// SolarController (and future types that need derived/accumulated data) gets a
// DEDICATED table on top of the generic layer:
//   - solar_history : append-only time-series; energy_gain & pump_runtime are
//                     accumulated per local day (step #2 reads this for the
//                     energy-yield / pump-runtime aggregations).
//   - solar_state   : VIEW = latest history row per node + derived
//                     generated_power_kw (no duplication, always fresh).
//
// There is intentionally NO generic time-series table: current state lives in
// node_param; per-type history lives in its dedicated table only.
//
// The wire MessageStruct (node_protocol.h) is a typed union; the per-type
// knowledge lives ONLY in the cgo decoder (telemetry.go), which emits generic
// (param_key, value) pairs. node_param stays type-agnostic.
func initNodeSchema(db *sql.DB) error {
	stmts := []string{
		`CREATE TABLE IF NOT EXISTS node (
			node_id        INTEGER PRIMARY KEY,
			node_type      INTEGER NOT NULL,
			name           TEXT,
			provisioned_at INTEGER,
			last_seen      INTEGER
		)`,
		`CREATE TABLE IF NOT EXISTS param_def (
			node_type     INTEGER NOT NULL,
			param_key     TEXT    NOT NULL,
			label         TEXT,
			unit          TEXT,
			value_type    TEXT,              -- float | int | bool | percent | text
			archive       INTEGER NOT NULL DEFAULT 1,  -- catalog hint: param has dedicated history
			display_order INTEGER NOT NULL DEFAULT 0,
			PRIMARY KEY (node_type, param_key)
		)`,
		`CREATE TABLE IF NOT EXISTS node_param (
			node_id    INTEGER NOT NULL,
			param_key  TEXT    NOT NULL,
			value_num  REAL,
			value_text TEXT,
			ts         INTEGER NOT NULL,
			PRIMARY KEY (node_id, param_key)
		)`,
		`CREATE TABLE IF NOT EXISTS solar_history (
			id                INTEGER PRIMARY KEY AUTOINCREMENT,
			node_id           INTEGER NOT NULL,
			reading_time      INTEGER NOT NULL,   -- unix s
			input_temp        REAL,               -- Tin
			output_temp       REAL,               -- Tout
			bufor1_temp       REAL,               -- T1
			bufor2_temp       REAL,               -- T2
			bufor3_temp       REAL,               -- T3
			bufor4_temp       REAL,               -- T4
			collector_temp    REAL,               -- Tcol
			flow_rate         INTEGER,            -- %
			second_pump_state INTEGER,            -- 0/1
			energy_gain_delta INTEGER,            -- raw 2-min gain, kWh*10000
			energy_gain       INTEGER,            -- accumulated this local day, kWh*10000
			pump_runtime      INTEGER             -- accumulated this local day, minutes
		)`,
		`CREATE INDEX IF NOT EXISTS idx_solar_hist ON solar_history(node_id, reading_time)`,
		`CREATE VIEW IF NOT EXISTS solar_state AS
			SELECT node_id,
			       bufor1_temp, bufor2_temp, bufor3_temp, bufor4_temp,
			       collector_temp, flow_rate,
			       30.0 * energy_gain_delta / 10000.0 AS generated_power_kw,
			       second_pump_state, reading_time
			FROM solar_history h
			WHERE reading_time = (SELECT MAX(reading_time)
			                      FROM solar_history WHERE node_id = h.node_id)`,
	}
	for _, q := range stmts {
		if _, err := db.Exec(q); err != nil {
			return err
		}
	}
	// Defense in depth behind solarMinIntervalS: one reading per node per second,
	// enforced by the DB itself. Deliberately NOT fatal - on a DB that still holds
	// legacy duplicates this fails, and refusing to start (losing telemetry and
	// recovery) would be a far worse outcome than a missing constraint.
	if _, err := db.Exec(
		`CREATE UNIQUE INDEX IF NOT EXISTS idx_solar_hist_uniq
		 ON solar_history(node_id, reading_time)`); err != nil {
		log.Printf("[Store] WARNING: solar_history uniqueness not enforced (%v) - "+
			"clean the duplicates, then restart to apply it", err)
	}
	// migrate: node.factory_id (the chip's CC1310 FCFG identity) - added after the
	// original schema so a known chip's rejoin can reuse its address. ALTER fails
	// with "duplicate column name" if it already exists; treat that as success.
	if _, err := db.Exec(`ALTER TABLE node ADD COLUMN factory_id TEXT`); err != nil &&
		!strings.Contains(err.Error(), "duplicate column name") {
		return err
	}
	// migrate: node.status - provisioning lifecycle state (pending_join | active |
	// pending_remove). NULL on pre-existing rows is treated as 'active'.
	if _, err := db.Exec(`ALTER TABLE node ADD COLUMN status TEXT`); err != nil &&
		!strings.Contains(err.Error(), "duplicate column name") {
		return err
	}
	// migrate: node.room - user-facing grouping for the phone's device manager.
	// Purely a label (the node knows nothing about rooms); NULL/empty = "Bez pokoju".
	if _, err := db.Exec(`ALTER TABLE node ADD COLUMN room TEXT`); err != nil &&
		!strings.Contains(err.Error(), "duplicate column name") {
		return err
	}
	return seedParamDefs(db)
}

// paramDefSeed is a built-in catalog row. Labels/units are sensible defaults for
// the two active node types; provisioning (step #3) may override them. Units for
// energyGain/flowRate are best-guess (verify against the node firmware).
type paramDefSeed struct {
	nodeType   uint8
	key        string
	label      string
	unit       string
	valueType  string
	archive    int
	order      int
}

// seedParamDefs is INSERT OR IGNORE: idempotent, and never clobbers admin/
// provisioning edits to an existing (node_type, param_key).
func seedParamDefs(db *sql.DB) error {
	// NODE_SOLAR_CONTROLLER = 0, NODE_BUFOR_CONTROLLER = 1 (node_protocol.h).
	seeds := []paramDefSeed{
		{0, "Tcol", "Temp. kolektora", "°C", "float", 1, 1},
		{0, "Tin", "Temp. zasilania", "°C", "float", 1, 2},
		{0, "Tout", "Temp. powrotu", "°C", "float", 1, 3},
		{0, "T1", "Temperatura T1", "°C", "float", 1, 4},
		{0, "T2", "Temperatura T2", "°C", "float", 1, 5},
		{0, "T3", "Temperatura T3", "°C", "float", 1, 6},
		{0, "T4", "Temperatura T4", "°C", "float", 1, 7},
		{0, "energyGain", "Uzysk energii", "Wh", "int", 1, 8},
		{0, "flowRate", "Przepływ", "%", "percent", 1, 9},
		{0, "pumpState", "Pompa", "", "bool", 1, 10},
		{1, "sBuforTemp", "Temp. bufora", "°C", "float", 1, 1},
		// NODE_TH_SENSOR = 6: current state only for now (archive=0, no history table).
		{6, "temperature", "Temperatura", "°C", "float", 0, 1},
		{6, "humidity", "Wilgotność", "%", "percent", 0, 2},
		{6, "batt_mv", "Napięcie baterii", "mV", "int", 0, 3},
		{6, "soh_pct", "Kondycja baterii (SOH)", "%", "int", 0, 4},
		{6, "acc_uah", "Zużycie skumulowane", "µAh", "int", 0, 5},
	}
	for _, d := range seeds {
		if _, err := db.Exec(
			`INSERT OR IGNORE INTO param_def
			 (node_type, param_key, label, unit, value_type, archive, display_order)
			 VALUES (?, ?, ?, ?, ?, ?, ?)`,
			d.nodeType, d.key, d.label, d.unit, d.valueType, d.archive, d.order,
		); err != nil {
			return err
		}
	}
	return nil
}

// nodeTypeSolar mirrors NODE_SOLAR_CONTROLLER (node_protocol.h). A solar full
// reading (cmd SEND_DATA_TO_DB -> carries energyGain) also feeds solar_history.
const nodeTypeSolar uint8 = 0

// Assignable address pool (node_protocol.h ADDR_POOL_FIRST/LAST). The legacy
// fixed-address nodes (0xF1/0xF2/0xF3) and the gateway (0xF0) sit ABOVE this
// range, so provisioning never collides with them.
const (
	addrPoolFirst = 0x10
	addrPoolLast  = 0xEF
)

// ProvisionNode binds an approved JOIN to a logical address: the lowest free pool
// address, recorded with the node's type/name/factory id. It is idempotent per
// factory id - re-approving (or a known chip rejoining) reuses the same address
// instead of consuming a new one. Returns the assigned address. ts is unix s.
func (s *Store) ProvisionNode(nodeType uint8, name, factoryID string, ts int64) (uint8, error) {
	tx, err := s.db.Begin()
	if err != nil {
		return 0, err
	}
	defer tx.Rollback()

	// Known chip -> reuse its address (idempotent re-approve / rejoin).
	var existing sql.NullInt64
	err = tx.QueryRow(`SELECT node_id FROM node WHERE factory_id = ?`, factoryID).Scan(&existing)
	if err != nil && err != sql.ErrNoRows {
		return 0, err
	}
	if existing.Valid {
		// Re-approve of a known chip: keep its address + update name/type. A node
		// mid-removal (pending_remove) is revived to active (cancels the removal);
		// otherwise the status is left as-is.
		addr := uint8(existing.Int64)
		if _, err := tx.Exec(
			`UPDATE node SET name = ?, node_type = ?, provisioned_at = ?, last_seen = ?,
			     status = CASE WHEN status = 'pending_remove' THEN 'active' ELSE status END
			 WHERE node_id = ?`, name, nodeType, ts, ts, addr); err != nil {
			return 0, err
		}
		return addr, tx.Commit()
	}

	addr, err := allocAddr(tx)
	if err != nil {
		return 0, err
	}
	// New node: pending_join until it confirms (reports under the assigned address).
	if _, err := tx.Exec(
		`INSERT INTO node (node_id, node_type, name, factory_id, provisioned_at, last_seen, status)
		 VALUES (?, ?, ?, ?, ?, ?, 'pending_join')`, addr, nodeType, name, factoryID, ts, ts); err != nil {
		return 0, err
	}
	return addr, tx.Commit()
}

// NodeStatus returns a node's lifecycle state + identity (for the telemetry-path
// state machine). status is 'active' for pre-migration rows (NULL).
// UpdateNode sets the user-facing label and room of an already registered node.
// Both are pure labels - the node knows nothing about either. The phone's device
// editor saves them together, so this is one call (and one round trip).
func (s *Store) UpdateNode(address uint8, name, room string) error {
	_, err := s.db.Exec(`UPDATE node SET name = ?, room = ? WHERE node_id = ?`, name, room, address)
	return err
}

func (s *Store) NodeStatus(address uint8) (status, factoryID string, nodeType uint8, exists bool, err error) {
	var st, fid sql.NullString
	var nt int
	e := s.db.QueryRow(
		`SELECT COALESCE(status,'active'), COALESCE(factory_id,''), node_type
		 FROM node WHERE node_id = ?`, address).Scan(&st, &fid, &nt)
	if e == sql.ErrNoRows {
		return "", "", 0, false, nil
	}
	if e != nil {
		return "", "", 0, false, e
	}
	return st.String, fid.String, uint8(nt), true, nil
}

// MarkActive flips a node pending_join -> active (called on first telemetry from
// the assigned address = the node's read-back confirmation).
func (s *Store) MarkActive(address uint8) error {
	_, err := s.db.Exec(
		`UPDATE node SET status = 'active' WHERE node_id = ? AND status = 'pending_join'`, address)
	return err
}

// SetPendingRemove marks a node as awaiting removal confirmation. The row (and
// thus its address reservation) stays until the node confirms it cleared itself.
func (s *Store) SetPendingRemove(address uint8) error {
	_, err := s.db.Exec(`UPDATE node SET status = 'pending_remove' WHERE node_id = ?`, address)
	return err
}

// NodeInfo is one provisioned node for the phone's device list (listnodes).
type NodeInfo struct {
	Address       int    `json:"address"`      // node_id (assigned address)
	Type          int    `json:"type"`         // NODE_* (app maps to a label)
	Name          string `json:"name"`         // user label set at approval
	FactoryID     string `json:"factory"`      // chip identity (hex)
	Status        string `json:"status"`       // pending_join | active | pending_remove
	LastSeen      int64  `json:"lastSeen"`     // unix s of last telemetry (0 = never)
	ProvisionedAt int64  `json:"provisionedAt"` // unix s
	Room          string `json:"room"`         // user grouping ("" = Bez pokoju)
}

// NodeState is the LAST KNOWN telemetry of one node, as stored in node_param.
// The phone reads this once on open so every field (temperatures, pump states,
// toggles) shows real values immediately, without waiting for the next 2-minute
// WS telemetry push.
type NodeState struct {
	Address int                `json:"address"`
	Type    int                `json:"type"`    // NODE_* (from the node row RecordTelemetry upserts)
	Params  map[string]float64 `json:"params"`  // param_key -> value (same keys as the WS telemetry event)
	Ts      int64              `json:"ts"`      // unix s of the newest param in this node
	// PowerKw is the derived instantaneous power for solar nodes, taken from the
	// solar_state VIEW (30 * energy_gain_delta / 10000, i.e. the last 2-minute
	// delta scaled to an hour). Sent so the phone can show real power immediately
	// on open instead of waiting for a second reading to diff. nil = not a solar
	// node / no history yet.
	PowerKw *float64 `json:"powerKw,omitempty"`
}

// ListState returns the current state of every node we have telemetry for.
// Mirrors the shape of the WS "telemetry" event so the app can seed its live
// state with the same code path.
func (s *Store) ListState() ([]NodeState, error) {
	// Unknown type MUST NOT default to 0: NODE_SOLAR_CONTROLLER is 0, so an orphan
	// node_param row (no node row) would masquerade as the solar controller and the
	// app would render its missing params as zeros. -1 = unknown, matches no NODE_*.
	rows, err := s.db.Query(
		`SELECT p.node_id, COALESCE(n.node_type, -1), p.param_key,
		        COALESCE(p.value_num, 0), p.ts
		 FROM node_param p
		 LEFT JOIN node n ON n.node_id = p.node_id
		 ORDER BY p.node_id, p.param_key`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	byID := make(map[int]*NodeState)
	order := make([]int, 0, 8)
	for rows.Next() {
		var id, typ int
		var key string
		var val float64
		var ts int64
		if err := rows.Scan(&id, &typ, &key, &val, &ts); err != nil {
			return nil, err
		}
		st, ok := byID[id]
		if !ok {
			st = &NodeState{Address: id, Type: typ, Params: make(map[string]float64)}
			byID[id] = st
			order = append(order, id)
		}
		st.Params[key] = val
		if ts > st.Ts {
			st.Ts = ts
		}
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}

	// Derived power straight from the VIEW (last history row). Best-effort: a solar
	// node with no history yet simply has no powerKw.
	if prows, perr := s.db.Query(`SELECT node_id, generated_power_kw FROM solar_state`); perr == nil {
		defer prows.Close()
		for prows.Next() {
			var id int
			var kw sql.NullFloat64
			if err := prows.Scan(&id, &kw); err != nil {
				continue
			}
			if st, ok := byID[id]; ok && kw.Valid {
				v := kw.Float64
				st.PowerKw = &v
			}
		}
	}

	out := make([]NodeState, 0, len(order))
	for _, id := range order {
		out = append(out, *byID[id])
	}
	return out, nil
}

// ListNodes returns the PROVISIONED nodes (those with a factory_id, i.e. they
// went through approval) for the phone's "devices" screen. Telemetry-only rows
// (no factory_id) are excluded; so are pending_remove rows — once the user hits
// remove the device vanishes from the UI immediately while the gateway quietly
// finishes (confirms with the node, frees the address, or keeps it reserved).
// last_seen is kept fresh by RecordTelemetry.
func (s *Store) ListNodes() ([]NodeInfo, error) {
	rows, err := s.db.Query(
		`SELECT node_id, node_type, COALESCE(name,''), COALESCE(factory_id,''),
		        COALESCE(status,'active'), COALESCE(last_seen,0), COALESCE(provisioned_at,0),
		        COALESCE(room,'')
		 FROM node
		 WHERE factory_id IS NOT NULL AND COALESCE(status,'active') <> 'pending_remove'
		 ORDER BY node_id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []NodeInfo{}
	for rows.Next() {
		var n NodeInfo
		if err := rows.Scan(&n.Address, &n.Type, &n.Name, &n.FactoryID,
			&n.Status, &n.LastSeen, &n.ProvisionedAt, &n.Room); err != nil {
			return nil, err
		}
		out = append(out, n)
	}
	return out, rows.Err()
}

// GetNode returns a node's factory id (hex) and type by address.
func (s *Store) GetNode(address uint8) (factoryID string, nodeType uint8, ok bool, err error) {
	var fid sql.NullString
	var nt int
	e := s.db.QueryRow(
		`SELECT COALESCE(factory_id,''), node_type FROM node WHERE node_id = ?`, address).
		Scan(&fid, &nt)
	if e == sql.ErrNoRows {
		return "", 0, false, nil
	}
	if e != nil {
		return "", 0, false, e
	}
	return fid.String, uint8(nt), true, nil
}

// DeleteNode removes a node's registry row (provisioning remove). Its current
// state in node_param is left as-is (harmless; a re-provision overwrites).
func (s *Store) DeleteNode(address uint8) error {
	_, err := s.db.Exec(`DELETE FROM node WHERE node_id = ?`, address)
	return err
}

// allocAddr returns the lowest free pool address (freed addresses are reused).
// This is safe because of the graceful-remove invariant: a node's row is deleted
// (its address freed) ONLY after the node confirms it cleared itself to
// unprovisioned - so a freed address never has a stale node on it. An offline /
// never-confirmed node keeps its row (pending_remove), so its address stays
// reserved and out of the free set. Runs inside the tx.
func allocAddr(tx *sql.Tx) (uint8, error) {
	rows, err := tx.Query(
		`SELECT node_id FROM node WHERE node_id BETWEEN ? AND ?`, addrPoolFirst, addrPoolLast)
	if err != nil {
		return 0, err
	}
	defer rows.Close()
	used := map[int]bool{}
	for rows.Next() {
		var id int
		if err := rows.Scan(&id); err != nil {
			return 0, err
		}
		used[id] = true
	}
	if err := rows.Err(); err != nil {
		return 0, err
	}
	for a := addrPoolFirst; a <= addrPoolLast; a++ {
		if !used[a] {
			return uint8(a), nil
		}
	}
	return 0, fmt.Errorf("address pool exhausted (0x%02X-0x%02X)", addrPoolFirst, addrPoolLast)
}

// RecordTelemetry upserts the node's current state (node_param) and, for a solar
// full reading, appends a solar_history row, all in one transaction. ts is unix s.
func (s *Store) RecordTelemetry(nodeID, nodeType uint8, params []NodeParam, ts int64) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()

	if _, err := tx.Exec(
		`INSERT INTO node (node_id, node_type, provisioned_at, last_seen)
		 VALUES (?, ?, ?, ?)
		 ON CONFLICT(node_id) DO UPDATE SET
		     node_type = excluded.node_type,
		     last_seen = excluded.last_seen`,
		nodeID, nodeType, ts, ts,
	); err != nil {
		return err
	}

	upState, err := tx.Prepare(
		`INSERT INTO node_param (node_id, param_key, value_num, ts)
		 VALUES (?, ?, ?, ?)
		 ON CONFLICT(node_id, param_key) DO UPDATE SET
		     value_num = excluded.value_num,
		     ts        = excluded.ts`)
	if err != nil {
		return err
	}
	defer upState.Close()

	vals := make(map[string]float64, len(params))
	for _, pm := range params {
		if _, err := upState.Exec(nodeID, pm.Key, pm.Num, ts); err != nil {
			return err
		}
		vals[pm.Key] = pm.Num
	}

	// Solar full reading (has energyGain) -> dedicated history with daily accumulation.
	// A pump-only update (SEND_PUMP_STATUS) carries just pumpState -> node_param only.
	if nodeType == nodeTypeSolar {
		if _, full := vals["energyGain"]; full {
			if err := s.recordSolarHistory(tx, nodeID, vals, ts); err != nil {
				return err
			}
		}
	}
	return tx.Commit()
}

// solarMinIntervalS rejects a second history row too close to the previous one.
// The node reports every 2 min, so anything within a minute is a duplicate of the
// SAME reading redelivered by the transport, not new data. That happens for real:
// when the M4F stalls, the CC1310 keeps retrying the frame up over SPI, and once
// the M4F recovers the whole backlog lands at once (observed: 8 identical rows in
// one second after an 11h stall). Since energy_gain accumulates as prev + delta,
// each duplicate would inflate the daily yield by another delta.
//
// A UNIQUE(node_id, reading_time) index alone would NOT be enough: it only catches
// duplicates that share the exact same receive second.
const solarMinIntervalS = 60

// recordSolarHistory appends one solar_history row. energy_gain and pump_runtime
// accumulate on the previous row IF it is the same local day, else they restart
// from this reading (midnight reset in s.loc). energy_gain_delta is the raw 2-min
// gain (kWh*10000); pump_runtime adds the 2-min report interval whenever flow>0.
// Duplicate redeliveries are dropped (see solarMinIntervalS).
func (s *Store) recordSolarHistory(tx *sql.Tx, nodeID uint8, v map[string]float64, ts int64) error {
	delta := int64(v["energyGain"]) // raw 2-min gain, kWh*10000
	pumpInc := int64(0)
	if v["flowRate"] > 0 {
		pumpInc = 2 // minutes (matches the node's 2-min report cadence)
	}

	var prevTime, prevEnergy, prevRuntime sql.NullInt64
	err := tx.QueryRow(
		`SELECT reading_time, energy_gain, pump_runtime FROM solar_history
		 WHERE node_id = ? ORDER BY reading_time DESC LIMIT 1`, nodeID).
		Scan(&prevTime, &prevEnergy, &prevRuntime)
	if err != nil && err != sql.ErrNoRows {
		return err
	}

	if prevTime.Valid && ts-prevTime.Int64 < solarMinIntervalS {
		log.Printf("[Store] solar node %d: duplicate reading %ds after the previous one - skipped",
			nodeID, ts-prevTime.Int64)
		return nil // node_param still got the update; only history must stay clean
	}

	energy, runtime := delta, pumpInc
	if prevTime.Valid && sameLocalDay(prevTime.Int64, ts, s.loc) {
		energy = prevEnergy.Int64 + delta
		runtime = prevRuntime.Int64 + pumpInc
	}

	_, err = tx.Exec(
		`INSERT INTO solar_history
		 (node_id, reading_time, input_temp, output_temp,
		  bufor1_temp, bufor2_temp, bufor3_temp, bufor4_temp, collector_temp,
		  flow_rate, second_pump_state, energy_gain_delta, energy_gain, pump_runtime)
		 VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
		nodeID, ts, v["Tin"], v["Tout"],
		v["T1"], v["T2"], v["T3"], v["T4"], v["Tcol"],
		int64(v["flowRate"]), int64(v["pumpState"]), delta, energy, runtime)
	return err
}

// sameLocalDay reports whether two unix timestamps fall on the same calendar day
// in loc (the daily accumulation reset boundary).
func sameLocalDay(a, b int64, loc *time.Location) bool {
	ya, ma, da := time.Unix(a, 0).In(loc).Date()
	yb, mb, db := time.Unix(b, 0).In(loc).Date()
	return ya == yb && ma == mb && da == db
}

func (s *Store) Close() error {
	return s.db.Close()
}

// GetRulesJSON returns the stored ruleset as the app's JSON array ("[]" if none).
func (s *Store) GetRulesJSON() (string, error) {
	var v string
	err := s.db.QueryRow(`SELECT value FROM config WHERE key = 'rules'`).Scan(&v)
	if err == sql.ErrNoRows {
		return "[]", nil
	}
	if err != nil {
		return "", err
	}
	return v, nil
}

// GetRules parses the stored ruleset into the Rule model (for the engine push).
func (s *Store) GetRules() ([]Rule, error) {
	j, err := s.GetRulesJSON()
	if err != nil {
		return nil, err
	}
	return parseAppRules(j)
}

// SetRules persists the ruleset (stored canonicalized, so getrules round-trips
// cleanly regardless of how the app formatted the request).
func (s *Store) SetRules(rules []Rule) error {
	j, err := marshalAppRules(rules)
	if err != nil {
		return err
	}
	_, err = s.db.Exec(
		`INSERT INTO config(key, value) VALUES('rules', ?)
		 ON CONFLICT(key) DO UPDATE SET value = excluded.value`, j)
	return err
}
