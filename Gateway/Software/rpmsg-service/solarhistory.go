package main

// Solar history: the hourly rollup and the chart series built from it.
//
// The chain is: solar_history (raw 2-min rows) -> solar_rollup (hourly) -> chart
// series (day/month/year/total). Only the hourly step is materialized; the rest
// are sums over it, computed per request.
//
// Everything here derives from energy_gain_delta, the raw per-reading gain. That
// is the one real advantage over gen1: gen1 never stored the delta, so it had to
// reconstruct yields by subtracting an odometer (extraTemp) across a chain of
// three cron scripts, each rounding and stamping periods its own way. Summing
// deltas has no such drift, and it can be recomputed for any range at any time.
//
// All bucketing is done in LOCAL time (s.loc): an "hour", a "day" and a "month"
// are what the user's wall clock says, and DST must not shift them.

import (
	"database/sql"
	"fmt"
	"log"
	"time"
)

// SolarBar is one bucket of a chart series.
type SolarBar struct {
	Bucket      int64   `json:"bucket"`      // unix s, local period start
	EnergyKwh   float64 `json:"energyKwh"`   // real kWh (not the wire's kWh*10000)
	PumpMinutes int64   `json:"pumpMinutes"`
	Samples     int64   `json:"samples"`  // raw readings behind this bucket
	Expected    int64   `json:"expected"` // how many there should have been
}

// SolarSeries is one chart period (a day / a month / a year / everything).
type SolarSeries struct {
	Bucket      int64      `json:"bucket"` // start of the period, local
	Label       string     `json:"label"` // pre-formatted PL (e.g. "12 lip 2026")
	Bars        []SolarBar `json:"bars"`
	EnergyKwh   float64    `json:"energyKwh"`   // total over the WHOLE period
	PumpMinutes int64      `json:"pumpMinutes"` // ditto
	Samples     int64      `json:"samples"`
	Expected    int64      `json:"expected"`
}

// plMonthAbbr are the Polish month abbreviations the design uses ("12 lip 2026").
// The gateway formats the label because commonMain (KMP) has no date library, and
// the gateway is the only side that already holds the correct local-time context.
var plMonthAbbr = [...]string{"", "sty", "lut", "mar", "kwi", "maj", "cze",
	"lip", "sie", "wrz", "paź", "lis", "gru"}

func (s *Store) solarLabel(rng string, bucket, last int64) string {
	t := time.Unix(bucket, 0).In(s.loc)
	switch rng {
	case "day":
		return fmt.Sprintf("%d %s %d", t.Day(), plMonthAbbr[t.Month()], t.Year())
	case "month":
		return fmt.Sprintf("%s %d", plMonthAbbr[t.Month()], t.Year())
	case "year":
		return fmt.Sprintf("%d", t.Year())
	case "total":
		return fmt.Sprintf("%d – %d", t.Year(), time.Unix(last, 0).In(s.loc).Year())
	}
	return ""
}

const (
	solarReadingsPerHour = 30 // 2-min cadence
	solarDayFirstHour    = 6  // the design's day chart runs 6:00..21:00 (16 bars)
	solarDayLastHour     = 21
)

// hourStart snaps a timestamp to the beginning of its local hour.
func (s *Store) hourStart(ts int64) int64 {
	t := time.Unix(ts, 0).In(s.loc)
	return time.Date(t.Year(), t.Month(), t.Day(), t.Hour(), 0, 0, 0, s.loc).Unix()
}

