// DB monitor: a live view of the whole SQLite database + a journal of every write.
//
// Why it exists: this DB is the backbone of the system (telemetry, node registry,
// rules, history). Debugging it blind let junk accumulate (orphan node_param rows,
// duplicate solar_history timestamps, a dead `sample` table). Seeing the tables and
// WHO writes WHAT, live, turns those into obvious findings.
//
// Two design choices worth knowing:
//   - The journal is driven by SQLite's own update hook, not by instrumenting call
//     sites: it therefore catches EVERY insert/update/delete on EVERY table,
//     including ones we forget about and tables added later (no code to touch).
//   - The hook runs INSIDE the write, so it only drops an event into a buffered
//     channel and never blocks: the monitor can't slow telemetry down.
//
// It is DEV-ONLY: enabled with -db-monitor. It exposes the whole database (and a
// SQL console) over HTTP, so production must not run with the flag.
package main

import (
	"context"
	"database/sql"
	_ "embed"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"regexp"
	"strconv"
	"strings"
	"time"

	sqlite3 "github.com/mattn/go-sqlite3"
)

//go:embed dbmonitor.html
var dbMonitorPage []byte

// driverWithUpdateHook is a sqlite3 driver that reports row changes. Registered
// once; OpenStore always uses it (harmless when no hook is installed).
const driverWithUpdateHook = "sqlite3_hooked"

// dbChangeHook is set by Store.OnChange. Package-level because the update hook is
// installed per-connection at driver level, and we run a single Store with a single
// connection (db.SetMaxOpenConns(1)).
var dbChangeHook func(op int, table string, rowid int64)

func init() {
	sql.Register(driverWithUpdateHook, &sqlite3.SQLiteDriver{
		ConnectHook: func(conn *sqlite3.SQLiteConn) error {
			conn.RegisterUpdateHook(func(op int, _ string, table string, rowid int64) {
				if h := dbChangeHook; h != nil {
					h(op, table, rowid)
				}
			})
			return nil
		},
	})
}

// DBEvent is one row change, as seen by SQLite itself.
type DBEvent struct {
	Op    string `json:"op"`    // insert | update | delete
	Table string `json:"table"`
	RowID int64  `json:"rowid"`
	Ts    int64  `json:"ts"` // unix ms (the journal wants sub-second ordering)
}

func opName(op int) string {
	switch op {
	case sqlite3.SQLITE_INSERT:
		return "insert"
	case sqlite3.SQLITE_UPDATE:
		return "update"
	case sqlite3.SQLITE_DELETE:
		return "delete"
	default:
		return "?"
	}
}

// OnChange starts forwarding row changes to fn. The update hook itself must stay
// fast (it runs inside the write), so it only offers to a buffered channel and
// drops events when the consumer lags - a busy monitor must never stall telemetry.
func (s *Store) OnChange(fn func(DBEvent)) {
	ch := make(chan DBEvent, 512)
	go func() {
		for ev := range ch {
			fn(ev)
		}
	}()
	dbChangeHook = func(op int, table string, rowid int64) {
		ev := DBEvent{Op: opName(op), Table: table, RowID: rowid, Ts: time.Now().UnixMilli()}
		select {
		case ch <- ev:
		default: // journal is behind; dropping beats blocking a DB write
		}
	}
}

// ---------- HTTP ----------

type dbColumn struct {
	Name    string `json:"name"`
	Type    string `json:"type"`
	PK      bool   `json:"pk"`
	NotNull bool   `json:"notNull"`
}

type dbTable struct {
	Name    string     `json:"name"`
	Kind    string     `json:"kind"` // table | view
	Count   int64      `json:"count"`
	Columns []dbColumn `json:"columns"`
	SQL     string     `json:"sql"`
}

type dbQueryResult struct {
	Columns  []string   `json:"columns"`
	Rows     [][]any    `json:"rows"`
	Affected int64      `json:"affected"`
	Duration int64      `json:"durationMs"`
	Error    string     `json:"error,omitempty"`
	Truncated bool      `json:"truncated"`
}

