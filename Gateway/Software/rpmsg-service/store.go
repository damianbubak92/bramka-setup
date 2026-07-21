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

	// backup_queue: pending pushes to the external mirror (see backup.go). Filled by
	// triggers on the mirrored tables (installed only when backup is enabled), drained
	// by the backup worker with retry - so a change made while the server is offline is
	// held here and delivered when it comes back.
	if _, err := db.Exec(`CREATE TABLE IF NOT EXISTS backup_queue (
		id      INTEGER PRIMARY KEY AUTOINCREMENT,
		kind    TEXT NOT NULL,
		op      TEXT NOT NULL,
		payload TEXT NOT NULL
	)`); err != nil {
		db.Close()
		return nil, fmt.Errorf("init backup_queue: %w", err)
	}

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
		// Three aggregate levels, gen1's proven model (SolarSystem{Daily,Monthly,
		// Annual}Stats). Each row = one period; each level derives from the one below by
		// diffing the CUMULATIVE energy at period boundaries (never summing per-reading
		// deltas - that is what made a single sensor glitch explode a day to 300 kWh).
		// solar_history is only a ~2h rolling buffer feeding these; the aggregates ARE
		// the durable truth. Yields stored in real kWh (raw keeps the *10000 int form);
		// pump in minutes. Per-node keyed, so provisioning/removal is one cascade.
		//
		//   solar_hourly  : 1 row per hour  -> Day chart   (day_yield = cum. since local midnight)
		//   solar_daily   : 1 row per day   -> Month chart (month_yield = cum. since 1st of month)
		//   solar_monthly : 1 row per month -> Year + Total (year_yield = cum. since January)
		`CREATE TABLE IF NOT EXISTS solar_hourly (
			node_id    INTEGER NOT NULL,
			bucket     INTEGER NOT NULL,  -- unix s, local hour start
			hour_yield REAL NOT NULL,     -- kWh produced in this hour
			hour_pump  INTEGER NOT NULL DEFAULT 0, -- pump minutes in this hour
			day_yield  REAL NOT NULL,     -- kWh cumulative (chain reference for the diff)
			day_pump   INTEGER NOT NULL,  -- pump minutes cumulative (chain reference)
			PRIMARY KEY (node_id, bucket)
		)`,
		`CREATE TABLE IF NOT EXISTS solar_daily (
			node_id     INTEGER NOT NULL,
			bucket      INTEGER NOT NULL,  -- unix s, local day start
			day_yield   REAL NOT NULL,     -- kWh this day
			month_yield REAL NOT NULL,     -- kWh cumulative since 1st of month
			month_pump  INTEGER NOT NULL,  -- pump minutes cumulative this month
			PRIMARY KEY (node_id, bucket)
		)`,
		`CREATE TABLE IF NOT EXISTS solar_monthly (
			node_id     INTEGER NOT NULL,
			bucket      INTEGER NOT NULL,  -- unix s, local month start
			month_yield REAL NOT NULL,     -- kWh this month
			year_yield  REAL NOT NULL,     -- kWh cumulative since January
			year_pump   INTEGER NOT NULL,  -- pump minutes cumulative this year
			PRIMARY KEY (node_id, bucket)
		)`,
		`DROP TABLE IF EXISTS solar_rollup`, // superseded by the three-level aggregates
		// DROP+CREATE (not IF NOT EXISTS): a VIEW is only a definition, so recreating
		// it every start keeps it in sync when we add columns (e.g. energy_gain_kwh).
		`DROP VIEW IF EXISTS solar_state`,
		`CREATE VIEW solar_state AS
			SELECT node_id,
			       bufor1_temp, bufor2_temp, bufor3_temp, bufor4_temp,
			       collector_temp, flow_rate,
			       30.0 * energy_gain_delta / 10000.0 AS generated_power_kw,
			       energy_gain / 10000.0 AS energy_gain_kwh,  -- accumulated this local day
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
	// migrate: node.status - lifecycle state (pending_join | active | detached |
	// legacy). NULL on pre-existing rows is treated as 'active'.
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
	// migrate: solar_history.source - which system produced the row ('gen2' live, or
	// 'gen1' back-filled from the legacy MySQL). The rollup uses gen1 for any hour it
	// covers (gen1 is the source of truth with full coverage) and gen2 only where gen1
	// has none - so a gen2 hang gap is auto-healed by gen1. Pre-existing rows = 'gen2'.
	if _, err := db.Exec(`ALTER TABLE solar_history ADD COLUMN source TEXT NOT NULL DEFAULT 'gen2'`); err != nil &&
		!strings.Contains(err.Error(), "duplicate column name") {
		return err
	}
	// migrate: solar_hourly.hour_pump - per-hour pump minutes. The daily total sums
	// per-hour increments (robust when gen1's reset lands inside our local day), so we
	// need the per-hour value, not just the cumulative day_pump.
	if _, err := db.Exec(`ALTER TABLE solar_hourly ADD COLUMN hour_pump INTEGER NOT NULL DEFAULT 0`); err != nil &&
		!strings.Contains(err.Error(), "duplicate column name") {
		return err
	}
	// migrate: decouple the stable logical id from the RF address (Docs/NODE-MANAGEMENT.md).
	if err := migrateNodeIdentity(db); err != nil {
		return err
	}
	// migrate: node.archived_at - LOCAL trash (soft-delete). NULL = live; a unix ts = in
	// the trash (kept row + history, address freed) awaiting restore or the 60-day purge.
	// Makes trash/restore work without the external mirror (economy/standard tiers); the
	// mirror stays a backup that reflects archived_at via the upsert triggers.
	if _, err := db.Exec(`ALTER TABLE node ADD COLUMN archived_at INTEGER`); err != nil &&
		!strings.Contains(err.Error(), "duplicate column name") {
		return err
	}
	// migrate: node.capabilities - bitmask of node-executable actions the node declared at
	// JOIN (NODE_CAP(ACTION_*), automation.h). Drives the app's Action editor; NULL/0 =
	// sensor-only node. Populated on approve from joinData.capabilities.
	if _, err := db.Exec(`ALTER TABLE node ADD COLUMN capabilities INTEGER`); err != nil &&
		!strings.Contains(err.Error(), "duplicate column name") {
		return err
	}
	// One-time: automations moved from device-TYPE codes to per-node (node_id). Old
	// type-based rules are incompatible with the new format, so clear them once. A marker
	// keeps rules created in the new format from being wiped on later restarts.
	var rulesV2 int
	_ = db.QueryRow(`SELECT COUNT(*) FROM config WHERE key = 'rules_v2'`).Scan(&rulesV2)
	if rulesV2 == 0 {
		if _, err := db.Exec(`DELETE FROM config WHERE key = 'rules'`); err != nil {
			return err
		}
		if _, err := db.Exec(`INSERT OR REPLACE INTO config(key, value) VALUES('rules_v2', '1')`); err != nil {
			return err
		}
	}
	// migrate: tag the gen1 sniff controllers (solar 0xF1, buffer 0xF2) as `legacy`.
	// Keyed by their fixed gen1 addresses - the reliable signal (factory_id is unreliable
	// here: these got one from manual registration, while the 0xF3 dev sim node has none).
	// The gen2 pool is 0x10-0xEF, so 0xF1/0xF2 are exclusively gen1. Legacy nodes are
	// grandfathered: passive sniff, no (addr,factory_id) validation, no MSG_UNREGISTERED
	// (Docs/NODE-MANAGEMENT §13). Idempotent.
	if _, err := db.Exec(
		`UPDATE node SET status = 'legacy'
		 WHERE address IN (241, 242) AND COALESCE(status,'') <> 'legacy'`); err != nil {
		return err
	}
	return seedParamDefs(db)
}

// migrateNodeIdentity splits node.node_id (which USED to be the RF address) into a
// stable logical id + a separate, reusable `address`. After this, node_id is a
// never-reused AUTOINCREMENT logical id (history and rules hang off it) and `address`
// is the RF routing field (0x10-0xEF, NULL when a node is detached). See
// Docs/NODE-MANAGEMENT.md. One-time recreate (a rowid PK cannot become AUTOINCREMENT
// via ALTER). Existing nodes KEEP their node_id and get address = node_id (they were
// the same), so NO history re-keying is needed - history stays keyed by the same
// node_id values. New nodes get an AUTOINCREMENT node_id above the current max,
// distinct from their pool-allocated address.
func migrateNodeIdentity(db *sql.DB) error {
	rows, err := db.Query(`PRAGMA table_info(node)`)
	if err != nil {
		return err
	}
	hasAddress := false
	for rows.Next() {
		var cid, notnull, pk int
		var name, ctype string
		var dflt sql.NullString
		if err := rows.Scan(&cid, &name, &ctype, &notnull, &dflt, &pk); err != nil {
			rows.Close()
			return err
		}
		if name == "address" {
			hasAddress = true
		}
	}
	rows.Close()
	if hasAddress {
		return nil // already migrated
	}

	steps := []string{
		`CREATE TABLE node_new (
			node_id        INTEGER PRIMARY KEY AUTOINCREMENT,  -- STABLE logical id, NOT the RF address
			address        INTEGER,                            -- RF address 0x10-0xEF; NULL = detached
			factory_id     TEXT,
			node_type      INTEGER NOT NULL,
			name           TEXT,
			room           TEXT,
			status         TEXT,
			provisioned_at INTEGER,
			last_seen      INTEGER
		)`,
		// Existing node_id was the address -> preserve it as the logical id AND seed the
		// address with the same value. pending_remove collapses to active (that state is
		// gone in the new model).
		`INSERT INTO node_new (node_id, address, factory_id, node_type, name, room, status, provisioned_at, last_seen)
		 SELECT node_id, node_id, factory_id, node_type, name, room,
		        CASE WHEN status = 'pending_remove' THEN 'active' ELSE COALESCE(status, 'active') END,
		        provisioned_at, last_seen
		 FROM node`,
		`DROP TABLE node`,
		`ALTER TABLE node_new RENAME TO node`,
		`CREATE UNIQUE INDEX IF NOT EXISTS ux_node_addr    ON node(address)    WHERE address    IS NOT NULL`,
		`CREATE UNIQUE INDEX IF NOT EXISTS ux_node_factory ON node(factory_id) WHERE factory_id IS NOT NULL`,
	}
	tx, err := db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	for _, q := range steps {
		if _, err := tx.Exec(q); err != nil {
			return fmt.Errorf("node identity migration: %w", err)
		}
	}
	if err := tx.Commit(); err != nil {
		return err
	}
	log.Printf("[Store] node identity migrated: node_id is now a stable logical id, address is separate")
	return nil
}

// nodeIDForAddress resolves an RF address to the stable logical node_id. ok=false if
// no node currently holds that address.
func (s *Store) nodeIDForAddress(address uint8) (int64, bool, error) {
	var id int64
	err := s.db.QueryRow(`SELECT node_id FROM node WHERE address = ?`, address).Scan(&id)
	if err == sql.ErrNoRows {
		return 0, false, nil
	}
	if err != nil {
		return 0, false, err
	}
	return id, true, nil
}

// addressForNode resolves a logical node_id to its current RF address (0, false if the
// node is detached / has no address).
func (s *Store) addressForNode(id int64) (uint8, bool, error) {
	var addr sql.NullInt64
	err := s.db.QueryRow(`SELECT address FROM node WHERE node_id = ?`, id).Scan(&addr)
	if err == sql.ErrNoRows || !addr.Valid {
		return 0, false, nil
	}
	if err != nil {
		return 0, false, err
	}
	return uint8(addr.Int64), true, nil
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
func (s *Store) ProvisionNode(nodeType uint8, name, factoryID string, capabilities uint32, ts int64) (uint8, error) {
	tx, err := s.db.Begin()
	if err != nil {
		return 0, err
	}
	defer tx.Rollback()

	// Known LIVE chip -> reuse its logical node + address (idempotent re-approve / rejoin).
	// Archived (trashed) chips are excluded: those come back via restore/repair, not approve.
	var addr sql.NullInt64
	err = tx.QueryRow(`SELECT address FROM node WHERE factory_id = ? AND archived_at IS NULL`, factoryID).Scan(&addr)
	if err != nil && err != sql.ErrNoRows {
		return 0, err
	}
	if err != sql.ErrNoRows {
		// Re-approve of a known chip: keep its node_id/address + update name/type.
		// A node mid-removal is revived to active; a detached node needs a fresh
		// address (handled by the replace/re-pair path, not here).
		// capabilities: overwrite only when the JOIN declared something (!=0); a node that
		// declares 0 (e.g. firmware not yet sending caps) keeps its known capabilities so a
		// re-JOIN / re-pair doesn't silently wipe them. Real declarations always win.
		if _, err := tx.Exec(
			`UPDATE node SET name = ?, node_type = ?,
			     capabilities = CASE WHEN ? != 0 THEN ? ELSE capabilities END,
			     provisioned_at = ?, last_seen = ?,
			     status = CASE WHEN status = 'pending_remove' THEN 'active' ELSE status END
			 WHERE factory_id = ?`, name, nodeType, capabilities, capabilities, ts, ts, factoryID); err != nil {
			return 0, err
		}
		return uint8(addr.Int64), tx.Commit()
	}

	newAddr, err := allocAddr(tx)
	if err != nil {
		return 0, err
	}
	// New logical node: node_id is AUTOINCREMENT (a stable id, distinct from the
	// reusable RF address). pending_join until it confirms under the assigned address.
	if _, err := tx.Exec(
		`INSERT INTO node (address, node_type, name, factory_id, capabilities, provisioned_at, last_seen, status)
		 VALUES (?, ?, ?, ?, ?, ?, ?, 'pending_join')`, newAddr, nodeType, name, factoryID, capabilities, ts, ts); err != nil {
		return 0, err
	}
	return newAddr, tx.Commit()
}

// ReplaceNode swaps a dead chip for a fresh one on an EXISTING address. History is
// keyed by node_id (the address), not by factory_id, so re-pointing the address to
// the new chip keeps all of the old node's history, name and room automatically. The
// new chip then gets JOIN_ACCEPT with this same address and starts reporting under it.
// status -> pending_join until the new chip confirms. Returns an error if the target
// address is not a provisioned node.
func (s *Store) ReplaceNode(targetAddr uint8, newFactoryID string, capabilities uint32, ts int64) error {
	// capabilities: keep existing when the new chip declared 0 (see ProvisionNode note).
	res, err := s.db.Exec(
		`UPDATE node SET factory_id = ?,
		     capabilities = CASE WHEN ? != 0 THEN ? ELSE capabilities END,
		     status = 'pending_join', provisioned_at = ?, last_seen = ?
		 WHERE address = ?`, newFactoryID, capabilities, capabilities, ts, ts, targetAddr)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return fmt.Errorf("no node at address 0x%02X to replace", targetAddr)
	}
	return nil
}

// RepairNode re-pairs a fresh chip onto a DETACHED node (restored from trash),
// reclaiming its stable node_id + history. Unlike ReplaceNode (which reuses the
// still-live address of an active node), a detached node has no address, so a fresh
// one is allocated. status -> pending_join until the new chip confirms. Returns the
// newly allocated RF address, or an error if the node is not detached.
func (s *Store) RepairNode(nodeID int64, newFactoryID string, capabilities uint32, ts int64) (uint8, error) {
	tx, err := s.db.Begin()
	if err != nil {
		return 0, err
	}
	defer tx.Rollback()

	var status string
	var archivedAt sql.NullInt64
	err = tx.QueryRow(`SELECT COALESCE(status,'active'), archived_at FROM node WHERE node_id = ?`, nodeID).Scan(&status, &archivedAt)
	if err == sql.ErrNoRows {
		return 0, fmt.Errorf("no node with id %d", nodeID)
	}
	if err != nil {
		return 0, err
	}
	if archivedAt.Valid {
		return 0, fmt.Errorf("node %d is in trash - restore it first", nodeID)
	}
	if status != "detached" {
		return 0, fmt.Errorf("node %d is %s, not detached (use replace for an active node)", nodeID, status)
	}
	// A chip is one node: if this factory_id is already bound to another node the
	// partial-unique index would reject the UPDATE with a raw SQL error. Pre-check so
	// the user gets a clear message (remove that node first, or use a fresh chip).
	var otherID int64
	var otherName sql.NullString
	switch e := tx.QueryRow(
		`SELECT node_id, name FROM node WHERE factory_id = ? AND node_id <> ? AND archived_at IS NULL`,
		newFactoryID, nodeID).Scan(&otherID, &otherName); e {
	case nil:
		return 0, fmt.Errorf("chip already assigned to node %d (%s) - remove it first or use a new chip",
			otherID, otherName.String)
	case sql.ErrNoRows:
		// good: chip is free
	default:
		return 0, e
	}
	newAddr, err := allocAddr(tx)
	if err != nil {
		return 0, err
	}
	// capabilities: keep existing when the re-paired chip declared 0 (see ProvisionNode note).
	if _, err := tx.Exec(
		`UPDATE node SET address = ?, factory_id = ?,
		     capabilities = CASE WHEN ? != 0 THEN ? ELSE capabilities END,
		     status = 'pending_join', provisioned_at = ?, last_seen = ? WHERE node_id = ?`,
		newAddr, newFactoryID, capabilities, capabilities, ts, ts, nodeID); err != nil {
		return 0, err
	}
	return newAddr, tx.Commit()
}

// NodeStatus returns a node's lifecycle state + identity (for the telemetry-path
// state machine). status is 'active' for pre-migration rows (NULL).
// UpdateNode sets the user-facing label and room of an already registered node.
// Both are pure labels - the node knows nothing about either. The phone's device
// editor saves them together, so this is one call (and one round trip).
func (s *Store) UpdateNode(address uint8, name, room string) error {
	_, err := s.db.Exec(`UPDATE node SET name = ?, room = ? WHERE address = ?`, name, room, address)
	return err
}

// FactoryStatus returns the lifecycle status of the node currently bound to a chip
// (factory_id hex), or ok=false if no node has that chip. Used to silence a JOIN from
// an already-active node (user pressed the button on a working device by accident).
func (s *Store) FactoryStatus(factoryID string) (status string, ok bool) {
	var st sql.NullString
	e := s.db.QueryRow(
		`SELECT COALESCE(status,'active') FROM node WHERE factory_id = ? AND archived_at IS NULL`,
		factoryID).Scan(&st)
	if e != nil {
		return "", false
	}
	return st.String, true
}

func (s *Store) NodeStatus(address uint8) (status, factoryID string, nodeType uint8, exists bool, err error) {
	var st, fid sql.NullString
	var nt int
	e := s.db.QueryRow(
		`SELECT COALESCE(status,'active'), COALESCE(factory_id,''), node_type
		 FROM node WHERE address = ?`, address).Scan(&st, &fid, &nt)
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
		`UPDATE node SET status = 'active' WHERE address = ? AND status = 'pending_join'`, address)
	return err
}

// NodeInfo is one provisioned node for the phone's device list (listnodes).
type NodeInfo struct {
	NodeID        int64  `json:"id"`           // stable logical id (survives address/chip changes)
	Address       int    `json:"address"`      // current RF address (reusable)
	Type          int    `json:"type"`         // NODE_* (app maps to a label)
	Name          string `json:"name"`         // user label set at approval
	FactoryID     string `json:"factory"`      // chip identity (hex)
	Status        string `json:"status"`       // pending_join | active | detached | legacy
	LastSeen      int64  `json:"lastSeen"`     // unix s of last telemetry (0 = never)
	ProvisionedAt int64  `json:"provisionedAt"` // unix s
	Room          string `json:"room"`         // user grouping ("" = Bez pokoju)
	Capabilities  uint32 `json:"capabilities"` // bitmask NODE_CAP(ACTION_*): actions the app may target on this node
}

// NodeState is the LAST KNOWN telemetry of one node, as stored in node_param.
// The phone reads this once on open so every field (temperatures, pump states,
// toggles) shows real values immediately, without waiting for the next 2-minute
// WS telemetry push.
type NodeState struct {
	NodeID  int64              `json:"id"`      // stable logical id
	Address int                `json:"address"` // current RF address
	Type    int                `json:"type"`    // NODE_* (from the node row RecordTelemetry upserts)
	Params  map[string]float64 `json:"params"`  // param_key -> value (same keys as the WS telemetry event)
	Ts      int64              `json:"ts"`      // unix s of the newest param in this node
	// PowerKw is the derived instantaneous power for solar nodes, taken from the
	// solar_state VIEW (30 * energy_gain_delta / 10000, i.e. the last 2-minute
	// delta scaled to an hour). Sent so the phone can show real power immediately
	// on open instead of waiting for a second reading to diff. nil = not a solar
	// node / no history yet.
	PowerKw *float64 `json:"powerKw,omitempty"`
	// EnergyDayKwh is the yield accumulated so far this local day (solar_state VIEW,
	// energy_gain/10000). The telemetry `energyGain` param is only the raw 2-min
	// delta, so the phone cannot derive the daily figure itself. nil = not solar /
	// no history yet.
	EnergyDayKwh *float64 `json:"energyDayKwh,omitempty"`
}

// ListState returns the current state of every node we have telemetry for.
// Mirrors the shape of the WS "telemetry" event so the app can seed its live
// state with the same code path.
func (s *Store) ListState() ([]NodeState, error) {
	// Unknown type MUST NOT default to 0: NODE_SOLAR_CONTROLLER is 0, so an orphan
	// node_param row (no node row) would masquerade as the solar controller and the
	// app would render its missing params as zeros. -1 = unknown, matches no NODE_*.
	rows, err := s.db.Query(
		`SELECT p.node_id, COALESCE(n.address, 0), COALESCE(n.node_type, -1), p.param_key,
		        COALESCE(p.value_num, 0), p.ts
		 FROM node_param p
		 LEFT JOIN node n ON n.node_id = p.node_id
		 WHERE n.archived_at IS NULL
		 ORDER BY p.node_id, p.param_key`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	byID := make(map[int64]*NodeState)
	order := make([]int64, 0, 8)
	for rows.Next() {
		var id int64
		var address, typ int
		var key string
		var val float64
		var ts int64
		if err := rows.Scan(&id, &address, &typ, &key, &val, &ts); err != nil {
			return nil, err
		}
		st, ok := byID[id]
		if !ok {
			st = &NodeState{NodeID: id, Address: address, Type: typ, Params: make(map[string]float64)}
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
	if prows, perr := s.db.Query(`SELECT node_id, generated_power_kw, energy_gain_kwh FROM solar_state`); perr == nil {
		defer prows.Close()
		for prows.Next() {
			var id int64
			var kw, day sql.NullFloat64
			if err := prows.Scan(&id, &kw, &day); err != nil {
				continue
			}
			st, ok := byID[id]
			if !ok {
				continue
			}
			if kw.Valid {
				v := kw.Float64
				st.PowerKw = &v
			}
			if day.Valid {
				v := day.Float64
				st.EnergyDayKwh = &v
			}
		}
	}

	out := make([]NodeState, 0, len(order))
	for _, id := range order {
		out = append(out, *byID[id])
	}
	return out, nil
}

// ListNodes returns the gen2 nodes for the phone's "devices" screen: active,
// pending_join and detached (restored-from-trash, awaiting re-pair - factory_id NULL
// but a real logical node the user should see and pair). Only `legacy` gen1 sniff
// nodes are excluded - they belong on the Solar screen, not Devices. last_seen is
// kept fresh by RecordTelemetry.
func (s *Store) ListNodes() ([]NodeInfo, error) {
	rows, err := s.db.Query(
		`SELECT node_id, COALESCE(address,0), node_type, COALESCE(name,''), COALESCE(factory_id,''),
		        COALESCE(status,'active'), COALESCE(last_seen,0), COALESCE(provisioned_at,0),
		        COALESCE(room,''), COALESCE(capabilities,0)
		 FROM node
		 WHERE archived_at IS NULL
		 ORDER BY node_id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []NodeInfo{}
	for rows.Next() {
		var n NodeInfo
		if err := rows.Scan(&n.NodeID, &n.Address, &n.Type, &n.Name, &n.FactoryID,
			&n.Status, &n.LastSeen, &n.ProvisionedAt, &n.Room, &n.Capabilities); err != nil {
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
		`SELECT COALESCE(factory_id,''), node_type FROM node WHERE address = ?`, address).
		Scan(&fid, &nt)
	if e == sql.ErrNoRows {
		return "", 0, false, nil
	}
	if e != nil {
		return "", 0, false, e
	}
	return fid.String, uint8(nt), true, nil
}

