package main

// Solar aggregation - gen1's proven three-level model (SolarSystem{Daily,Monthly,
// Annual}Stats), ported into the service so it is compact per node (no system cron):
// a node's own telemetry drives its aggregation, and removing the node drops all of
// it in one cascade.
//
// The method is gen1's, exactly: never sum per-reading deltas (a single glitch could
// explode a day). Instead each level diffs the CUMULATIVE energy at period
// boundaries, clamped >= 0. solar_history is only a ~2h rolling buffer feeding this;
// the aggregate tables are the durable truth. Yields are stored in real kWh; the raw
// buffer keeps the *10000 int form the wire uses.
//
// Live composition (read side) stacks the last record of each coarser level plus the
// raw tail of the one in-progress period - so a chart value updates every 2 min
// without waiting for the period to close.

import (
	"database/sql"
	"fmt"
	"log"
	"time"
)

const (
	solarDayFirstHour  = 6  // Day chart window (design): 6:00..21:00, 16 bars
	solarDayLastHour   = 21
	solarRawBufferSec  = 2 * 3600 // keep this much raw behind the newest reading
	solarKwhScale      = 10000.0  // wire/raw energy is kWh * 10000
)

// ---- API-facing shapes (unchanged JSON so the app is untouched) ----

type SolarBar struct {
	Bucket      int64   `json:"bucket"` // unix s, local period start
	EnergyKwh   float64 `json:"energyKwh"`
	PumpMinutes int64   `json:"pumpMinutes"`
	Samples     int64   `json:"samples"`  // reserved (coverage), 0 for now
	Expected    int64   `json:"expected"` // reserved
}

type SolarSeries struct {
	Bucket      int64      `json:"bucket"`
	Label       string     `json:"label"`
	Bars        []SolarBar `json:"bars"`
	EnergyKwh   float64    `json:"energyKwh"`   // total over the whole period (live)
	PumpMinutes int64      `json:"pumpMinutes"` // ditto
	Samples     int64      `json:"samples"`
	Expected    int64      `json:"expected"`
}

var plMonthAbbr = [...]string{"", "sty", "lut", "mar", "kwi", "maj", "cze",
	"lip", "sie", "wrz", "paź", "lis", "gru"}

// ---- local-time helpers ----

func (s *Store) local(ts int64) time.Time { return time.Unix(ts, 0).In(s.loc) }

func (s *Store) hourStart(ts int64) int64 {
	t := s.local(ts)
	return time.Date(t.Year(), t.Month(), t.Day(), t.Hour(), 0, 0, 0, s.loc).Unix()
}
func (s *Store) dayStart(ts int64) int64 {
	t := s.local(ts)
	return time.Date(t.Year(), t.Month(), t.Day(), 0, 0, 0, 0, s.loc).Unix()
}
func (s *Store) monthStart(ts int64) int64 {
	t := s.local(ts)
	return time.Date(t.Year(), t.Month(), 1, 0, 0, 0, 0, s.loc).Unix()
}
func (s *Store) nextHour(bucket int64) int64  { return s.local(bucket).Add(time.Hour).Unix() }
func (s *Store) nextDay(bucket int64) int64   { return s.local(bucket).AddDate(0, 0, 1).Unix() }
func (s *Store) nextMonth(bucket int64) int64 { return s.local(bucket).AddDate(0, 1, 0).Unix() }

func sameLocalMonth(a, b int64, loc *time.Location) bool {
	ta, tb := time.Unix(a, 0).In(loc), time.Unix(b, 0).In(loc)
	return ta.Year() == tb.Year() && ta.Month() == tb.Month()
}
func sameLocalYear(a, b int64, loc *time.Location) bool {
	return time.Unix(a, 0).In(loc).Year() == time.Unix(b, 0).In(loc).Year()
}

// ================= write side =================

// aggregateSolar rolls up every hour/day/month that has completed for this node, then
// prunes the raw buffer. Own transaction: called AFTER the raw row is committed, so a
// failure here never loses the reading - it just leaves the buffer for a retry.
func (s *Store) aggregateSolar(node int64, ts int64) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if err := s.aggregateHoursUpTo(tx, node, s.hourStart(ts)); err != nil {
		return err
	}
	if err := s.aggregateDaysUpTo(tx, node, s.dayStart(ts)); err != nil {
		return err
	}
	if err := s.aggregateMonthsUpTo(tx, node, s.monthStart(ts)); err != nil {
		return err
	}
	if err := s.pruneSolarRaw(tx, node, ts); err != nil {
		return err
	}
	return tx.Commit()
}