// rebuildSolarHour recomputes ONE hourly bucket from raw rows. Runs inside the
// telemetry transaction, so it sees the row just inserted. Recompute-and-replace
// rather than increment: a redelivered or back-filled reading must not be able to
// double-count.
func (s *Store) rebuildSolarHour(tx *sql.Tx, nodeID uint8, ts int64) error {
	from := s.hourStart(ts)
	to := time.Unix(from, 0).In(s.loc).Add(time.Hour).Unix()

	var energy, pump, samples sql.NullInt64
	err := tx.QueryRow(
		`SELECT COALESCE(SUM(energy_gain_delta),0),
		        COALESCE(SUM(CASE WHEN flow_rate > 0 THEN 2 ELSE 0 END),0),
		        COUNT(*)
		 FROM solar_history
		 WHERE node_id = ? AND reading_time >= ? AND reading_time < ?`,
		nodeID, from, to).Scan(&energy, &pump, &samples)
	if err != nil {
		return err
	}
	_, err = tx.Exec(
		`INSERT INTO solar_rollup (node_id, bucket, energy_gain, pump_runtime, samples)
		 VALUES (?,?,?,?,?)
		 ON CONFLICT(node_id, bucket) DO UPDATE SET
		   energy_gain = excluded.energy_gain,
		   pump_runtime = excluded.pump_runtime,
		   samples = excluded.samples`,
		nodeID, from, energy.Int64, pump.Int64, samples.Int64)
	return err
}

// RebuildSolarRollup recomputes every hourly bucket for a node from raw history.
// Used to backfill the rollup for data that predates it (and, later, after syncing
// gen1 history down). Safe to run at any time - it only ever rewrites.
func (s *Store) RebuildSolarRollup(nodeID uint8) (int, error) {
	rows, err := s.db.Query(
		`SELECT DISTINCT reading_time FROM solar_history WHERE node_id = ? ORDER BY reading_time`, nodeID)
	if err != nil {
		return 0, err
	}
	var stamps []int64
	for rows.Next() {
		var ts int64
		if err := rows.Scan(&ts); err != nil {
			rows.Close()
			return 0, err
		}
		stamps = append(stamps, ts)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return 0, err
	}

	// One bucket per distinct local hour.
	seen := map[int64]bool{}
	tx, err := s.db.Begin()
	if err != nil {
		return 0, err
	}
	defer tx.Rollback()
	n := 0
	for _, ts := range stamps {
		h := s.hourStart(ts)
		if seen[h] {
			continue
		}
		seen[h] = true
		if err := s.rebuildSolarHour(tx, nodeID, ts); err != nil {
			return 0, err
		}
		n++
	}
	return n, tx.Commit()
}

// BackfillSolarRollup rebuilds the rollup for every solar node whose history is
// not covered yet. Runs at startup: it costs nothing once the rollup is current,
// and it means history that predates this feature (or arrived while the rollup
// did not exist) shows up on the charts without anyone running a migration.
func (s *Store) BackfillSolarRollup() error {
	rows, err := s.db.Query(
		`SELECT DISTINCT h.node_id FROM solar_history h
		 WHERE NOT EXISTS (SELECT 1 FROM solar_rollup r WHERE r.node_id = h.node_id)`)
	if err != nil {
		return err
	}
	var ids []uint8
	for rows.Next() {
		var id uint8
		if err := rows.Scan(&id); err != nil {
			rows.Close()
			return err
		}
		ids = append(ids, id)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return err
	}
	for _, id := range ids {
		n, err := s.RebuildSolarRollup(id)
		if err != nil {
			return fmt.Errorf("rollup backfill node %d: %w", id, err)
		}
		log.Printf("[Store] solar node %d: built %d hourly rollup bucket(s) from history", id, n)
	}
	return nil
}

// solarHours reads raw hourly buckets in [from, to).
func (s *Store) solarHours(nodeID uint8, from, to int64) (map[int64]SolarBar, error) {
	rows, err := s.db.Query(
		`SELECT bucket, energy_gain, pump_runtime, samples FROM solar_rollup
		 WHERE node_id = ? AND bucket >= ? AND bucket < ? ORDER BY bucket`,
		nodeID, from, to)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[int64]SolarBar{}
	for rows.Next() {
		var b SolarBar
		var gain int64
		if err := rows.Scan(&b.Bucket, &gain, &b.PumpMinutes, &b.Samples); err != nil {
			return nil, err
		}
		b.EnergyKwh = float64(gain) / 10000.0
		b.Expected = solarReadingsPerHour
		out[b.Bucket] = b
	}
	return out, rows.Err()
}

