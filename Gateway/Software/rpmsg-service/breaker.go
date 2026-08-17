// Reboot circuit-breaker.
//
// A dead M4F is recovered by a clean full reboot (no per-core reset on AM62). That
// is fine for a transient hang, but a PERSISTENT cause (unplugged/flaky CC1310, a
// hardware fault, a recurring wedge) would reboot the gateway forever: each
// go-peer-dead -> reboot -> M4F still dead -> reboot ... systemd's StartLimitBurst
// does NOT stop this (it guards in-place restart loops, not reboot loops).
//
// The breaker gives a few FREE recovery reboots (a single reboot fixes most
// transient M4F issues), then THROTTLES with an exponential backoff: it keeps the
// gateway UP in a DEGRADED mode (phone API + DB still served, M4F/RF down) and
// retries after 10 -> 30 -> 60 min, so a transient cause self-heals over time
// while a hard fault stops hammering and a human gets a loud log.
//
// KEY INVARIANT: a reboot is ONLY ever triggered by a real go-peer-dead event.
// The breaker GATES reboots (allow now / defer), it never SCHEDULES a reboot on a
// healthy gateway. The only timer involved is the backoff sleep inside degraded
// mode, armed strictly after a confirmed death. A healthy M4F is never rebooted.
package main

import (
	"fmt"
	"log"
	"os"
	"strconv"
	"strings"
	"time"
)

const (
	bkFreeReboots      = 3 // immediate recovery reboots before throttling
	breakerDeathsPath  = "/var/lib/bramka/breaker_deaths"
	degradedMarkerPath = "/run/bramka/degraded"
)

// Timings are vars (not consts) so a test run can shrink them via env without a
// rebuild; defaults are the production values. bkStabilityT = how long the M4F must
// stay CONNECTED to clear the storm counter. bkBackoffs = degraded wait per throttle
// stage (last value is the cap -> retries roughly hourly in a sustained storm).
//   BREAKER_STABILITY_SEC=60  BREAKER_BACKOFF_SEC=20,40,60
var (
	bkStabilityT = 15 * time.Minute
	bkBackoffs   = []time.Duration{10 * time.Minute, 30 * time.Minute, 60 * time.Minute}
)

func init() {
	if s := os.Getenv("BREAKER_STABILITY_SEC"); s != "" {
		if n, err := strconv.Atoi(strings.TrimSpace(s)); err == nil && n > 0 {
			bkStabilityT = time.Duration(n) * time.Second
		}
	}
	if s := os.Getenv("BREAKER_BACKOFF_SEC"); s != "" {
		var out []time.Duration
		for _, part := range strings.Split(s, ",") {
			if n, err := strconv.Atoi(strings.TrimSpace(part)); err == nil && n > 0 {
				out = append(out, time.Duration(n)*time.Second)
			}
		}
		if len(out) > 0 {
			bkBackoffs = out
		}
	}
}

type breakerActionType int

const (
	actRebootNow breakerActionType = iota
	actDegrade
)

type breakerDecision struct {
	action breakerActionType
	wait   time.Duration // actDegrade: stay degraded this long before the retry reboot
	reason string        // for the loud log
}

// decideRecovery runs on a real PEER DEAD. connectedSince is when the M4F last
// connected (zero if it never connected this boot). It bumps the persistent storm
// counter and returns whether to reboot now or throttle.
func decideRecovery(now, connectedSince time.Time) breakerDecision {
	healthy := "never connected this boot"
	var healthyFor time.Duration
	if !connectedSince.IsZero() {
		healthyFor = now.Sub(connectedSince)
		healthy = healthyFor.Round(time.Second).String()
	}

	deaths := readBreakerDeaths()
	// M4F was stable a good while, then died -> fresh incident, forgive the storm
	// history. (The stability watcher normally already zeroed this; belt-and-braces
	// in case it did not run, e.g. a very short-lived connect.)
	if healthyFor >= bkStabilityT {
		deaths = 0
	}
	deaths++
	writeBreakerDeaths(deaths) // flushed to disk by recoverByReboot's Sync before reboot

	if deaths <= bkFreeReboots {
		return breakerDecision{
			action: actRebootNow,
			reason: fmt.Sprintf("recovery reboot %d/%d (M4F healthy %s before dying)", deaths, bkFreeReboots, healthy),
		}
	}

	stage := deaths - bkFreeReboots - 1 // 0,1,2,...
	if stage >= len(bkBackoffs) {
		stage = len(bkBackoffs) - 1
	}
	backoff := bkBackoffs[stage]
	return breakerDecision{
		action: actDegrade,
		wait:   backoff,
		reason: fmt.Sprintf("STORM: %d consecutive M4F deaths - throttling (retry in %s)", deaths, backoff),
	}
}