// RebuildSolarAggregates wipes and recomputes all three levels for a node from the
// raw buffer, then prunes it. Used after a gen1 import (which loads the full raw,
// rebuilds, and drops it). gen1 is authoritative simply because we recompute from its
// raw every time.
func (s *Store) RebuildSolarAggregates(node int64) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	for _, t := range []string{"solar_hourly", "solar_daily", "solar_monthly"} {
		if _, err := tx.Exec("DELETE FROM "+t+" WHERE node_id = ?", node); err != nil {
			return err
		}
	}
	now := time.Now().Unix()
	if err := s.aggregateHoursUpTo(tx, node, s.hourStart(now)); err != nil {
		return err
	}
	if err := s.aggregateDaysUpTo(tx, node, s.dayStart(now)); err != nil {
		return err
	}
	if err := s.aggregateMonthsUpTo(tx, node, s.monthStart(now)); err != nil {
		return err
	}
	if err := s.pruneSolarRaw(tx, node, now); err != nil {
		return err
	}
	return tx.Commit()
}

// distinctBucketsAfter returns the local period buckets (via startFn) that appear in
// `table` for the node, are strictly before `boundary`, and after `after`.
func (s *Store) distinctBuckets(tx *sql.Tx, table string, node int64, timeCol string,
	after, boundary int64, startFn func(int64) int64) ([]int64, error) {
	rows, err := tx.Query(
		"SELECT "+timeCol+" FROM "+table+" WHERE node_id = ? AND "+timeCol+" >= ? AND "+timeCol+" < ? ORDER BY "+timeCol,
		node, after, boundary)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []int64
	seen := make(map[int64]bool)
	for rows.Next() {
		var t int64
		if err := rows.Scan(&t); err != nil {
			return nil, err
		}
		b := startFn(t)
		if b < boundary && !seen[b] {
			seen[b] = true
			out = append(out, b)
		}
	}
	return out, rows.Err()
}

func (s *Store) aggregateHoursUpTo(tx *sql.Tx, node int64, boundaryHour int64) error {
	var lastAgg sql.NullInt64
	if err := tx.QueryRow(`SELECT MAX(bucket) FROM solar_hourly WHERE node_id = ?`, node).Scan(&lastAgg); err != nil {
		return err
	}
	hours, err := s.distinctBuckets(tx, "solar_history", node, "reading_time",
		lastAgg.Int64, boundaryHour, s.hourStart)
	if err != nil {
		return err
	}
	for _, h := range hours {
		if err := s.aggregateOneHour(tx, node, h); err != nil {
			return err
		}
	}
	return nil
}

func (s *Store) aggregateOneHour(tx *sql.Tx, node int64, h int64) error {
	// This hour's cumulative = the last raw reading inside it.
	var curEnergy, curPump int64
	err := tx.QueryRow(
		`SELECT energy_gain, pump_runtime FROM solar_history
		 WHERE node_id = ? AND reading_time >= ? AND reading_time < ?
		 ORDER BY reading_time DESC LIMIT 1`, node, h, s.nextHour(h)).Scan(&curEnergy, &curPump)
	if err == sql.ErrNoRows {
		return nil // no raw in this hour (gap) - no row
	}
	if err != nil {
		return err
	}
	curKwh := float64(curEnergy) / solarKwhScale

	// Previous hour's stored cumulative (continuous chain, any day). Clamp >= 0 so the
	// daily reset (cumulative drops) yields 0 for that hour, gen1-style.
	var prevYield sql.NullFloat64
	var prevPump sql.NullInt64
	_ = tx.QueryRow(
		`SELECT day_yield, day_pump FROM solar_hourly
		 WHERE node_id = ? AND bucket < ? ORDER BY bucket DESC LIMIT 1`, node, h).Scan(&prevYield, &prevPump)

	hourYield := curKwh - prevYield.Float64
	if hourYield < 0 {
		hourYield = 0
	}
	hourPump := curPump - prevPump.Int64
	if hourPump < 0 {
		hourPump = 0
	}
	_, err = tx.Exec(
		`INSERT INTO solar_hourly (node_id, bucket, hour_yield, hour_pump, day_yield, day_pump)
		 VALUES (?,?,?,?,?,?)
		 ON CONFLICT(node_id, bucket) DO UPDATE SET
		   hour_yield = excluded.hour_yield, hour_pump = excluded.hour_pump,
		   day_yield = excluded.day_yield, day_pump = excluded.day_pump`,
		node, h, hourYield, hourPump, curKwh, curPump)
	return err
}