// DeleteNode removes a node LOCALLY: its registry row (freeing the RF address at
// once) and, for a solar node, its raw + aggregate history - so nothing is left
// behind on the gateway (compact lifecycle, immediate removal per the reactive
// model). Its node_param current state is left as-is (harmless; a re-provision
// overwrites). On the MIRROR the row + history are kept (the bq_node_d trigger
// enqueues archive_node = soft-delete): that is the trash, from which RestoreNode
// can bring the node back within the retention window.
// DeleteNode SOFT-deletes: the node goes to the LOCAL trash (archived_at set), its
// row + history are kept for restore, and its RF address is freed (NULL) for reuse.
// factory_id is kept so the node remembers its chip (own chip -> "Przywróć"). The
// history is dropped only by the 60-day purge (PurgeExpiredTrash), the one hard-delete.
func (s *Store) DeleteNode(address uint8) error {
	id, ok, err := s.nodeIDForAddress(address)
	if err != nil {
		return err
	}
	if !ok {
		return nil // already gone
	}
	return s.DeleteNodeByID(id)
}

// DeleteNodeByID SOFT-deletes by the stable node_id. Needed for DETACHED nodes (no RF
// address, so DeleteNode's address lookup can't reach them) - without this a detached
// node that never gets re-paired would be an undeletable corpse. Moves it to the local
// trash (history + chip kept for restore/purge).
func (s *Store) DeleteNodeByID(id int64) error {
	_, err := s.db.Exec(
		`UPDATE node SET archived_at = ?, address = NULL WHERE node_id = ? AND archived_at IS NULL`,
		time.Now().Unix(), id)
	return err
}