// SolarHistory builds the chart series for a range: "day", "month", "year" or
// "total". count is how many periods back to return (newest last); it is ignored
// for "total", which is always a single all-time series.
func (s *Store) SolarHistory(nodeID uint8, rng string, count int) ([]SolarSeries, error) {
	first, last, err := s.solarSpan(nodeID)
	if err != nil {
		return nil, err
	}
	if first == 0 {
		return []SolarSeries{}, nil // no data: an empty series, never a fake one
	}
	now := time.Now().In(s.loc)

	// One place to stamp the label, so every branch below stays about numbers.
	label := func(out []SolarSeries) []SolarSeries {
		for i := range out {
			out[i].Label = s.solarLabel(rng, out[i].Bucket, last)
		}
		return out
	}

	switch rng {
	case "day":
		out := []SolarSeries{}
		for i := count - 1; i >= 0; i-- {
			d := now.AddDate(0, 0, -i)
			day := time.Date(d.Year(), d.Month(), d.Day(), 0, 0, 0, 0, s.loc)
			if day.Unix() < s.dayStart(first) {
				continue // before we have any data at all
			}
			ser, err := s.solarDay(nodeID, day)
			if err != nil {
				return nil, err
			}
			out = append(out, ser)
		}
		return label(out), nil

	case "month":
		out := []SolarSeries{}
		for i := count - 1; i >= 0; i-- {
			m := now.AddDate(0, -i, 0)
			month := time.Date(m.Year(), m.Month(), 1, 0, 0, 0, 0, s.loc)
			if month.AddDate(0, 1, 0).Unix() <= first {
				continue
			}
			ser, err := s.solarMonth(nodeID, month)
			if err != nil {
				return nil, err
			}
			out = append(out, ser)
		}
		return label(out), nil

	case "year":
		out := []SolarSeries{}
		firstYear := time.Unix(first, 0).In(s.loc).Year()
		for y := firstYear; y <= now.Year(); y++ {
			if count > 0 && now.Year()-y >= count {
				continue
			}
			ser, err := s.solarYear(nodeID, y)
			if err != nil {
				return nil, err
			}
			out = append(out, ser)
		}
		return label(out), nil

	case "total":
		ser, err := s.solarTotal(nodeID, first, last)
		if err != nil {
			return nil, err
		}
		return label([]SolarSeries{ser}), nil
	}
	return nil, fmt.Errorf("unknown range %q (day|month|year|total)", rng)
}

// solarSpan reports the first and last reading timestamps for a node (0,0 if none).
func (s *Store) solarSpan(nodeID uint8) (int64, int64, error) {
	var first, last sql.NullInt64
	err := s.db.QueryRow(
		`SELECT MIN(reading_time), MAX(reading_time) FROM solar_history WHERE node_id = ?`,
		nodeID).Scan(&first, &last)
	if err != nil {
		return 0, 0, err
	}
	return first.Int64, last.Int64, nil
}

func (s *Store) dayStart(ts int64) int64 {
	t := time.Unix(ts, 0).In(s.loc)
	return time.Date(t.Year(), t.Month(), t.Day(), 0, 0, 0, 0, s.loc).Unix()
}

// solarDay: bars = hours solarDayFirstHour..solarDayLastHour (the design's window).
// The totals cover the WHOLE day, so a stray reading outside the window still
// counts toward the yield even though no bar shows it.
func (s *Store) solarDay(nodeID uint8, day time.Time) (SolarSeries, error) {
	next := day.AddDate(0, 0, 1)
	hours, err := s.solarHours(nodeID, day.Unix(), next.Unix())
	if err != nil {
		return SolarSeries{}, err
	}
	ser := SolarSeries{Bucket: day.Unix()}
	for h := solarDayFirstHour; h <= solarDayLastHour; h++ {
		bucket := time.Date(day.Year(), day.Month(), day.Day(), h, 0, 0, 0, s.loc).Unix()
		b, ok := hours[bucket]
		if !ok {
			b = SolarBar{Bucket: bucket, Expected: solarReadingsPerHour}
		}
		ser.Bars = append(ser.Bars, b)
	}
	for _, b := range hours { // totals over the whole day, not just the window
		ser.EnergyKwh += b.EnergyKwh
		ser.PumpMinutes += b.PumpMinutes
		ser.Samples += b.Samples
	}
	ser.Expected = int64(next.Sub(day).Hours()) * solarReadingsPerHour // DST-aware
	return ser, nil
}