const (
	dbMaxRows     = 1000            // cap the payload; the UI paginates anyway
	dbQueryTimeout = 10 * time.Second // MaxOpenConns(1): a slow query stalls telemetry
)

// registerDBMonitor wires the monitor routes onto mux. Called only with -db-monitor.
func registerDBMonitor(mux *http.ServeMux, store *Store, token string) {
	auth := func(w http.ResponseWriter, r *http.Request) bool {
		if r.URL.Query().Get("token") != token {
			w.WriteHeader(http.StatusUnauthorized)
			io.WriteString(w, "Odmowa")
			return false
		}
		return true
	}

	mux.HandleFunc("/db", func(w http.ResponseWriter, r *http.Request) {
		if !auth(w, r) {
			return
		}
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write(dbMonitorPage)
	})

	mux.HandleFunc("/db/api/schema", func(w http.ResponseWriter, r *http.Request) {
		if !auth(w, r) {
			return
		}
		tables, err := dbSchema(store.db)
		writeJSON(w, tables, err)
	})

	mux.HandleFunc("/db/api/rows", func(w http.ResponseWriter, r *http.Request) {
		if !auth(w, r) {
			return
		}
		table := r.URL.Query().Get("table")
		if !dbTableExists(store.db, table) { // never interpolate an unvetted name
			w.WriteHeader(http.StatusBadRequest)
			io.WriteString(w, "unknown table")
			return
		}
		limit := atoiOr(r.URL.Query().Get("limit"), 100)
		offset := atoiOr(r.URL.Query().Get("offset"), 0)
		// Newest first where there is an obvious ordering key.
		order := ""
		for _, c := range []string{"id", "ts", "reading_time", "last_seen"} {
			if dbHasColumn(store.db, table, c) {
				order = " ORDER BY " + c + " DESC"
				break
			}
		}
		q := fmt.Sprintf("SELECT * FROM %q%s LIMIT %d OFFSET %d", table, order, limit, offset)
		res := dbRunQuery(store.db, q)
		writeJSON(w, res, nil)
	})

	// SQL console. Deliberately unrestricted (dev tool) - hence the -db-monitor flag.
	// Every statement is logged so there is always an audit trail of what touched the DB.
	mux.HandleFunc("/db/api/query", func(w http.ResponseWriter, r *http.Request) {
		if !auth(w, r) {
			return
		}
		body, _ := io.ReadAll(io.LimitReader(r.Body, 64<<10))
		var in struct {
			SQL string `json:"sql"`
		}
		if err := json.Unmarshal(body, &in); err != nil {
			writeJSON(w, dbQueryResult{Error: "bad JSON"}, nil)
			return
		}
		stmt := strings.TrimSpace(in.SQL)
		if stmt == "" {
			writeJSON(w, dbQueryResult{Error: "pusty SQL"}, nil)
			return
		}
		log.Printf("[DBMON] exec: %s", singleLine(stmt))
		writeJSON(w, dbRunQuery(store.db, stmt), nil)
	})

	log.Printf("[DBMON] enabled: /db (dev tool - exposes the whole DB + SQL console)")
}

// dbRunQuery runs one statement. SELECT-like statements return rows; everything
// else reports rows affected (phpMyAdmin-ish behaviour).
func dbRunQuery(db *sql.DB, stmt string) dbQueryResult {
	ctx, cancel := context.WithTimeout(context.Background(), dbQueryTimeout)
	defer cancel()
	start := time.Now()
	out := dbQueryResult{Columns: []string{}, Rows: [][]any{}}

	if isReadStatement(stmt) {
		rows, err := db.QueryContext(ctx, stmt)
		if err != nil {
			out.Error = err.Error()
			out.Duration = time.Since(start).Milliseconds()
			return out
		}
		defer rows.Close()
		cols, err := rows.Columns()
		if err != nil {
			out.Error = err.Error()
			return out
		}
		out.Columns = cols
		for rows.Next() {
			if len(out.Rows) >= dbMaxRows {
				out.Truncated = true
				break
			}
			vals := make([]any, len(cols))
			ptrs := make([]any, len(cols))
			for i := range vals {
				ptrs[i] = &vals[i]
			}
			if err := rows.Scan(ptrs...); err != nil {
				out.Error = err.Error()
				break
			}
			for i, v := range vals {
				if b, ok := v.([]byte); ok { // TEXT/BLOB arrive as []byte -> JSON base64
					vals[i] = string(b)
				}
			}
			out.Rows = append(out.Rows, vals)
		}
		if err := rows.Err(); err != nil && out.Error == "" {
			out.Error = err.Error()
		}
		out.Duration = time.Since(start).Milliseconds()
		return out
	}

	res, err := db.ExecContext(ctx, stmt)
	if err != nil {
		out.Error = err.Error()
	} else if n, e := res.RowsAffected(); e == nil {
		out.Affected = n
	}
	out.Duration = time.Since(start).Milliseconds()
	return out
}

