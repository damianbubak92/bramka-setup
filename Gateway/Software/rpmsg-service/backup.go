package main

// Live backup to the external mirror (Gateway/Software/server). Every change to a durable table
// is captured by an SQLite trigger into backup_queue; a worker drains the queue to
// gw-backup.php with retry, so changes made while the server is unreachable are held
// and delivered when it returns. A fresh gateway rebuilds itself from gw-restore.php.
//
// Triggers (not call-site hooks) so nothing can be forgotten: every INSERT/UPDATE/
// DELETE on the mirrored tables enqueues automatically, including future code paths.
// Installed only when backup is enabled, else the queue would grow with no drainer.

import (
	"bytes"
	"crypto/tls"
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"time"
)

type BackupConfig struct {
	PushURL  string // gw-backup.php
	Key      string
	Insecure bool
	Interval time.Duration
}

const backupBatch = 500

// mirroredTables lists the tables that trigger a backup enqueue. Order matters only
// for readability; the queue itself is ordered by insert id.
//
// Each entry: the trigger payload (json_object args) for INSERT/UPDATE upserts. node
// DELETE enqueues a purge_node (wipes the node across the mirror in one shot); the
// aggregates and node_param have no delete trigger - they are only removed via a node
// purge or a dev-only rebuild that re-inserts.
var backupTriggerSQL = []string{
	// config (backup_* keys are gateway-local bookkeeping - never mirror them)
	`CREATE TRIGGER IF NOT EXISTS bq_config_i AFTER INSERT ON config WHEN NEW.key NOT LIKE 'backup%' BEGIN
		INSERT INTO backup_queue(kind,op,payload) VALUES('config','upsert',
			json_object('key',NEW.key,'value',NEW.value)); END`,
	`CREATE TRIGGER IF NOT EXISTS bq_config_u AFTER UPDATE ON config WHEN NEW.key NOT LIKE 'backup%' BEGIN
		INSERT INTO backup_queue(kind,op,payload) VALUES('config','upsert',
			json_object('key',NEW.key,'value',NEW.value)); END`,
	`CREATE TRIGGER IF NOT EXISTS bq_config_d AFTER DELETE ON config WHEN OLD.key NOT LIKE 'backup%' BEGIN
		INSERT INTO backup_queue(kind,op,payload) VALUES('config','delete',
			json_object('key',OLD.key)); END`,
	// node
	`CREATE TRIGGER IF NOT EXISTS bq_node_i AFTER INSERT ON node BEGIN
		INSERT INTO backup_queue(kind,op,payload) VALUES('node','upsert',
			json_object('node_id',NEW.node_id,'node_type',NEW.node_type,'name',NEW.name,
				'factory_id',NEW.factory_id,'status',NEW.status,'provisioned_at',NEW.provisioned_at,
				'last_seen',NEW.last_seen,'room',NEW.room)); END`,
	`CREATE TRIGGER IF NOT EXISTS bq_node_u AFTER UPDATE ON node BEGIN
		INSERT INTO backup_queue(kind,op,payload) VALUES('node','upsert',
			json_object('node_id',NEW.node_id,'node_type',NEW.node_type,'name',NEW.name,
				'factory_id',NEW.factory_id,'status',NEW.status,'provisioned_at',NEW.provisioned_at,
				'last_seen',NEW.last_seen,'room',NEW.room)); END`,
	`CREATE TRIGGER IF NOT EXISTS bq_node_d AFTER DELETE ON node BEGIN
		INSERT INTO backup_queue(kind,op,payload) VALUES('purge_node','delete',
			json_object('node_id',OLD.node_id)); END`,
	// node_param (current state)
	`CREATE TRIGGER IF NOT EXISTS bq_np_i AFTER INSERT ON node_param BEGIN
		INSERT INTO backup_queue(kind,op,payload) VALUES('node_param','upsert',
			json_object('node_id',NEW.node_id,'param_key',NEW.param_key,'value_num',NEW.value_num,'ts',NEW.ts)); END`,
	`CREATE TRIGGER IF NOT EXISTS bq_np_u AFTER UPDATE ON node_param BEGIN
		INSERT INTO backup_queue(kind,op,payload) VALUES('node_param','upsert',
			json_object('node_id',NEW.node_id,'param_key',NEW.param_key,'value_num',NEW.value_num,'ts',NEW.ts)); END`,
	// solar aggregates
	`CREATE TRIGGER IF NOT EXISTS bq_sh_i AFTER INSERT ON solar_hourly BEGIN
		INSERT INTO backup_queue(kind,op,payload) VALUES('solar_hourly','upsert',
			json_object('node_id',NEW.node_id,'bucket',NEW.bucket,'hour_yield',NEW.hour_yield,
				'hour_pump',NEW.hour_pump,'day_yield',NEW.day_yield,'day_pump',NEW.day_pump)); END`,
	`CREATE TRIGGER IF NOT EXISTS bq_sh_u AFTER UPDATE ON solar_hourly BEGIN
		INSERT INTO backup_queue(kind,op,payload) VALUES('solar_hourly','upsert',
			json_object('node_id',NEW.node_id,'bucket',NEW.bucket,'hour_yield',NEW.hour_yield,
				'hour_pump',NEW.hour_pump,'day_yield',NEW.day_yield,'day_pump',NEW.day_pump)); END`,
	`CREATE TRIGGER IF NOT EXISTS bq_sd_i AFTER INSERT ON solar_daily BEGIN
		INSERT INTO backup_queue(kind,op,payload) VALUES('solar_daily','upsert',
			json_object('node_id',NEW.node_id,'bucket',NEW.bucket,'day_yield',NEW.day_yield,
				'month_yield',NEW.month_yield,'month_pump',NEW.month_pump)); END`,
	`CREATE TRIGGER IF NOT EXISTS bq_sd_u AFTER UPDATE ON solar_daily BEGIN
		INSERT INTO backup_queue(kind,op,payload) VALUES('solar_daily','upsert',
			json_object('node_id',NEW.node_id,'bucket',NEW.bucket,'day_yield',NEW.day_yield,
				'month_yield',NEW.month_yield,'month_pump',NEW.month_pump)); END`,
	`CREATE TRIGGER IF NOT EXISTS bq_sm_i AFTER INSERT ON solar_monthly BEGIN
		INSERT INTO backup_queue(kind,op,payload) VALUES('solar_monthly','upsert',
			json_object('node_id',NEW.node_id,'bucket',NEW.bucket,'month_yield',NEW.month_yield,
				'year_yield',NEW.year_yield,'year_pump',NEW.year_pump)); END`,
	`CREATE TRIGGER IF NOT EXISTS bq_sm_u AFTER UPDATE ON solar_monthly BEGIN
		INSERT INTO backup_queue(kind,op,payload) VALUES('solar_monthly','upsert',
			json_object('node_id',NEW.node_id,'bucket',NEW.bucket,'month_yield',NEW.month_yield,
				'year_yield',NEW.year_yield,'year_pump',NEW.year_pump)); END`,
}