func (s *Store) aggregateDaysUpTo(tx *sql.Tx, node int64, boundaryDay int64) error {
	var lastAgg sql.NullInt64
	if err := tx.QueryRow(`SELECT MAX(bucket) FROM solar_daily WHERE node_id = ?`, node).Scan(&lastAgg); err != nil {
		return err
	}
	days, err := s.distinctBuckets(tx, "solar_hourly", node, "bucket",
		lastAgg.Int64, boundaryDay, s.dayStart)
	if err != nil {
		return err
	}
	for _, d := range days {
		if err := s.aggregateOneDay(tx, node, d); err != nil {
			return err
		}
	}
	return nil
}

func (s *Store) aggregateOneDay(tx *sql.Tx, node int64, d int64) error {
	// Day total = SUM of per-hour increments (NOT the last hour's cumulative). gen1's
	// reset can land inside our local day - zeroing the cumulative near the end - so the
	// last hour is unreliable; the reset hour just contributes hour_yield 0 to the sum.
	var dayYield sql.NullFloat64
	var dayPump sql.NullInt64
	err := tx.QueryRow(
		`SELECT SUM(hour_yield), SUM(hour_pump) FROM solar_hourly
		 WHERE node_id = ? AND bucket >= ? AND bucket < ?`, node, d, s.nextDay(d)).Scan(&dayYield, &dayPump)
	if err != nil {
		return err
	}
	if !dayYield.Valid { // no hourly rows for this day
		return nil
	}
	dYield, dPump := dayYield.Float64, dayPump.Int64
	// Cumulative since 1st of month: previous same-month day + this day.
	prevYield, prevPump := 0.0, int64(0)
	var pb sql.NullInt64
	var py sql.NullFloat64
	var pp sql.NullInt64
	if err := tx.QueryRow(
		`SELECT bucket, month_yield, month_pump FROM solar_daily
		 WHERE node_id = ? AND bucket < ? ORDER BY bucket DESC LIMIT 1`, node, d).
		Scan(&pb, &py, &pp); err == nil && pb.Valid && sameLocalMonth(pb.Int64, d, s.loc) {
		prevYield, prevPump = py.Float64, pp.Int64
	}
	_, err = tx.Exec(
		`INSERT INTO solar_daily (node_id, bucket, day_yield, month_yield, month_pump)
		 VALUES (?,?,?,?,?)
		 ON CONFLICT(node_id, bucket) DO UPDATE SET
		   day_yield = excluded.day_yield, month_yield = excluded.month_yield, month_pump = excluded.month_pump`,
		node, d, dYield, prevYield+dYield, prevPump+dPump)
	return err
}

func (s *Store) aggregateMonthsUpTo(tx *sql.Tx, node int64, boundaryMonth int64) error {
	var lastAgg sql.NullInt64
	if err := tx.QueryRow(`SELECT MAX(bucket) FROM solar_monthly WHERE node_id = ?`, node).Scan(&lastAgg); err != nil {
		return err
	}
	months, err := s.distinctBuckets(tx, "solar_daily", node, "bucket",
		lastAgg.Int64, boundaryMonth, s.monthStart)
	if err != nil {
		return err
	}
	for _, m := range months {
		if err := s.aggregateOneMonth(tx, node, m); err != nil {
			return err
		}
	}
	return nil
}