// dbWriteKeyword spots the writing tail of a CTE (see isReadStatement).
var dbWriteKeyword = regexp.MustCompile(`(?i)\b(insert|update|delete|replace)\b`)

func isReadStatement(stmt string) bool {
	s := strings.ToLower(strings.TrimLeft(stmt, " \t\r\n("))
	switch {
	case strings.HasPrefix(s, "select"), strings.HasPrefix(s, "pragma"),
		strings.HasPrefix(s, "explain"):
		return true
	case strings.HasPrefix(s, "with"):
		// A CTE may end in SELECT *or* in INSERT/UPDATE/DELETE. Only the former can
		// take the query path; sending a write down it would report "0 rows" instead
		// of doing the work. When in doubt, take the write path: it always executes.
		return !dbWriteKeyword.MatchString(s)
	}
	return false
}

func dbSchema(db *sql.DB) ([]dbTable, error) {
	rows, err := db.Query(
		`SELECT name, type, COALESCE(sql,'') FROM sqlite_master
		 WHERE type IN ('table','view') AND name NOT LIKE 'sqlite_%'
		 ORDER BY type, name`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []dbTable
	for rows.Next() {
		var t dbTable
		if err := rows.Scan(&t.Name, &t.Kind, &t.SQL); err != nil {
			return nil, err
		}
		out = append(out, t)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	// Columns + counts per object (introspection, so new tables just show up).
	for i := range out {
		out[i].Columns = dbColumns(db, out[i].Name)
		_ = db.QueryRow(fmt.Sprintf("SELECT COUNT(*) FROM %q", out[i].Name)).Scan(&out[i].Count)
	}
	return out, nil
}

func dbColumns(db *sql.DB, table string) []dbColumn {
	rows, err := db.Query(fmt.Sprintf("PRAGMA table_info(%q)", table))
	if err != nil {
		return nil
	}
	defer rows.Close()
	cols := []dbColumn{}
	for rows.Next() {
		var cid int
		var name, ctype string
		var notnull, pk int
		var dflt sql.NullString
		if err := rows.Scan(&cid, &name, &ctype, &notnull, &dflt, &pk); err != nil {
			return cols
		}
		cols = append(cols, dbColumn{Name: name, Type: ctype, PK: pk > 0, NotNull: notnull > 0})
	}
	return cols
}

func dbTableExists(db *sql.DB, name string) bool {
	if name == "" {
		return false
	}
	var n int
	err := db.QueryRow(
		`SELECT COUNT(*) FROM sqlite_master WHERE name = ? AND type IN ('table','view')`, name).Scan(&n)
	return err == nil && n > 0
}

func dbHasColumn(db *sql.DB, table, col string) bool {
	for _, c := range dbColumns(db, table) {
		if c.Name == col {
			return true
		}
	}
	return false
}

func writeJSON(w http.ResponseWriter, v any, err error) {
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		io.WriteString(w, err.Error())
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(v)
}

func atoiOr(s string, def int) int {
	if n, err := strconv.Atoi(s); err == nil && n >= 0 {
		return n
	}
	return def
}

func singleLine(s string) string {
	return strings.Join(strings.Fields(s), " ")
}