// ListTrashLocal returns the LOCAL trash (soft-deleted nodes). Works without the mirror,
// so the trash feature is available to every tier. Shape matches the app's TrashNodeDto.
func (s *Store) ListTrashLocal() ([]ArchivedNode, error) {
	rows, err := s.db.Query(
		`SELECT node_id, node_type, COALESCE(name,''), COALESCE(room,''), COALESCE(factory_id,''),
		        COALESCE(last_seen,0), COALESCE(archived_at,0)
		 FROM node WHERE archived_at IS NOT NULL ORDER BY archived_at DESC`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []ArchivedNode{}
	for rows.Next() {
		var a ArchivedNode
		if err := rows.Scan(&a.NodeID, &a.Type, &a.Name, &a.Room, &a.Factory, &a.LastSeen, &a.ArchivedAt); err != nil {
			return nil, err
		}
		out = append(out, a)
	}
	return out, rows.Err()
}

// RestoreNodeLocal brings a node back from the LOCAL trash as `detached` (keeps its stable
// id, history and factory_id; awaits re-pair). The upsert trigger re-pushes archived_at=NULL,
// un-archiving it on the mirror too (self-healing).
func (s *Store) RestoreNodeLocal(id int64) error {
	res, err := s.db.Exec(
		`UPDATE node SET archived_at = NULL, status = 'detached' WHERE node_id = ? AND archived_at IS NOT NULL`, id)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return fmt.Errorf("node %d not in trash", id)
	}
	return nil
}