// handlePeerDead is the recovery entry point, called once the peer-dead watcher
// fires. It consults the breaker and either reboots now or degrades + retries.
func handlePeerDead(p *Protocol) {
	d := decideRecovery(time.Now(), p.ConnectedAt())
	if d.action == actRebootNow {
		log.Printf("[Breaker] %s -> clean reboot", d.reason)
		rebootAndBlock()
		return
	}

	// Throttled. Keep the gateway UP and serving (phone API / DB / app run on their
	// own goroutines; the systemd watchdog is kicked independently of the M4F), wait
	// out the backoff, then retry. A dead M4F on AM62 recovers only via a reboot and
	// cannot revive in-process, so after the backoff the retry is unconditional -
	// no healthy M4F can be hit here.
	log.Printf("[Breaker] *** DEGRADED MODE *** %s", d.reason)
	log.Printf("[Breaker] Gateway stays UP (phone API + DB still served); M4F + RF are DOWN until recovery.")
	log.Printf("[Breaker] A reboot cannot fix a hardware fault (e.g. an unplugged CC1310) - CHECK THE M4F/CC1310 LINK.")
	raiseDegradedAlarm(d.reason)

	time.Sleep(d.wait)

	log.Printf("[Breaker] backoff (%s) elapsed - retrying recovery via clean reboot", d.wait)
	rebootAndBlock()
}

func rebootAndBlock() {
	sdNotify("STOPPING=1")
	recoverByReboot()
	select {} // reboot in progress - block until the system goes down
}

// watchM4FStability clears the storm counter once the M4F has stayed CONNECTED for
// bkStabilityT with no peer-dead. This is the recovery reset: after a successful
// (retry) reboot - or a manual reboot - a stable M4F wipes the storm history so the
// next incident starts fresh with free recovery reboots. It NEVER triggers a reboot;
// it only zeroes the counter. Started in serve mode right after the M4F connects.
func watchM4FStability(p *Protocol) {
	select {
	case <-p.PeerDeadCh():
		return // died before proving stable - the breaker handles it
	case <-time.After(bkStabilityT):
		select {
		case <-p.PeerDeadCh(): // died right at the boundary
			return
		default:
			if readBreakerDeaths() != 0 {
				breakerReset()
				log.Printf("[Breaker] M4F stable for %s - storm counter cleared", bkStabilityT)
			}
			// Clear any stale degraded marker now that we are healthy.
			_ = os.Remove(degradedMarkerPath)
		}
	}
}

// raiseDegradedAlarm surfaces the degraded state. For now: the loud logs above plus
// a best-effort marker file (a future push-notification hookup can read/replace it).
func raiseDegradedAlarm(reason string) {
	msg := fmt.Sprintf("%d DEGRADED %s\n", time.Now().Unix(), reason)
	if err := os.WriteFile(degradedMarkerPath, []byte(msg), 0o644); err != nil {
		log.Printf("[Breaker] could not write degraded marker: %v", err)
	}
}

func readBreakerDeaths() int {
	b, err := os.ReadFile(breakerDeathsPath)
	if err != nil {
		return 0
	}
	n, err := strconv.Atoi(strings.TrimSpace(string(b)))
	if err != nil || n < 0 {
		return 0
	}
	return n
}

func writeBreakerDeaths(n int) {
	if err := os.WriteFile(breakerDeathsPath, []byte(strconv.Itoa(n)+"\n"), 0o644); err != nil {
		log.Printf("[Breaker] could not persist storm counter: %v", err)
	}
}

func breakerReset() { writeBreakerDeaths(0) }