func (s *Store) aggregateOneMonth(tx *sql.Tx, node int64, m int64) error {
	var monthYield float64
	var monthPump int64
	err := tx.QueryRow(
		`SELECT month_yield, month_pump FROM solar_daily
		 WHERE node_id = ? AND bucket >= ? AND bucket < ?
		 ORDER BY bucket DESC LIMIT 1`, node, m, s.nextMonth(m)).Scan(&monthYield, &monthPump)
	if err == sql.ErrNoRows {
		return nil
	}
	if err != nil {
		return err
	}
	prevYield, prevPump := 0.0, int64(0)
	var pb sql.NullInt64
	var py sql.NullFloat64
	var pp sql.NullInt64
	if err := tx.QueryRow(
		`SELECT bucket, year_yield, year_pump FROM solar_monthly
		 WHERE node_id = ? AND bucket < ? ORDER BY bucket DESC LIMIT 1`, node, m).
		Scan(&pb, &py, &pp); err == nil && pb.Valid && sameLocalYear(pb.Int64, m, s.loc) {
		prevYield, prevPump = py.Float64, pp.Int64
	}
	_, err = tx.Exec(
		`INSERT INTO solar_monthly (node_id, bucket, month_yield, year_yield, year_pump)
		 VALUES (?,?,?,?,?)
		 ON CONFLICT(node_id, bucket) DO UPDATE SET
		   month_yield = excluded.month_yield, year_yield = excluded.year_yield, year_pump = excluded.year_pump`,
		node, m, monthYield, prevYield+monthYield, prevPump+monthPump)
	return err
}

// pruneSolarRaw drops raw older than the 2h buffer (keeps enough for the next power
// calc, which needs the previous reading).
func (s *Store) pruneSolarRaw(tx *sql.Tx, node int64, ts int64) error {
	_, err := tx.Exec(`DELETE FROM solar_history WHERE node_id = ? AND reading_time < ?`,
		node, ts-solarRawBufferSec)
	return err
}

// dropSolarNode removes a node's raw + all aggregates (used when a node is removed).
func (s *Store) dropSolarNode(node int64) error {
	for _, t := range []string{"solar_history", "solar_hourly", "solar_daily", "solar_monthly"} {
		if _, err := s.db.Exec("DELETE FROM "+t+" WHERE node_id = ?", node); err != nil {
			return err
		}
	}
	return nil
}

// AggregateAllSolarOnStartup rolls up any hours that completed while the service was
// down (the 2h buffer may hold a finished hour). Cheap; not a full rebuild.
func (s *Store) AggregateAllSolarOnStartup() {
	rows, err := s.db.Query(`SELECT DISTINCT node_id FROM solar_history`)
	if err != nil {
		return
	}
	var nodes []int64
	for rows.Next() {
		var n int64
		if rows.Scan(&n) == nil {
			nodes = append(nodes, n)
		}
	}
	rows.Close()
	now := time.Now().Unix()
	for _, n := range nodes {
		if err := s.aggregateSolar(n, now); err != nil {
			log.Printf("[Store] solar node %d: startup aggregation failed: %v", n, err)
		}
	}
}

// ================= read side =================

// solarLiveDay returns today's live cumulative (kWh, pump minutes) from the newest raw
// row, plus which local day it belongs to. ok=false when there is no raw at all.
func (s *Store) solarLiveDay(node int64) (day int64, kwh float64, pump int64, ok bool) {
	var ts, energy, p int64
	err := s.db.QueryRow(
		`SELECT reading_time, energy_gain, pump_runtime FROM solar_history
		 WHERE node_id = ? ORDER BY reading_time DESC LIMIT 1`, node).Scan(&ts, &energy, &p)
	if err != nil {
		return 0, 0, 0, false
	}
	return s.dayStart(ts), float64(energy) / solarKwhScale, p, true
}

func (s *Store) solarLabel(rng string, bucket, lastYear int64) string {
	t := s.local(bucket)
	switch rng {
	case "day":
		return fmt.Sprintf("%d %s %d", t.Day(), plMonthAbbr[t.Month()], t.Year())
	case "month":
		return fmt.Sprintf("%s %d", plMonthAbbr[t.Month()], t.Year())
	case "year":
		return fmt.Sprintf("%d", t.Year())
	case "total":
		return fmt.Sprintf("%d – %d", t.Year(), lastYear)
	}
	return ""
}