// PurgeExpiredTrash hard-deletes trashed nodes older than maxAge (60-day retention) and
// drops their history. This is the ONLY hard delete of a node. Returns how many were purged.
func (s *Store) PurgeExpiredTrash(maxAge time.Duration) (int, error) {
	cutoff := time.Now().Add(-maxAge).Unix()
	rows, err := s.db.Query(`SELECT node_id FROM node WHERE archived_at IS NOT NULL AND archived_at < ?`, cutoff)
	if err != nil {
		return 0, err
	}
	var ids []int64
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			rows.Close()
			return 0, err
		}
		ids = append(ids, id)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return 0, err
	}
	for _, id := range ids {
		if err := s.dropNode(id); err != nil {
			return len(ids), err
		}
	}
	return len(ids), nil
}

// dropNode PERMANENTLY removes a node and every trace of it: it deletes the node's rows
// from EVERY table that has a node_id column (node, node_param, and whatever per-type
// history it left - e.g. solar_*). The tables are discovered from the schema, so a new
// node type with its own history table is cleaned automatically, with no per-type code.
// A node that kept no history simply leaves node + node_param, which is all this removes.
// (param_def is keyed by node_type, not node_id, so the shared catalog is never touched.)
func (s *Store) dropNode(id int64) error {
	rows, err := s.db.Query(`SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'`)
	if err != nil {
		return err
	}
	var tables []string
	for rows.Next() {
		var t string
		if err := rows.Scan(&t); err != nil {
			rows.Close()
			return err
		}
		tables = append(tables, t)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return err
	}
	for _, t := range tables {
		if !s.tableHasColumn(t, "node_id") {
			continue
		}
		if _, err := s.db.Exec("DELETE FROM "+t+" WHERE node_id = ?", id); err != nil {
			return fmt.Errorf("dropNode %d from %s: %w", id, t, err)
		}
	}
	return nil
}