var backupTriggerNames = []string{
	"bq_config_i", "bq_config_u", "bq_config_d", "bq_node_i", "bq_node_u", "bq_node_d",
	"bq_np_i", "bq_np_u", "bq_sh_i", "bq_sh_u", "bq_sd_i", "bq_sd_u", "bq_sm_i", "bq_sm_u",
}

// InstallBackupTriggers turns on change-capture. Idempotent. First probes json_object:
// the triggers build their payload with it, and a trigger that errors at fire time
// would abort the very INSERT/UPDATE that fired it (i.e. break telemetry). If JSON1 is
// missing we refuse to install (backup stays off) rather than risk the writes.
func (s *Store) InstallBackupTriggers() error {
	if _, err := s.db.Exec(`SELECT json_object('probe', 1)`); err != nil {
		return fmt.Errorf("SQLite JSON1 unavailable, backup disabled: %w", err)
	}
	for _, q := range backupTriggerSQL {
		if _, err := s.db.Exec(q); err != nil {
			return fmt.Errorf("install backup trigger: %w", err)
		}
	}
	return nil
}

// RemoveBackupTriggers turns off change-capture (so backup_queue stops growing when
// backup is disabled).
func (s *Store) RemoveBackupTriggers() {
	for _, n := range backupTriggerNames {
		s.db.Exec("DROP TRIGGER IF EXISTS " + n)
	}
}