// SolarHistory builds chart series for "day"|"month"|"year"|"total". count = periods
// back (newest last); ignored for "total". count <= 0 means "every period that has
// data" (from the first reading to now), so the app's arrows can browse the whole
// history and stop exactly where the data does.
func (s *Store) SolarHistory(node int64, rng string, count int) ([]SolarSeries, error) {
	first, _, err := s.solarSpan(node)
	if err != nil {
		return nil, err
	}
	if first == 0 {
		return []SolarSeries{}, nil
	}
	liveDay, liveKwh, livePump, hasLive := s.solarLiveDay(node)
	now := time.Now().In(s.loc)

	if count <= 0 { // span the whole history for this range
		f := s.local(first)
		switch rng {
		case "day":
			count = int(now.Sub(time.Date(f.Year(), f.Month(), f.Day(), 0, 0, 0, 0, s.loc)).Hours()/24) + 2
		case "month":
			count = (now.Year()-f.Year())*12 + int(now.Month()) - int(f.Month()) + 1
		}
		// year/total loops below already cover the full span when count <= 0.
	}

	switch rng {
	case "day":
		out := []SolarSeries{}
		for i := count - 1; i >= 0; i-- {
			d := now.AddDate(0, 0, -i)
			dayB := time.Date(d.Year(), d.Month(), d.Day(), 0, 0, 0, 0, s.loc).Unix()
			if dayB < s.dayStart(first) {
				continue
			}
			out = append(out, s.daySeries(node, dayB, liveDay, liveKwh, livePump, hasLive))
		}
		return out, nil
	case "month":
		out := []SolarSeries{}
		for i := count - 1; i >= 0; i-- {
			m := now.AddDate(0, -i, 0)
			mB := time.Date(m.Year(), m.Month(), 1, 0, 0, 0, 0, s.loc).Unix()
			if s.nextMonth(mB) <= first {
				continue
			}
			out = append(out, s.monthSeries(node, mB, liveDay, liveKwh, livePump, hasLive))
		}
		return out, nil
	case "year":
		out := []SolarSeries{}
		firstYear := s.local(first).Year()
		for y := firstYear; y <= now.Year(); y++ {
			if count > 0 && now.Year()-y >= count {
				continue
			}
			out = append(out, s.yearSeries(node, y, liveDay, liveKwh, livePump, hasLive))
		}
		return out, nil
	case "total":
		return []SolarSeries{s.totalSeries(node, s.local(first).Year(), now.Year(), liveDay, liveKwh, livePump, hasLive)}, nil
	}
	return nil, fmt.Errorf("unknown range %q (day|month|year|total)", rng)
}

func (s *Store) solarSpan(node int64) (int64, int64, error) {
	// The oldest data may already be aggregated away from the raw buffer, so span
	// comes from the coarsest level present.
	var first, last sql.NullInt64
	err := s.db.QueryRow(`
		SELECT MIN(b), MAX(b) FROM (
		  SELECT MIN(bucket) b, MAX(bucket) FROM solar_monthly WHERE node_id = ?1
		  UNION ALL SELECT MIN(bucket), MAX(bucket) FROM solar_daily WHERE node_id = ?1
		  UNION ALL SELECT MIN(bucket), MAX(bucket) FROM solar_hourly WHERE node_id = ?1
		  UNION ALL SELECT MIN(reading_time), MAX(reading_time) FROM solar_history WHERE node_id = ?1
		)`, node).Scan(&first, &last)
	if err != nil {
		return 0, 0, err
	}
	return first.Int64, last.Int64, nil
}

// currentHourLiveYield: today's live cumulative minus the last aggregated hour of
// today = the in-progress hour's partial yield (the raw tail of the day chart).
func (s *Store) currentHourLiveYield(node int64, dayB int64, liveKwh float64) float64 {
	var lastHourCum sql.NullFloat64
	_ = s.db.QueryRow(
		`SELECT day_yield FROM solar_hourly
		 WHERE node_id = ? AND bucket >= ? AND bucket < ? ORDER BY bucket DESC LIMIT 1`,
		node, dayB, s.nextDay(dayB)).Scan(&lastHourCum)
	v := liveKwh - lastHourCum.Float64
	if v < 0 {
		return 0
	}
	return v
}