// tableHasColumn reports whether table has a column named col. The table name comes from
// the schema (sqlite_master), never user input, so the PRAGMA string build is safe.
func (s *Store) tableHasColumn(table, col string) bool {
	rows, err := s.db.Query("PRAGMA table_info(" + table + ")")
	if err != nil {
		return false
	}
	defer rows.Close()
	for rows.Next() {
		var cid, notnull, pk int
		var name, ctype string
		var dflt sql.NullString
		if rows.Scan(&cid, &name, &ctype, &notnull, &dflt, &pk) == nil && name == col {
			return true
		}
	}
	return false
}

// allocAddr returns the lowest free pool address (freed addresses are reused).
// "Free" = no node row holds that address. Removal frees the address immediately
// (the node goes to the trash keyed by its stable id, not its address; detached
// nodes hold address=NULL) - so the pool never freezes because of the trash. The
// hazard of a reused address (a removed-but-offline node returning on it) is closed
// reactively by the wire contract: a frame's (addr, factory_id) must match the live
// binding or the gateway sends MSG_UNREGISTERED (Docs/NODE-MANAGEMENT §5.2, firmware
// step §12.2). Until that lands, removal best-effort tells an online node to clear
// itself. Runs inside the tx.
func allocAddr(tx *sql.Tx) (uint8, error) {
	rows, err := tx.Query(
		`SELECT address FROM node WHERE address BETWEEN ? AND ?`, addrPoolFirst, addrPoolLast)
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
// full reading, appends a solar_history row, all in one transaction. `address` is the
// RF source address; it is resolved to the stable node_id under which state and
// history are keyed. The node must already be registered (the drain gates on
// NodeStatus), so the resolution always finds a row. ts is unix s.
func (s *Store) RecordTelemetry(address, nodeType uint8, params []NodeParam, ts int64) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()

	var nodeID int64
	if err := tx.QueryRow(`SELECT node_id FROM node WHERE address = ?`, address).Scan(&nodeID); err != nil {
		return fmt.Errorf("telemetry from unregistered address 0x%02X: %w", address, err)
	}
	if _, err := tx.Exec(
		`UPDATE node SET node_type = ?, last_seen = ? WHERE node_id = ?`,
		nodeType, ts, nodeID); err != nil {
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

	// Solar full reading (has energyGain) -> raw buffer with daily accumulation.
	// A pump-only update (SEND_PUMP_STATUS) carries just pumpState -> node_param only.
	solarFull := false
	if nodeType == nodeTypeSolar {
		if _, full := vals["energyGain"]; full {
			if err := s.recordSolarHistory(tx, nodeID, vals, ts); err != nil {
				return err
			}
			solarFull = true
		}
	}
	if err := tx.Commit(); err != nil {
		return err
	}

	// Roll up completed hours/days/months and prune the raw buffer AFTER the raw row
	// is durably committed: an aggregation error must not lose the reading, it just
	// leaves the buffer to be retried on the next ingest.
	if solarFull {
		if err := s.aggregateSolar(nodeID, ts); err != nil {
			log.Printf("[Store] solar node %d: aggregation deferred (%v) - raw buffer kept", nodeID, err)
		}
	}
	return nil
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

// recordSolarHistory appends one solar_history row (the ~2h raw buffer), then rolls
// completed hours/days/months into the aggregates and prunes old raw. energy_gain is
// the yield accumulated since local midnight (kWh*10000): the node sends the 2-min
// delta, we add it, resetting at midnight. pump_runtime accumulates 2 min whenever
// that delta is non-zero (gen1's rule - the pump can have stopped, flow=0, while a
// little yield still lands; matching it keeps gen1 and gen2 numbers comparable).
// energy_gain_delta keeps the last 2-min delta for the power VIEW only.
// Duplicate redeliveries are dropped (see solarMinIntervalS).
func (s *Store) recordSolarHistory(tx *sql.Tx, nodeID int64, v map[string]float64, ts int64) error {
	delta := int64(v["energyGain"]) // raw 2-min gain, kWh*10000
	pumpInc := int64(0)
	if delta > 0 {
		pumpInc = 2 // gen1 rule: pump minutes accrue on non-zero yield, not on flow
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
	// Aggregation + pruning happen AFTER this tx commits (see RecordTelemetry): if
	// they fail the raw row must survive so the next ingest can retry - the whole
	// point of "aggregation failed => keep the buffer".
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

// RulesForPush returns the ENABLED, address-resolved ruleset to push to the M4F engine.
// (node_id -> current RF address; rules with an unresolvable node are dropped.)
func (s *Store) RulesForPush() ([]Rule, error) {
	j, err := s.GetRulesJSON()
	if err != nil {
		return nil, err
	}
	in, err := parseAppRules(j)
	if err != nil {
		return nil, err
	}
	return appRulesToWire(in, s), nil
}

// SetRulesJSON validates and stores the RAW app JSON blob (node_id-based). getrules
// returns it verbatim; resolution to RF addresses happens only at push (RulesForPush),
// so the stored rules survive address reuse / re-pair.
func (s *Store) SetRulesJSON(raw string) error {
	if _, err := parseAppRules(raw); err != nil {
		return err
	}
	_, err := s.db.Exec(
		`INSERT INTO config(key, value) VALUES('rules', ?)
		 ON CONFLICT(key) DO UPDATE SET value = excluded.value`, raw)
	return err
}