// SeedBackupFromCurrentState enqueues the whole current DB once, so the mirror starts
// complete (triggers only capture future changes). Guarded by a backup_seeded marker
// so restarts do not re-enqueue two years of aggregates every time.
func (s *Store) SeedBackupFromCurrentState() error {
	var seeded sql.NullString
	s.db.QueryRow(`SELECT value FROM config WHERE key = 'backup_seeded'`).Scan(&seeded)
	if seeded.Valid {
		return nil // already seeded on a previous run
	}
	seeds := []struct{ kind, sql string }{
		{"config", `INSERT INTO backup_queue(kind,op,payload) SELECT 'config','upsert',
			json_object('key',key,'value',value) FROM config WHERE key NOT LIKE 'backup%'`},
		{"node", `INSERT INTO backup_queue(kind,op,payload) SELECT 'node','upsert',
			json_object('node_id',node_id,'node_type',node_type,'name',name,'factory_id',factory_id,
				'status',status,'provisioned_at',provisioned_at,'last_seen',last_seen,'room',room) FROM node`},
		{"node_param", `INSERT INTO backup_queue(kind,op,payload) SELECT 'node_param','upsert',
			json_object('node_id',node_id,'param_key',param_key,'value_num',value_num,'ts',ts) FROM node_param`},
		{"solar_hourly", `INSERT INTO backup_queue(kind,op,payload) SELECT 'solar_hourly','upsert',
			json_object('node_id',node_id,'bucket',bucket,'hour_yield',hour_yield,'hour_pump',hour_pump,
				'day_yield',day_yield,'day_pump',day_pump) FROM solar_hourly`},
		{"solar_daily", `INSERT INTO backup_queue(kind,op,payload) SELECT 'solar_daily','upsert',
			json_object('node_id',node_id,'bucket',bucket,'day_yield',day_yield,'month_yield',month_yield,
				'month_pump',month_pump) FROM solar_daily`},
		{"solar_monthly", `INSERT INTO backup_queue(kind,op,payload) SELECT 'solar_monthly','upsert',
			json_object('node_id',node_id,'bucket',bucket,'month_yield',month_yield,'year_yield',year_yield,
				'year_pump',year_pump) FROM solar_monthly`},
	}
	for _, sd := range seeds {
		if _, err := s.db.Exec(sd.sql); err != nil {
			return fmt.Errorf("seed backup %s: %w", sd.kind, err)
		}
	}
	_, err := s.db.Exec(`INSERT OR REPLACE INTO config(key,value) VALUES('backup_seeded', ?)`,
		fmt.Sprintf("%d", time.Now().Unix()))
	return err
}

func httpClient(insecure bool) *http.Client {
	c := &http.Client{Timeout: 30 * time.Second}
	if insecure {
		c.Transport = &http.Transport{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}}
	}
	return c
}

type backupItem struct {
	Kind string          `json:"kind"`
	Op   string          `json:"op"`
	Data json.RawMessage `json:"data"`
}

// backupWorker drains backup_queue to the mirror with retry. One goroutine; runs
// every cfg.Interval and pushes in batches until the queue is empty or a push fails
// (kept for the next tick).
func backupWorker(store *Store, cfg BackupConfig) {
	client := httpClient(cfg.Insecure)
	log.Printf("[Backup] live backup -> %s (every %s)", cfg.PushURL, cfg.Interval)
	t := time.NewTicker(cfg.Interval)
	defer t.Stop()
	for range t.C {
		for {
			ids, items, err := store.readBackupBatch(backupBatch)
			if err != nil {
				log.Printf("[Backup] read queue failed: %v", err)
				break
			}
			if len(items) == 0 {
				break
			}
			if err := pushBackup(client, cfg, items); err != nil {
				log.Printf("[Backup] push deferred (%d pending): %v", len(items), err)
				break // server unreachable - keep the rows, retry next tick
			}
			if err := store.deleteBackupRows(ids); err != nil {
				log.Printf("[Backup] queue cleanup failed: %v", err)
				break
			}
			if len(items) < backupBatch {
				break
			}
		}
	}
}

func (s *Store) readBackupBatch(limit int) ([]int64, []backupItem, error) {
	rows, err := s.db.Query(
		`SELECT id, kind, op, payload FROM backup_queue ORDER BY id LIMIT ?`, limit)
	if err != nil {
		return nil, nil, err
	}
	defer rows.Close()
	var ids []int64
	var items []backupItem
	for rows.Next() {
		var id int64
		var it backupItem
		var payload string
		if err := rows.Scan(&id, &it.Kind, &it.Op, &payload); err != nil {
			return nil, nil, err
		}
		it.Data = json.RawMessage(payload)
		ids = append(ids, id)
		items = append(items, it)
	}
	return ids, items, rows.Err()
}