func (s *Store) daySeries(node int64, dayB, liveDay int64, liveKwh float64, livePump int64, hasLive bool) SolarSeries {
	isToday := hasLive && liveDay == dayB
	ser := SolarSeries{Bucket: dayB, Label: s.solarLabel("day", dayB, 0)}

	// hour_yield per hour from solar_hourly; the in-progress hour gets the live tail.
	hours := map[int64]float64{}
	rows, _ := s.db.Query(
		`SELECT bucket, hour_yield FROM solar_hourly WHERE node_id = ? AND bucket >= ? AND bucket < ?`,
		node, dayB, s.nextDay(dayB))
	if rows != nil {
		for rows.Next() {
			var b int64
			var y float64
			if rows.Scan(&b, &y) == nil {
				hours[b] = y
			}
		}
		rows.Close()
	}
	curHour := s.hourStart(time.Now().Unix())
	for h := solarDayFirstHour; h <= solarDayLastHour; h++ {
		t := s.local(dayB)
		bucket := time.Date(t.Year(), t.Month(), t.Day(), h, 0, 0, 0, s.loc).Unix()
		y := hours[bucket]
		if isToday && bucket == curHour {
			y += s.currentHourLiveYield(node, dayB, liveKwh)
		}
		ser.Bars = append(ser.Bars, SolarBar{Bucket: bucket, EnergyKwh: y})
	}

	if isToday {
		ser.EnergyKwh, ser.PumpMinutes = liveKwh, livePump
	} else {
		ser.EnergyKwh, ser.PumpMinutes = s.dayTotal(node, dayB)
	}
	return ser
}

// dayTotal = SUM of the day's per-hour increments (robust to a mid-day reset; see
// aggregateOneDay).
func (s *Store) dayTotal(node int64, dayB int64) (float64, int64) {
	var y sql.NullFloat64
	var p sql.NullInt64
	_ = s.db.QueryRow(
		`SELECT SUM(hour_yield), SUM(hour_pump) FROM solar_hourly
		 WHERE node_id = ? AND bucket >= ? AND bucket < ?`,
		node, dayB, s.nextDay(dayB)).Scan(&y, &p)
	return y.Float64, p.Int64
}

func (s *Store) monthSeries(node int64, monthB, liveDay int64, liveKwh float64, livePump int64, hasLive bool) SolarSeries {
	ser := SolarSeries{Bucket: monthB, Label: s.solarLabel("month", monthB, 0)}
	isThisMonth := hasLive && sameLocalMonth(liveDay, monthB, s.loc)

	days := map[int64]float64{}
	rows, _ := s.db.Query(
		`SELECT bucket, day_yield FROM solar_daily WHERE node_id = ? AND bucket >= ? AND bucket < ?`,
		node, monthB, s.nextMonth(monthB))
	if rows != nil {
		for rows.Next() {
			var b int64
			var y float64
			if rows.Scan(&b, &y) == nil {
				days[b] = y
			}
		}
		rows.Close()
	}
	end := s.nextMonth(monthB)
	for d := monthB; d < end; d = s.nextDay(d) {
		y := days[d]
		if isThisMonth && d == liveDay {
			y = liveKwh // today's live total
		}
		ser.Bars = append(ser.Bars, SolarBar{Bucket: d, EnergyKwh: y})
	}

	// complete days this month (from solar_daily, excluding today) + today's live.
	compYield, compPump := s.monthThroughCompleteDays(node, monthB, liveDay, isThisMonth)
	if isThisMonth {
		ser.EnergyKwh, ser.PumpMinutes = compYield+liveKwh, compPump+livePump
	} else {
		ser.EnergyKwh, ser.PumpMinutes = s.monthTotal(node, monthB)
	}
	return ser
}

// monthThroughCompleteDays = month_yield/month_pump of the last solar_daily row in the
// month strictly before `beforeDay` (i.e. complete days only). When not this month,
// beforeDay is ignored by the caller.
func (s *Store) monthThroughCompleteDays(node int64, monthB, beforeDay int64, thisMonth bool) (float64, int64) {
	upper := s.nextMonth(monthB)
	if thisMonth {
		upper = beforeDay // exclude today
	}
	var y sql.NullFloat64
	var p sql.NullInt64
	_ = s.db.QueryRow(
		`SELECT month_yield, month_pump FROM solar_daily
		 WHERE node_id = ? AND bucket >= ? AND bucket < ? ORDER BY bucket DESC LIMIT 1`,
		node, monthB, upper).Scan(&y, &p)
	return y.Float64, p.Int64
}

func (s *Store) monthTotal(node int64, monthB int64) (float64, int64) {
	var y sql.NullFloat64
	var p sql.NullInt64
	_ = s.db.QueryRow(
		`SELECT month_yield, month_pump FROM solar_daily
		 WHERE node_id = ? AND bucket >= ? AND bucket < ? ORDER BY bucket DESC LIMIT 1`,
		node, monthB, s.nextMonth(monthB)).Scan(&y, &p)
	return y.Float64, p.Int64
}