// solarMonth: one bar per day of the month.
func (s *Store) solarMonth(nodeID uint8, month time.Time) (SolarSeries, error) {
	next := month.AddDate(0, 1, 0)
	hours, err := s.solarHours(nodeID, month.Unix(), next.Unix())
	if err != nil {
		return SolarSeries{}, err
	}
	ser := SolarSeries{Bucket: month.Unix()}
	for d := month; d.Before(next); d = d.AddDate(0, 0, 1) {
		dayEnd := d.AddDate(0, 0, 1)
		bar := SolarBar{Bucket: d.Unix(), Expected: int64(dayEnd.Sub(d).Hours()) * solarReadingsPerHour}
		for _, b := range hours {
			if b.Bucket >= d.Unix() && b.Bucket < dayEnd.Unix() {
				bar.EnergyKwh += b.EnergyKwh
				bar.PumpMinutes += b.PumpMinutes
				bar.Samples += b.Samples
			}
		}
		ser.Bars = append(ser.Bars, bar)
		ser.EnergyKwh += bar.EnergyKwh
		ser.PumpMinutes += bar.PumpMinutes
		ser.Samples += bar.Samples
		ser.Expected += bar.Expected
	}
	return ser, nil
}

// solarYear: one bar per month.
func (s *Store) solarYear(nodeID uint8, year int) (SolarSeries, error) {
	start := time.Date(year, time.January, 1, 0, 0, 0, 0, s.loc)
	next := start.AddDate(1, 0, 0)
	hours, err := s.solarHours(nodeID, start.Unix(), next.Unix())
	if err != nil {
		return SolarSeries{}, err
	}
	ser := SolarSeries{Bucket: start.Unix()}
	for m := start; m.Before(next); m = m.AddDate(0, 1, 0) {
		mEnd := m.AddDate(0, 1, 0)
		bar := SolarBar{Bucket: m.Unix(), Expected: int64(mEnd.Sub(m).Hours()) * solarReadingsPerHour}
		for _, b := range hours {
			if b.Bucket >= m.Unix() && b.Bucket < mEnd.Unix() {
				bar.EnergyKwh += b.EnergyKwh
				bar.PumpMinutes += b.PumpMinutes
				bar.Samples += b.Samples
			}
		}
		ser.Bars = append(ser.Bars, bar)
		ser.EnergyKwh += bar.EnergyKwh
		ser.PumpMinutes += bar.PumpMinutes
		ser.Samples += bar.Samples
		ser.Expected += bar.Expected
	}
	return ser, nil
}

// solarTotal: one bar per year, over everything we hold.
func (s *Store) solarTotal(nodeID uint8, first, last int64) (SolarSeries, error) {
	y0 := time.Unix(first, 0).In(s.loc).Year()
	y1 := time.Unix(last, 0).In(s.loc).Year()
	ser := SolarSeries{Bucket: time.Date(y0, time.January, 1, 0, 0, 0, 0, s.loc).Unix()}
	for y := y0; y <= y1; y++ {
		ys, err := s.solarYear(nodeID, y)
		if err != nil {
			return SolarSeries{}, err
		}
		ser.Bars = append(ser.Bars, SolarBar{
			Bucket: ys.Bucket, EnergyKwh: ys.EnergyKwh, PumpMinutes: ys.PumpMinutes,
			Samples: ys.Samples, Expected: ys.Expected,
		})
		ser.EnergyKwh += ys.EnergyKwh
		ser.PumpMinutes += ys.PumpMinutes
		ser.Samples += ys.Samples
		ser.Expected += ys.Expected
	}
	return ser, nil
}