func (s *Store) deleteBackupRows(ids []int64) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	stmt, err := tx.Prepare(`DELETE FROM backup_queue WHERE id = ?`)
	if err != nil {
		return err
	}
	defer stmt.Close()
	for _, id := range ids {
		if _, err := stmt.Exec(id); err != nil {
			return err
		}
	}
	return tx.Commit()
}

func pushBackup(client *http.Client, cfg BackupConfig, items []backupItem) error {
	body, err := json.Marshal(map[string]any{"key": cfg.Key, "items": items})
	if err != nil {
		return err
	}
	resp, err := client.Post(cfg.PushURL, "application/json", bytes.NewReader(body))
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	msg, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("HTTP %d: %s", resp.StatusCode, msg)
	}
	var r struct {
		OK bool `json:"ok"`
	}
	if json.Unmarshal(msg, &r); !r.OK {
		return fmt.Errorf("server rejected: %s", msg)
	}
	return nil
}

// ---- restore ----

type mirrorDump struct {
	Config       []map[string]any `json:"config"`
	Node         []map[string]any `json:"node"`
	NodeParam    []map[string]any `json:"node_param"`
	SolarHourly  []map[string]any `json:"solar_hourly"`
	SolarDaily   []map[string]any `json:"solar_daily"`
	SolarMonthly []map[string]any `json:"solar_monthly"`
}

// RestoreFromMirror pulls the whole mirror and repopulates this (fresh) gateway's DB.
// param_def re-seeds itself on schema init; the 2h raw buffer is not restored (it
// refills from live telemetry). Backup triggers must NOT be installed yet, or the
// restore would re-enqueue everything.
func (s *Store) RestoreFromMirror(restoreURL, key string, insecure bool) error {
	client := httpClient(insecure)
	resp, err := client.Get(restoreURL + "?key=" + key)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		b, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return fmt.Errorf("restore HTTP %d: %s", resp.StatusCode, b)
	}
	var dump mirrorDump
	if err := json.NewDecoder(resp.Body).Decode(&dump); err != nil {
		return fmt.Errorf("decode restore: %w", err)
	}

	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()

	type target struct {
		table string
		cols  []string
		rows  []map[string]any
	}
	targets := []target{
		{"config", []string{"key", "value"}, dump.Config},
		{"node", []string{"node_id", "node_type", "name", "factory_id", "status", "provisioned_at", "last_seen", "room"}, dump.Node},
		{"node_param", []string{"node_id", "param_key", "value_num", "ts"}, dump.NodeParam},
		{"solar_hourly", []string{"node_id", "bucket", "hour_yield", "hour_pump", "day_yield", "day_pump"}, dump.SolarHourly},
		{"solar_daily", []string{"node_id", "bucket", "day_yield", "month_yield", "month_pump"}, dump.SolarDaily},
		{"solar_monthly", []string{"node_id", "bucket", "month_yield", "year_yield", "year_pump"}, dump.SolarMonthly},
	}
	total := 0
	for _, t := range targets {
		ph := make([]string, len(t.cols))
		q := "INSERT OR REPLACE INTO " + t.table + " ("
		for i, c := range t.cols {
			ph[i] = "?"
			if i > 0 {
				q += ","
			}
			q += c
		}
		q += ") VALUES (" + joinStrings(ph, ",") + ")"
		stmt, err := tx.Prepare(q)
		if err != nil {
			return err
		}
		for _, row := range t.rows {
			args := make([]any, len(t.cols))
			for i, c := range t.cols {
				args[i] = row[c]
			}
			if _, err := stmt.Exec(args...); err != nil {
				stmt.Close()
				return fmt.Errorf("restore %s: %w", t.table, err)
			}
			total++
		}
		stmt.Close()
		log.Printf("[Restore] %s: %d row(s)", t.table, len(t.rows))
	}
	if err := tx.Commit(); err != nil {
		return err
	}
	log.Printf("[Restore] done: %d row(s) total", total)
	return nil
}

func joinStrings(parts []string, sep string) string {
	out := ""
	for i, p := range parts {
		if i > 0 {
			out += sep
		}
		out += p
	}
	return out
}
