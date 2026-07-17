package main

// gen1 history import: pull the legacy solar controller's raw readings from the gen1
// MySQL (via a small PHP JSON endpoint) into the raw buffer, then rebuild all
// aggregates from them and drop the raw. gen1 is authoritative simply because we
// recompute everything from its raw each time.
//
// Full recompute, not incremental: every run clears this node's raw, pulls the whole
// series, rebuilds the aggregates, and prunes the raw back to the 2h buffer. Imports
// are rare and dev-only, so a few minutes is fine (point 4 of the design).
//
// No delta reconstruction. gen1 stores extraTemp = yield accumulated over the local
// day (kWh*10000) and pumpRuntime = pump minutes accumulated over the day. We copy
// both verbatim into the raw buffer's cumulative columns; the aggregator diffs the
// cumulative at period boundaries (gen1's own method), which is robust to the sensor
// glitches that a per-reading delta approach blew up into 300 kWh days.

import (
	"crypto/tls"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"net/url"
	"time"
)

// Gen1Row is one row from the gen1 export endpoint (column names are the gen1 MySQL
// names, which lie: pwmValue is flow%, extraTemp is accumulated yield). See
// [[gen1-server-scripts]].
type Gen1Row struct {
	Ts        int64   `json:"ts"` // unix s (UNIX_TIMESTAMP(readingTime), server-local)
	InputTemp float64 `json:"inputTemp"`
	OutputTmp float64 `json:"outputTemp"`
	Bufor1    float64 `json:"bufor1Temp"`
	Bufor2    float64 `json:"bufor2Temp"`
	Bufor3    float64 `json:"bufor3Temp"`
	Bufor4    float64 `json:"bufor4Temp"`
	Collector float64 `json:"collectorTemp"`
	FlowPct   int64   `json:"pwmValue"`    // flow %, despite the name
	PumpState int64   `json:"sPumpState"`  // aux pump relay
	ExtraTemp int64   `json:"extraTemp"`   // accumulated daily yield, kWh*10000
	PumpRun   int64   `json:"pumpRuntime"` // accumulated daily pump minutes
}

const gen1ImportPageSize = 2000

// ImportGen1 clears this node's raw, pulls the full gen1 series, then rebuilds the
// aggregates and prunes the raw. insecure skips TLS validation (the gen1 host's cert
// name mismatches). maxPages bounds the pull (0 = all) for a quick verification run.
func (s *Store) ImportGen1(endpoint, key string, insecure bool, maxPages int) (int, error) {
	// Full recompute: start clean so re-imports do not double-count and gen2's own
	// recent raw does not overlap gen1's (gen1 is authoritative here).
	if _, err := s.db.Exec(`DELETE FROM solar_history WHERE node_id = ?`, solarDefaultNode); err != nil {
		return 0, err
	}
	log.Printf("[gen1] import from %s (full recompute)", endpoint)

	client := &http.Client{Timeout: 30 * time.Second}
	if insecure {
		client.Transport = &http.Transport{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}}
		log.Printf("[gen1] TLS certificate validation DISABLED (-gen1-insecure)")
	}

	total, since := 0, int64(0)
	for page := 0; maxPages == 0 || page < maxPages; page++ {
		rows, err := fetchGen1Page(client, endpoint, key, since, gen1ImportPageSize)
		if err != nil {
			return total, err
		}
		if len(rows) == 0 {
			break
		}
		n, err := s.storeGen1Page(rows)
		if err != nil {
			return total, err
		}
		total += n
		since = rows[len(rows)-1].Ts
		log.Printf("[gen1] page: %d rows (through %s)", len(rows),
			time.Unix(since, 0).In(s.loc).Format("2006-01-02 15:04"))
		if len(rows) < gen1ImportPageSize {
			break
		}
	}

	log.Printf("[gen1] %d raw row(s) loaded; rebuilding aggregates...", total)
	if err := s.RebuildSolarAggregates(solarDefaultNode); err != nil {
		return total, fmt.Errorf("aggregate rebuild after import: %w", err)
	}
	log.Printf("[gen1] aggregates rebuilt, raw pruned to buffer")
	return total, nil
}

func fetchGen1Page(c *http.Client, endpoint, key string, since int64, limit int) ([]Gen1Row, error) {
	u, err := url.Parse(endpoint)
	if err != nil {
		return nil, fmt.Errorf("bad endpoint %q: %w", endpoint, err)
	}
	q := u.Query()
	q.Set("since", fmt.Sprintf("%d", since))
	q.Set("limit", fmt.Sprintf("%d", limit))
	if key != "" {
		q.Set("key", key)
	}
	u.RawQuery = q.Encode()

	resp, err := c.Get(u.String())
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("gen1 endpoint HTTP %d", resp.StatusCode)
	}
	var rows []Gen1Row
	if err := json.NewDecoder(resp.Body).Decode(&rows); err != nil {
		return nil, fmt.Errorf("decode gen1 JSON: %w", err)
	}
	return rows, nil
}

// storeGen1Page inserts a page of gen1 rows verbatim: extraTemp -> energy_gain (the
// cumulative the aggregator diffs), pumpRuntime -> pump_runtime. energy_gain_delta is
// 0 (only the power VIEW reads it, and that always uses a live gen2 row). One tx.
func (s *Store) storeGen1Page(rows []Gen1Row) (int, error) {
	if len(rows) == 0 {
		return 0, nil
	}
	tx, err := s.db.Begin()
	if err != nil {
		return 0, err
	}
	defer tx.Rollback()

	stmt, err := tx.Prepare(
		`INSERT OR IGNORE INTO solar_history
		 (node_id, reading_time, input_temp, output_temp,
		  bufor1_temp, bufor2_temp, bufor3_temp, bufor4_temp, collector_temp,
		  flow_rate, second_pump_state, energy_gain_delta, energy_gain, pump_runtime, source)
		 VALUES (?,?,?,?,?,?,?,?,?,?,?,0,?,?, 'gen1')`)
	if err != nil {
		return 0, err
	}
	defer stmt.Close()

	inserted := 0
	for _, r := range rows {
		res, err := stmt.Exec(
			solarDefaultNode, r.Ts, r.InputTemp, r.OutputTmp,
			r.Bufor1, r.Bufor2, r.Bufor3, r.Bufor4, r.Collector,
			r.FlowPct, r.PumpState, r.ExtraTemp, r.PumpRun)
		if err != nil {
			return 0, err
		}
		if n, _ := res.RowsAffected(); n > 0 {
			inserted++
		}
	}
	return inserted, tx.Commit()
}