// yearSeries: bars = months of the year (month totals). The current month bar and the
// year total are composed live (complete days this month + today's raw tail).
func (s *Store) yearSeries(node int64, year int, liveDay int64, liveKwh float64, livePump int64, hasLive bool) SolarSeries {
	yearB := time.Date(year, 1, 1, 0, 0, 0, 0, s.loc).Unix()
	ser := SolarSeries{Bucket: yearB, Label: s.solarLabel("year", yearB, 0)}
	isThisYear := hasLive && s.local(liveDay).Year() == year

	months := map[int64]float64{}
	rows, _ := s.db.Query(
		`SELECT bucket, month_yield FROM solar_monthly WHERE node_id = ? AND bucket >= ? AND bucket < ?`,
		node, yearB, s.local(yearB).AddDate(1, 0, 0).Unix())
	if rows != nil {
		for rows.Next() {
			var b int64
			var y float64
			if rows.Scan(&b, &y) == nil {
				months[b] = y
			}
		}
		rows.Close()
	}
	curMonthB := s.monthStart(liveDay)
	// current-month live = complete days this month + today's live total
	curMonthComplete, curMonthCompleteP := s.monthThroughCompleteDays(node, curMonthB, liveDay, true)
	curMonthLive := curMonthComplete + liveKwh
	curMonthLiveP := curMonthCompleteP + livePump

	for m := 1; m <= 12; m++ {
		mB := time.Date(year, time.Month(m), 1, 0, 0, 0, 0, s.loc).Unix()
		y := months[mB]
		if isThisYear && mB == curMonthB {
			y = curMonthLive
		}
		ser.Bars = append(ser.Bars, SolarBar{Bucket: mB, EnergyKwh: y})
	}

	if isThisYear {
		cY, cP := s.yearThroughCompleteMonths(node, yearB, curMonthB)
		ser.EnergyKwh, ser.PumpMinutes = cY+curMonthLive, cP+curMonthLiveP
	} else {
		ser.EnergyKwh, ser.PumpMinutes = s.yearTotal(node, yearB)
	}
	return ser
}

// yearThroughCompleteMonths = year_yield/year_pump of the last solar_monthly row this
// year strictly before beforeMonth (complete months only).
func (s *Store) yearThroughCompleteMonths(node int64, yearB, beforeMonth int64) (float64, int64) {
	var y sql.NullFloat64
	var p sql.NullInt64
	_ = s.db.QueryRow(
		`SELECT year_yield, year_pump FROM solar_monthly
		 WHERE node_id = ? AND bucket >= ? AND bucket < ? ORDER BY bucket DESC LIMIT 1`,
		node, yearB, beforeMonth).Scan(&y, &p)
	return y.Float64, p.Int64
}

func (s *Store) yearTotal(node int64, yearB int64) (float64, int64) {
	var y sql.NullFloat64
	var p sql.NullInt64
	_ = s.db.QueryRow(
		`SELECT year_yield, year_pump FROM solar_monthly
		 WHERE node_id = ? AND bucket >= ? AND bucket < ? ORDER BY bucket DESC LIMIT 1`,
		node, yearB, s.local(yearB).AddDate(1, 0, 0).Unix()).Scan(&y, &p)
	return y.Float64, p.Int64
}

// totalSeries: one bar per year (year totals), current year composed live.
func (s *Store) totalSeries(node int64, firstYear, curYear int, liveDay int64, liveKwh float64, livePump int64, hasLive bool) SolarSeries {
	firstB := time.Date(firstYear, 1, 1, 0, 0, 0, 0, s.loc).Unix()
	ser := SolarSeries{Bucket: firstB, Label: s.solarLabel("total", firstB, int64(curYear))}
	for y := firstYear; y <= curYear; y++ {
		ys := s.yearSeries(node, y, liveDay, liveKwh, livePump, hasLive)
		ser.Bars = append(ser.Bars, SolarBar{Bucket: ys.Bucket, EnergyKwh: ys.EnergyKwh, PumpMinutes: ys.PumpMinutes})
		ser.EnergyKwh += ys.EnergyKwh
		ser.PumpMinutes += ys.PumpMinutes
	}
	return ser
}
