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

	_ "github.com/mattn/go-sqlite3"
)

type Store struct {
	db *sql.DB
}

// OpenStore opens (creating if needed) the SQLite DB and ensures the schema.
func OpenStore(path string) (*Store, error) {
	dsn := path + "?_journal_mode=WAL&_synchronous=NORMAL&_busy_timeout=5000"
	db, err := sql.Open("sqlite3", dsn)
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
	return &Store{db: db}, nil
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
