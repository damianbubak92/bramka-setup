package main

import (
	"encoding/hex"
	"flag"
	"fmt"
	"log"
	"os"
	"os/exec"
	"os/signal"
	"strconv"
	"syscall"
	"time"
	_ "time/tzdata" // embed the IANA tz database: the engine wall-clock is correct
	// even on a minimal Arago image with no system tzdata (LoadLocation works).
)

func main() {
	testMode := flag.String("test", "hello",
		"Test mode: hello|data|spam|retry|replay|retry-drop|event|hang|crash-m4f|heartbeat|silent-hang|heartbeat-busy|push-rules|fire-smoke|serve")
	heartbeatMs := flag.Int("heartbeat-idle-ms", 5000,
		"Idle time before sending heartbeat PING (ms)")
	httpAddr := flag.String("http-addr", ":9443", "phone HTTPS API listen address (serve mode)")
	tlsCert := flag.String("tls-cert", "/etc/bramka/tls/cert.pem", "TLS cert (must match the app's pinned cert)")
	tlsKey := flag.String("tls-key", "/etc/bramka/tls/key.pem", "TLS private key for tls-cert")
	authToken := flag.String("auth-token", defaultAuthToken, "phone API shared token")
	dbPath := flag.String("db", "/var/lib/bramka/bramka.db", "SQLite database path (serve mode)")
	tz := flag.String("tz", "Europe/Warsaw", "IANA timezone for the engine wall-clock (time-sync)")
	dbMonitor := flag.Bool("db-monitor", true,
		"DEV ONLY: serve the DB monitor at /db (whole database + SQL console). Pass -db-monitor=false in production")
	gen1URL := flag.String("gen1-url", "", "gen1 solar export endpoint (import-gen1 mode), e.g. https://host/solar-export.php")
	gen1Key := flag.String("gen1-key", "", "shared key for the gen1 export endpoint (import-gen1 mode)")
	gen1Insecure := flag.Bool("gen1-insecure", false, "skip TLS cert validation for the gen1 endpoint (its cert name mismatches)")
	gen1MaxPages := flag.Int("gen1-max-pages", 0, "limit import-gen1 to N pages (0 = all); use a small value to verify before a full pull")
	backupURL := flag.String("backup-url", "", "external mirror push endpoint (gw-backup.php); enables live backup when set")
	restoreURL := flag.String("restore-url", "", "external mirror pull endpoint (gw-restore.php); used by -test restore")
	backupKey := flag.String("backup-key", "", "shared key for the backup/restore endpoints")
	backupInsecure := flag.Bool("backup-insecure", false, "skip TLS cert validation for the backup/restore endpoints")
	flag.Parse()

	// import-gen1: a standalone admin operation (DB + HTTP only, no M4F). Handle it
	// before touching the transport so it runs without the M4F link up.
	if *testMode == "import-gen1" {
		runImportGen1(*dbPath, *tz, *gen1URL, *gen1Key, *gen1Insecure, *gen1MaxPages)
		return
	}
	// restore: rebuild a fresh gateway from the external mirror. Standalone (no M4F).
	if *testMode == "restore" {
		runRestore(*dbPath, *tz, *restoreURL, *backupKey, *backupInsecure)
		return
	}
	backupCfg := BackupConfig{PushURL: *backupURL, Key: *backupKey,
		Insecure: *backupInsecure, Interval: 15 * time.Second}

	httpCfg := HTTPConfig{Addr: *httpAddr, CertFile: *tlsCert, KeyFile: *tlsKey,
		AuthToken: *authToken, DBMonitor: *dbMonitor,
		RestoreURL: *restoreURL, RestoreKey: *backupKey, RestoreInsecure: *backupInsecure}

	log.SetFlags(log.LstdFlags | log.Lmicroseconds)
	log.Printf("=== rpmsg-service starting (test mode: %s, heartbeat idle: %dms) ===",
		*testMode, *heartbeatMs)

	// Setup transport
	t, err := OpenTransport()
	if err != nil {
		log.Fatalf("OpenTransport: %v", err)
	}
	defer t.Close()

	// Setup protocol layer
	p := NewProtocol(t, time.Duration(*heartbeatMs)*time.Millisecond)
	defer p.Close()

	// Signal handler
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)

	// systemd notify: service ready
	sdNotify("READY=1")

	// systemd watchdog kicker - sends WATCHDOG=1 every N seconds
	go watchdogKicker()

	// Run test
	go func() {
		switch *testMode {
		case "hello":
			runHelloTest(p)
		case "data":
			runDataTest(p)
		case "spam":
			runSpamTest(p)
		case "retry":
			runRetryTest(p)
		case "replay":
			runReplayTest(p)
		case "retry-drop":
			runRetryDropTest(p)
		case "event":
			runEventTest(p)
		case "hang":
			runHangTest(p)
		case "crash-m4f":
			runCrashM4FTest(p)
		case "heartbeat":
			runHeartbeatTest(p)
		case "silent-hang":
			runSilentHangTest(p)
		case "heartbeat-busy":
			runHeartbeatBusyTest(p)
		case "push-rules":
			runPushRulesTest(p)
		case "fire-smoke":
			runFireSmokeTest(p)
		case "serve":
			runServe(p, httpCfg, *dbPath, *tz, backupCfg)
		default:
			log.Printf("Unknown test mode: %s", *testMode)
		}
	}()

	// Peer-dead watcher: triggered when heartbeat PING gives up.
	// Recovery is a CLEAN REBOOT, not remoteproc stop/start: on AM62 there is
	// no per-core M4F reset, and writing "stop" to a hung M4F (silent-hang,
	// interrupts disabled) wedges the whole SoC. A clean reboot syncs the SD
	// and brings everything back; the HW watchdog (Warstwa D) is the backup if
	// the reboot itself can't proceed.
	go func() {
		<-p.PeerDeadCh()
		log.Printf("[Main] *** PEER DEAD - M4F unreachable (heartbeat giveup or device gone) ***")
		log.Printf("[Main] Recovery: clean reboot (remoteproc stop is unsafe on a hung M4F)")
		sdNotify("STOPPING=1")
		recoverByReboot()
		select {} // reboot in progress - block until the system goes down
	}()

	// Wait for signal
	sig := <-sigChan
	log.Printf("Received signal: %v, shutting down...", sig)
	
	// Notify systemd we're stopping
	sdNotify("STOPPING=1")
}

// helloWithRetry sends HELLO and retries with exponential backoff (1,2,4,8,16s).
// Catches the startup race where Go comes up before M4F is ready to reply
// HELLO_ACK (e.g. after a SoC reset M4F boots faster than Linux, but the
// rpmsg endpoint may still be settling). Returns nil once connected, or the
// last error after all attempts are exhausted.
func helloWithRetry(p *Protocol) error {
	const maxAttempts = 5
	var err error
	for attempt := 1; attempt <= maxAttempts; attempt++ {
		if err = p.Hello(3 * time.Second); err == nil {
			if attempt > 1 {
				log.Printf("[Test] HELLO succeeded on attempt %d/%d", attempt, maxAttempts)
			}
			return nil
		}
		log.Printf("[Test] HELLO attempt %d/%d failed: %v", attempt, maxAttempts, err)
		if attempt < maxAttempts {
			backoff := time.Duration(1<<(attempt-1)) * time.Second
			log.Printf("[Test] Retrying HELLO in %v...", backoff)
			time.Sleep(backoff)
		}
	}
	return fmt.Errorf("HELLO failed after %d attempts: %w", maxAttempts, err)
}

func runHelloTest(p *Protocol) {
	time.Sleep(200 * time.Millisecond) // give dispatcher time to start

	log.Println("[Test] Sending HELLO (with retry)...")
	if err := helloWithRetry(p); err != nil {
		log.Printf("[Test] HELLO failed: %v", err)
		return
	}
	log.Println("[Test] HELLO success, connected!")
	log.Println("[Test] Idling - Ctrl+C to stop")
}

func runDataTest(p *Protocol) {
	time.Sleep(200 * time.Millisecond)

	log.Println("[Test] Sending HELLO...")
	if err := p.Hello(3 * time.Second); err != nil {
		log.Printf("[Test] HELLO failed: %v", err)
		return
	}
	log.Println("[Test] Connected, sending DATA...")

	payload := []byte("Hello from Go DATA #1")
	if err := p.SendData(payload, 1*time.Second); err != nil {
		log.Printf("[Test] DATA failed: %v", err)
		return
	}
	log.Println("[Test] DATA delivered (ACK received)!")
}

func runSpamTest(p *Protocol) {
	time.Sleep(200 * time.Millisecond)

	if err := p.Hello(3 * time.Second); err != nil {
		log.Printf("[Test] HELLO failed: %v", err)
		return
	}
	log.Println("[Test] Connected, spamming 5 DATAs...")

	for i := 1; i <= 5; i++ {
		payload := []byte("Spam message #" + string(rune('0'+i)))
		start := time.Now()
		if err := p.SendData(payload, 1*time.Second); err != nil {
			log.Printf("[Test] DATA #%d failed: %v", i, err)
		} else {
			log.Printf("[Test] DATA #%d delivered (RTT %v)", i, time.Since(start))
		}
		time.Sleep(200 * time.Millisecond)
	}
	log.Println("[Test] Spam test done")
}

func runRetryTest(p *Protocol) {
    time.Sleep(200 * time.Millisecond)

    log.Println("[Test] Sending HELLO...")
    if err := p.Hello(3 * time.Second); err != nil {
        log.Printf("[Test] HELLO failed: %v", err)
        return
    }
    log.Println("[Test] Connected.")
    log.Println("[Test] === RETRY TEST ===")
    log.Println("[Test] Manually stop M4F NOW from another terminal:")
    log.Println("[Test]   ssh root@bramka 'echo stop > /sys/class/remoteproc/remoteproc0/state'")
    log.Println("[Test] Then we'll send DATA - expect retries and giveup")
    log.Println("[Test] Sleeping 10s to give you time...")

    for i := 10; i > 0; i-- {
        log.Printf("[Test]   ... %d seconds remaining", i)
        time.Sleep(1 * time.Second)
    }

    log.Println("[Test] NOW sending DATA - should retry 3x then giveup")
    start := time.Now()
    err := p.SendData([]byte("This message should fail"), 1*time.Second)
    elapsed := time.Since(start)

    if err == nil {
        log.Printf("[Test] UNEXPECTED: DATA succeeded in %v (was M4F stopped?)", elapsed)
    } else {
        log.Printf("[Test] EXPECTED FAILURE in %v: %v", elapsed, err)
    }
}

func runReplayTest(p *Protocol) {
    time.Sleep(200 * time.Millisecond)

    log.Println("[Test] Sending HELLO...")
    if err := p.Hello(3 * time.Second); err != nil {
        log.Printf("[Test] HELLO failed: %v", err)
        return
    }
    log.Println("[Test] Connected.")
    log.Println("[Test] === IDEMPOTENCY TEST ===")

    // First DATA with seq=2 - should be processed normally
    log.Println("[Test] Sending DATA seq=2 (first time)...")
    err := p.SendDataWithSeq([]byte("First send seq=2"), 2, 1*time.Second)
    if err != nil {
        log.Printf("[Test] First send failed: %v", err)
        return
    }
    log.Println("[Test] First send: DELIVERED")

    time.Sleep(500 * time.Millisecond)

    // Same seq=2 again - M4F should detect duplicate, re-ACK without re-process
    log.Println("[Test] Sending DATA seq=2 AGAIN (should be duplicate)...")
    err = p.SendDataWithSeq([]byte("Second send seq=2 (DUPLICATE)"), 2, 1*time.Second)
    if err != nil {
        log.Printf("[Test] Second send failed: %v", err)
        return
    }
    log.Println("[Test] Second send: DELIVERED (was M4F duplicate-aware?)")
    log.Println("[Test] Check m4f-watch - should see 'DUPLICATE - re-ACK only' for second send")
}

func runRetryDropTest(p *Protocol) {
    time.Sleep(200 * time.Millisecond)

    log.Println("[Test] Sending HELLO...")
    if err := p.Hello(3 * time.Second); err != nil {
        log.Printf("[Test] HELLO failed: %v", err)
        return
    }
    log.Println("[Test] Connected.")
    log.Println("[Test] === RETRY-DROP TEST ===")
    log.Println("[Test] Will drop next 2 ACKs - expect 2 retries then success on 3rd")

    p.SetDebugDropAcks(2)

    log.Println("[Test] Sending DATA...")
    start := time.Now()
    err := p.SendData([]byte("retry test message"), 1*time.Second)
    elapsed := time.Since(start)

    if err != nil {
        log.Printf("[Test] FAILED in %v: %v", elapsed, err)
    } else {
        log.Printf("[Test] DELIVERED in %v (with retries - check log above)", elapsed)
    }
    
    log.Println()
    log.Println("[Test] === GIVEUP TEST ===")
    log.Println("[Test] Will drop next 10 ACKs - expect 3 retries then giveup")
    
    p.SetDebugDropAcks(10)
    
    log.Println("[Test] Sending DATA...")
    start = time.Now()
    err = p.SendData([]byte("this should giveup"), 1*time.Second)
    elapsed = time.Since(start)
    
    if err != nil {
        log.Printf("[Test] EXPECTED FAILURE in %v: %v", elapsed, err)
    } else {
        log.Printf("[Test] UNEXPECTED SUCCESS in %v", elapsed)
    }
}

func runEventTest(p *Protocol) {
    time.Sleep(200 * time.Millisecond)

    log.Println("[Test] Sending HELLO...")
    if err := p.Hello(3 * time.Second); err != nil {
        log.Printf("[Test] HELLO failed: %v", err)
        return
    }
    log.Println("[Test] Connected.")
    log.Println("[Test] === EVENT LISTENER ===")
    log.Println("[Test] Listening for EVENTs from M4F (Ctrl+C to stop)")
    log.Println("[Test] M4F sends test EVENT every 10 ticks (~10 seconds)")
    log.Println()

    evCount := 0
    for ev := range p.EventRx() {
        evCount++
        log.Printf("[Test] EVENT #%d type=0x%02X seq=%d payload=%q",
            evCount, ev.Type, ev.Seq, string(ev.Payload))
    }
}

// watchdogKicker sends WATCHDOG=1 to systemd periodically.
// systemd will restart us if we miss too many.
//
// Frequency: read WATCHDOG_USEC env var (set by systemd), use half.
// Without systemd, this is a no-op (sdNotify silently returns).
func watchdogKicker() {
	usecStr := os.Getenv("WATCHDOG_USEC")
	if usecStr == "" {
		// Not running under systemd watchdog
		return
	}

	// Parse "WATCHDOG_USEC=10000000" (microseconds)
	usec, err := strconv.ParseInt(usecStr, 10, 64)
	if err != nil {
		log.Printf("[systemd] bad WATCHDOG_USEC=%q: %v", usecStr, err)
		return
	}

	// Kick at half the timeout (best practice)
	interval := time.Duration(usec) * time.Microsecond / 2
	log.Printf("[systemd] watchdog kicker: every %v (systemd timeout %v)",
		interval, time.Duration(usec)*time.Microsecond)

	ticker := time.NewTicker(interval)
	defer ticker.Stop()

	for range ticker.C {
		sdNotify("WATCHDOG=1")
	}
}

func runHangTest(p *Protocol) {
	time.Sleep(200 * time.Millisecond)

	log.Println("[Test] Sending HELLO...")
	if err := p.Hello(3 * time.Second); err != nil {
		log.Printf("[Test] HELLO failed: %v", err)
		return
	}
	log.Println("[Test] Connected.")
	log.Println("[Test] === HANG TEST ===")
	log.Println("[Test] Disabling watchdog kicks - systemd should kill us in 10s")

	debugDisableWatchdog.Store(true)

	// Loop showing we're "alive" but not kicking
	for i := 1; i <= 30; i++ {
		time.Sleep(1 * time.Second)
		log.Printf("[Test] Still alive (no kick) %ds", i)
	}

	log.Println("[Test] If you see this, systemd did NOT kill us (bug!)")
}

func runCrashM4FTest(p *Protocol) {
    time.Sleep(200 * time.Millisecond)
    
    log.Println("[Test] Sending HELLO...")
    if err := p.Hello(3 * time.Second); err != nil {
        log.Fatalf("HELLO failed: %v", err)
    }
    log.Println("[Test] Connected.")
    log.Println("[Test] === M4F CRASH RECOVERY TEST ===")
    log.Println("[Test] Will force M4F hard fault in 2s")
    log.Println("[Test] Then observe Linux remoteproc auto-recovery")
    
    time.Sleep(2 * time.Second)
    
    log.Println("[Test] Sending DEBUG_CRASH trigger...")
    if err := p.SendDebugCrash(); err != nil {
        log.Printf("[Test] Failed to send crash trigger: %v", err)
        return
    }
    
    log.Println("[Test] Crash trigger sent. Expected:")
    log.Println("[Test]   M4F hardfault handler -> SOC_generateSwWarmResetMcuDomain")
    log.Println("[Test]   -> FULL SoC reset (whole gateway reboots; no per-core reset on AM62)")
    log.Println("[Test]   -> ssh drops now; bramka back in ~70s, systemd auto-starts service")
    log.Println("[Test] Watching 30s (connection will drop before this finishes)...")
    
    // Czekamy żeby zobaczyć co się dzieje
    for i := 1; i <= 30; i++ {
        time.Sleep(1 * time.Second)
        log.Printf("[Test]   ... %ds elapsed", i)
    }
    
    log.Println("[Test] Done observing")
}

// recoverByReboot performs a clean, SD-friendly full reboot. This is the only
// safe recovery for a dead/hung M4F on AM62: there is no per-core M4F reset,
// and remoteproc stop on a hung M4F wedges the whole SoC. Primary path is
// `systemctl reboot` (runs the systemd shutdown sequence: unmount + sync);
// if that can't be issued we sync and force a kernel reboot. Last resort if
// even that stalls is the HW watchdog (Warstwa D).
// rebootRequestPath is the trigger watched by bramka-reboot.path. Touching it
// (as the unprivileged service user) makes systemd run a clean reboot - the
// service itself holds no reboot privilege. See modules/08-hardening.sh.
const rebootRequestPath = "/run/bramka/reboot-request"

func recoverByReboot() {
	// Drop a breadcrumb so the boot-accounting service can attribute the upcoming
	// reboot to us (vs an uninitiated hard reset). Written before Sync so it's
	// flushed to disk along with everything else.
	writeRebootReason("go-peer-dead (M4F unreachable via heartbeat/device-gone)")

	syscall.Sync() // flush state (incl. breadcrumb) before going down

	// Preferred path (works as the non-root service user): request a clean reboot
	// via the bramka-reboot.path watcher. We only write a file we own; PID 1 does
	// the privileged reboot.
	if err := os.WriteFile(rebootRequestPath, []byte("go-peer-dead\n"), 0o644); err == nil {
		log.Printf("[Recovery] reboot requested via %s (bramka-reboot.path)", rebootRequestPath)
		return
	} else {
		log.Printf("[Recovery] could not write %s: %v - falling back to direct reboot", rebootRequestPath, err)
	}

	// Fallback for dev/manual runs as root (path unit absent / outside systemd).
	log.Printf("[Recovery] Issuing 'systemctl reboot'...")
	if err := exec.Command("systemctl", "reboot").Run(); err != nil {
		log.Printf("[Recovery] 'systemctl reboot' failed: %v - forcing kernel reboot", err)
		if err := syscall.Reboot(syscall.LINUX_REBOOT_CMD_RESTART); err != nil {
			log.Printf("[Recovery] kernel reboot failed: %v - relying on HW watchdog (Warstwa D)", err)
		}
	}
}

// writeRebootReason drops a breadcrumb at /var/lib/bramka/reboot_reason so the
// boot-accounting service (modules/07-boot-accounting.sh) can attribute the
// next boot to this controlled reboot instead of an uninitiated hard reset.
// Best-effort: failure to write just means the reboot shows up as UNEXPECTED.
func writeRebootReason(reason string) {
	const dir = "/var/lib/bramka"
	if err := os.MkdirAll(dir, 0o755); err != nil {
		log.Printf("[Recovery] reboot-reason: mkdir %s: %v", dir, err)
		return
	}
	if err := os.WriteFile(dir+"/reboot_reason", []byte(reason+"\n"), 0o644); err != nil {
		log.Printf("[Recovery] reboot-reason: write: %v", err)
	}
}

// runHeartbeatTest: sanity check - sit idle, watch PINGs flow.
func runHeartbeatTest(p *Protocol) {
	time.Sleep(200 * time.Millisecond)

	log.Println("[Test] Sending HELLO...")
	if err := p.Hello(3 * time.Second); err != nil {
		log.Printf("[Test] HELLO failed: %v", err)
		return
	}
	log.Println("[Test] Connected.")
	log.Println("[Test] === HEARTBEAT SANITY TEST ===")
	log.Println("[Test] Idling 30s - expect ~5 heartbeat PINGs from Go side")
	log.Println("[Test] (also EVENTs from M4F every 10s will reset idle timer)")
	log.Println("[Test] Look for: 'TX heartbeat PING' + 'ACK seq=N type=0x03'")

	for i := 30; i > 0; i-- {
		log.Printf("[Test]   ... %ds remaining", i)
		time.Sleep(1 * time.Second)
	}

	log.Println("[Test] Done - heartbeat test complete")
}

// runSilentHangTest: force M4F into infinite loop, verify heartbeat detects.
func runSilentHangTest(p *Protocol) {
	time.Sleep(200 * time.Millisecond)

	log.Println("[Test] Sending HELLO...")
	if err := p.Hello(3 * time.Second); err != nil {
		log.Fatalf("HELLO: %v", err)
	}
	log.Println("[Test] Connected.")
	log.Println("[Test] === SILENT HANG DETECTION TEST ===")
	log.Println("[Test] Triggering M4F silent hang in 2s...")
	time.Sleep(2 * time.Second)

	hangSentAt := time.Now()
	if err := p.SendDebugHang(); err != nil {
		log.Printf("[Test] Failed to send hang trigger: %v", err)
		return
	}
	log.Println("[Test] M4F now hung. Expected timeline:")
	log.Println("[Test]   T+5s:  idle threshold reached, Go sends PING")
	log.Println("[Test]   T+6s:  1st retry")
	log.Println("[Test]   T+7s:  2nd retry")
	log.Println("[Test]   T+8s:  3rd retry")
	log.Println("[Test]   T+9s:  GIVEUP → PEER DEAD signal")
	log.Println("[Test]   T+9s:  clean reboot (recoverByReboot) - WHOLE GATEWAY REBOOTS")
	log.Println("[Test]   T+~70s: system back up, service reconnects")

	// Wait for peer-dead OR timeout
	select {
	case <-p.PeerDeadCh():
		elapsed := time.Since(hangSentAt)
		log.Printf("[Test] PEER DEAD detected after %v from hang trigger", elapsed)
		log.Println("[Test] Main goroutine will now clean-reboot the whole gateway")
		// Don't return - let main's peer-dead watcher do the recovery action
		time.Sleep(5 * time.Second)
	case <-time.After(15 * time.Second):
		log.Println("[Test] ERROR: peer dead NOT detected within 15s - bug?")
	}
}

// runPushRulesTest: push the gen1 example ruleset to the M4F engine and verify
// the RULE_BEGIN/ITEM/COMMIT handshake + atomic swap (ACK on each, COMMIT swaps).
func runPushRulesTest(p *Protocol) {
	time.Sleep(200 * time.Millisecond)

	log.Println("[Test] Sending HELLO (with retry)...")
	if err := helloWithRetry(p); err != nil {
		log.Printf("[Test] HELLO failed: %v", err)
		return
	}
	log.Println("[Test] Connected.")
	log.Println("[Test] === RULE PUSH TEST ===")

	rules := exampleRules()
	log.Printf("[Test] Pushing %d example rules (RULE_BEGIN -> ITEM* -> COMMIT)...", len(rules))
	if err := p.PushRules(rules); err != nil {
		log.Printf("[Test] rule push FAILED: %v", err)
		return
	}
	log.Println("[Test] Rule push OK - M4F committed the ruleset (atomic swap).")
	log.Println("[Test] m4f-watch: each RULE_* ACKed; COMMIT -> 'Engine ... rules' swap.")
	log.Println("[Test] NOTE: TIME rules need a time-sync to fire; data rules need")
	log.Println("[Test]       node input over SPI (not wired yet). See -test fire-smoke.")
}

// runFireSmokeTest: end-to-end engine firing check. Sync a fixed wall-clock,
// push one always-in-window TIME rule with SEND_MESSAGE (no solar guard), then
// listen for MSG_RULE_FIRED from the M4F engine. Fires land on the *engine's*
// wall-clock minute boundary (synced to 12:00:5x here, NOT real Linux time): the
// sync wakes the engine to re-align, so with second=50 the first RULE_FIRED hits
// the :00 boundary ~10s later, then once per aligned minute (12:01:00, 12:02:00).
func runFireSmokeTest(p *Protocol) {
	time.Sleep(200 * time.Millisecond)

	log.Println("[Test] Sending HELLO (with retry)...")
	if err := helloWithRetry(p); err != nil {
		log.Printf("[Test] HELLO failed: %v", err)
		return
	}
	log.Println("[Test] Connected.")
	log.Println("[Test] === FIRE SMOKE TEST ===")

	// 1) Deterministic wall-clock so COND_TIME can evaluate (M4F has no RTC).
	//    second=50 -> first :00 boundary ~10s away (quick first fire).
	const hh, mm, ss = 12, 0, 50
	log.Printf("[Test] Time-sync -> %02d:%02d:%02d", hh, mm, ss)
	if err := p.SendTimeSync(hh, mm, ss); err != nil {
		log.Printf("[Test] time-sync FAILED: %v", err)
		return
	}

	// 2) One TIME rule whose window covers 12:00, action SEND_MESSAGE to
	//    smartphone (no solar pumpState/sBuforTemp guard -> fires on time alone).
	rules := []Rule{{
		Name:       "SmokeFire",
		Conditions: []Condition{{Type: CondTime, Time: TimeCond{HourStart: 10, MinStart: 0, HourEnd: 14, MinEnd: 0}}},
		Action:     Action{Type: ActionSendMessage, Message: "SMOKE"},
	}}
	if err := p.PushRules(rules); err != nil {
		log.Printf("[Test] rule push FAILED: %v", err)
		return
	}

	log.Println("[Test] Rule committed. Expect ONE RULE_FIRED at each engine :00 boundary:")
	log.Println("[Test] first ~10s out (12:01:00), then every 60s - no double-fire.")
	log.Println("[Test] (log timestamps are real Linux time; fires track the synced clock.)")
	log.Println("[Test] Listening for MSG_RULE_FIRED (0x42) - Ctrl+C to stop...")
	n := 0
	for ev := range p.EventRx() {
		if ev.Type == MsgRuleFired {
			n++
			var idx uint16
			if len(ev.Payload) >= 2 {
				idx = uint16(ev.Payload[0])<<8 | uint16(ev.Payload[1])
			}
			log.Printf("[Test] RULE_FIRED #%d ruleIndex=%d (%d bytes payload)", n, idx, len(ev.Payload))
		} else {
			log.Printf("[Test] EVENT type=0x%02X seq=%d (%d bytes)", ev.Type, ev.Seq, len(ev.Payload))
		}
	}
}

// runServe is the production phone-access mode: connect to the M4F, keep its
// runImportGen1 back-fills solar history from the gen1 MySQL via its PHP export
// endpoint. Standalone: opens the DB, imports, rebuilds the rollup, exits. Safe to
// re-run (resumes from the newest gen1 row already stored).
func runImportGen1(dbPath, tz, endpoint, key string, insecure bool, maxPages int) {
	if endpoint == "" {
		log.Fatalf("[gen1] -gen1-url is required (e.g. -gen1-url https://host/solar-export.php)")
	}
	loc, err := time.LoadLocation(tz)
	if err != nil {
		loc = time.Local
	}
	store, err := OpenStore(dbPath, loc)
	if err != nil {
		log.Fatalf("[gen1] open DB %s failed: %v", dbPath, err)
	}
	defer store.Close()

	n, err := store.ImportGen1(endpoint, key, insecure, maxPages)
	if err != nil {
		log.Fatalf("[gen1] import failed after %d row(s): %v", n, err)
	}
	log.Printf("[gen1] done: %d new row(s) imported", n)
}

// runRestore rebuilds a fresh gateway's DB from the external mirror. Standalone
// (no M4F): run once after a re-flash, before starting the service.
func runRestore(dbPath, tz, restoreURL, key string, insecure bool) {
	if restoreURL == "" {
		log.Fatalf("[Restore] -restore-url is required (e.g. -restore-url https://host/gw-restore.php)")
	}
	loc, err := time.LoadLocation(tz)
	if err != nil {
		loc = time.Local
	}
	store, err := OpenStore(dbPath, loc)
	if err != nil {
		log.Fatalf("[Restore] open DB %s failed: %v", dbPath, err)
	}
	defer store.Close()
	if err := store.RestoreFromMirror(restoreURL, key, insecure); err != nil {
		log.Fatalf("[Restore] failed: %v", err)
	}
	// Re-derive the current-hour rollup from whatever raw the restore did not carry.
	store.AggregateAllSolarOnStartup()
	log.Printf("[Restore] gateway rebuilt from mirror - start the service normally now")
}

// wall-clock synced (NTP/system time), drain M4F->Linux events, and run the
// phone-facing HTTPS API. Blocks in the HTTP server; peer-dead recovery and
// signal handling stay in main().
func runServe(p *Protocol, cfg HTTPConfig, dbPath, tz string, backupCfg BackupConfig) {
	time.Sleep(200 * time.Millisecond)

	// Engine wall-clock zone, loaded from embedded tzdata so it's correct even if
	// the Arago system has no tzdata / is on UTC. Falls back to system local.
	loc, err := time.LoadLocation(tz)
	if err != nil {
		log.Printf("[Serve] timezone %q load failed: %v - using system local", tz, err)
		loc = time.Local
	}
	log.Printf("[Serve] engine timezone: %s (now %s)", tz, time.Now().In(loc).Format("15:04:05"))

	// Persistent rules store (source of truth for getrules + the on-connect push).
	// loc drives the solar daily-accumulation reset boundary.
	store, err := OpenStore(dbPath, loc)
	if err != nil {
		// Fail loud: without the DB there is no HTTP/WS API, so exiting non-zero lets
		// systemd restart (and, on a real fault like a root-owned DB, hit StartLimit
		// -> failed) instead of masquerading as "active" with transport up but no server.
		log.Fatalf("[Serve] open DB %s failed: %v", dbPath, err)
	}
	defer store.Close()
	log.Printf("[Serve] rules DB: %s", dbPath)

	// Roll up any hour that completed while the service was down (the 2h raw buffer
	// may hold a finished hour). Cheap; not a full rebuild.
	store.AggregateAllSolarOnStartup()

	// Local trash retention: purge nodes soft-deleted > 60 days ago (the sole hard
	// delete of a node + its history). Run once now, then daily.
	const trashRetention = 60 * 24 * time.Hour
	purgeTrash := func() {
		if n, err := store.PurgeExpiredTrash(trashRetention); err != nil {
			log.Printf("[Trash] purge failed: %v", err)
		} else if n > 0 {
			log.Printf("[Trash] purged %d expired node(s)", n)
		}
	}
	purgeTrash()
	go func() {
		t := time.NewTicker(24 * time.Hour)
		defer t.Stop()
		for range t.C {
			purgeTrash()
		}
	}()

	// Live backup: capture every change via triggers and drain to the mirror with
	// retry. Seed the queue with the current DB once so the mirror starts complete
	// (triggers only catch future changes). Disabled => remove triggers so the queue
	// does not grow unbounded with no drainer.
	if backupCfg.PushURL != "" {
		if err := store.InstallBackupTriggers(); err != nil {
			log.Printf("[Backup] trigger install failed (backup off): %v", err)
		} else {
			if err := store.SeedBackupFromCurrentState(); err != nil {
				log.Printf("[Backup] seed failed: %v", err)
			}
			go backupWorker(store, backupCfg)
		}
	} else {
		store.RemoveBackupTriggers()
	}

	// Pending node JOINs awaiting user approval (provisioning, [[provisioning-model]]).
	joins := newJoinRegistry()

	// WebSocket hub: the in-app live channel (telemetry + provisioning events now;
	// commands with reqId in Phase B). FCM (background alerts) comes later.
	hub := newWSHub()
	go hub.run()

	// DB monitor journal: SQLite's own update hook -> WS. Catches every write on
	// every table (including ones added later), and never blocks a write.
	if cfg.DBMonitor {
		store.OnChange(hub.PublishDBEvent)
	}

	// Drain M4F->Linux events FIRST (before HELLO): a reconnect can find a backlog
	// of stale telemetry/rule-fired buffered while Linux was down; consuming it
	// here keeps eventRx from overflowing. Phase 3 routes these to a DB + the
	// phone state endpoint; for now just observe.
	go func() {
		for ev := range p.EventRx() {
			switch ev.Type {
			case MsgRuleFired:
				log.Printf("[Serve] RULE_FIRED (%d bytes)", len(ev.Payload))
			case MsgNodeTelemetry:
				// Provisioning frames ride the telemetry path; demux by cmd.
				cmd, cok := NodeMsgCmd(ev.Payload)
				if cok && cmd == CmdJoinRequest {
					fid, nt, caps, ok := DecodeJoinRequest(ev.Payload)
					if !ok {
						log.Printf("[Serve] JOIN_REQUEST undecodable (%d bytes)", len(ev.Payload))
						break
					}
					factoryHex := hex.EncodeToString(fid[:])
					// Silence a JOIN from an ALREADY-ACTIVE node: the user pressed the button
					// on a working device by accident. No pending, no event -> total silence in
					// the app. (detached/trash/new all still surface for the add/restore flow.)
					if st, ok := store.FactoryStatus(factoryHex); ok && st == "active" {
						log.Printf("[Serve] JOIN from active node (factory %s) - ignored", factoryHex)
						break
					}
					if joins.Add(fid, nt, caps, time.Now().Unix()) {
						log.Printf("[Serve] JOIN request: node type %d factory %s caps 0x%X (awaiting approval)", nt, factoryHex, caps)
					}
					// Publish on EVERY JOIN (not just first-seen) so a user who pressed
					// JOIN again while on the devices screen always re-pops the dialog.
					hub.PublishJoinPending(factoryHex, nt)
					break
				}
				if cok && cmd == CmdRemove {
					// Node confirmed it cleared its identity -> delete row, free address.
					if id, ok := NodeMsgId(ev.Payload); ok {
						if err := store.DeleteNode(id); err != nil {
							log.Printf("[Serve] remove-confirm 0x%02X delete failed: %v", id, err)
						} else {
							log.Printf("[Serve] node 0x%02X confirmed removal - address freed", id)
							hub.PublishNodeStatus(id, "removed") // live: drop from device list
						}
					}
					break
				}

				nodeID, nodeType, frameFactory, params, ok := DecodeTelemetry(ev.Payload)
				if !ok {
					log.Printf("[Serve] NODE_TELEMETRY undecodable (%d bytes)", len(ev.Payload))
					break
				}

				// Lifecycle gate: only registered nodes are recorded; status drives the rest.
				status, storedFactory, _, exists, err := store.NodeStatus(nodeID)
				if err != nil {
					log.Printf("[Serve] node status query failed (0x%02X): %v", nodeID, err)
					break
				}
				if !exists {
					log.Printf("[Serve] telemetry from unregistered node 0x%02X - ignoring (unprovisioned should be silent)", nodeID)
					break
				}

				// Reactive identity check (§12.2): a v2 frame (non-zero factory_id) from a
				// non-legacy node must match the binding. A mismatch = a stale/impersonating
				// chip on a reused address -> tell it to unregister and drop the reading.
				// Legacy sniff nodes and old 'D' frames (factory_id == 0) are exempt.
				if status != "legacy" && frameFactory != ([8]byte{}) {
					if bound, ok := factoryHexToBytes(storedFactory); ok && bound != frameFactory {
						if err := p.SendUnregister(frameFactory, nodeType, nodeID); err != nil {
							log.Printf("[Serve] node 0x%02X identity mismatch - SendUnregister failed: %v", nodeID, err)
						} else {
							log.Printf("[Serve] node 0x%02X identity mismatch (frame factory != binding) - sent UNREGISTERED, dropping reading", nodeID)
						}
						break
					}
				}

				if err := store.RecordTelemetry(nodeID, nodeType, params, time.Now().Unix()); err != nil {
					log.Printf("[Serve] telemetry store failed (node %d type %d): %v", nodeID, nodeType, err)
					break
				}
				if status == "pending_join" {
					if err := store.MarkActive(nodeID); err == nil {
						log.Printf("[Serve] node 0x%02X confirmed provisioning - now active", nodeID)
						hub.PublishNodeStatus(nodeID, "active") // live: device goes online
					}
				}
				// Live telemetry push to any open app (the "system feels alive" stream).
				pm := make(map[string]float64, len(params))
				for _, prm := range params {
					pm[prm.Key] = prm.Num
				}
				hub.PublishTelemetry(nodeID, nodeType, pm, time.Now().Unix())
				log.Printf("[Serve] telemetry node %d type %d: %d param(s) stored", nodeID, nodeType, len(params))
			case MsgNodeState:
				log.Printf("[Serve] NODE_STATE (%d bytes)", len(ev.Payload))
			default:
				log.Printf("[Serve] EVENT 0x%02X seq=%d (%d bytes)", ev.Type, ev.Seq, len(ev.Payload))
			}
		}
	}()

	log.Println("[Serve] Connecting to M4F (HELLO)...")
	if err := helloWithRetry(p); err != nil {
		log.Printf("[Serve] HELLO failed: %v", err)
		return
	}
	log.Println("[Serve] Connected.")

	// Engine wall-clock: the M4F has no RTC, so seed it from system time (NTP on
	// Linux) and re-sync periodically to correct drift. Production carrier adds an
	// RTC; until then this is the time source for COND_TIME + the :00 tick.
	syncClock(p, loc)
	go func() {
		t := time.NewTicker(10 * time.Minute)
		defer t.Stop()
		for range t.C {
			syncClock(p, loc)
		}
	}()

	// Define the engine ruleset on connect from the DB (source of truth): makes
	// M4F state deterministic and stops stale rules from a prior session firing.
	rules, err := store.RulesForPush()
	if err != nil {
		log.Printf("[Serve] load rules from DB failed: %v (pushing empty)", err)
		rules = []Rule{}
	}
	if err := p.PushRules(rules); err != nil {
		log.Printf("[Serve] rule push failed: %v", err)
	} else {
		log.Printf("[Serve] pushed %d rule(s) to engine from DB", len(rules))
	}

	log.Println("[Serve] Starting phone HTTPS API + WebSocket (/ws)...")
	if err := StartHTTPAPI(p, store, joins, hub, cfg); err != nil {
		log.Printf("[Serve] HTTP API stopped: %v", err)
	}
}

// syncClock pushes the current wall-clock (h:m:s) in loc to the M4F engine.
func syncClock(p *Protocol, loc *time.Location) {
	now := time.Now().In(loc)
	if err := p.SendTimeSync(uint8(now.Hour()), uint8(now.Minute()), uint8(now.Second())); err != nil {
		log.Printf("[Serve] time-sync failed: %v", err)
		return
	}
	log.Printf("[Serve] time-sync -> %02d:%02d:%02d (%s)", now.Hour(), now.Minute(), now.Second(), loc)
}

// runHeartbeatBusyTest: send DATA every 2s, verify NO PINGs fire.
func runHeartbeatBusyTest(p *Protocol) {
	time.Sleep(200 * time.Millisecond)

	if err := p.Hello(3 * time.Second); err != nil {
		log.Printf("[Test] HELLO failed: %v", err)
		return
	}
	log.Println("[Test] Connected.")
	log.Println("[Test] === HEARTBEAT BUSY TEST ===")
	log.Println("[Test] Sending DATA every 2s for 30s")
	log.Println("[Test] Expected: ZERO 'TX heartbeat PING' lines in log")
	log.Println("[Test] (because traffic resets idle timer every 2s)")

	for i := 1; i <= 15; i++ {
		payload := []byte(fmt.Sprintf("busy traffic #%d", i))
		start := time.Now()
		if err := p.SendData(payload, 1*time.Second); err != nil {
			log.Printf("[Test] DATA #%d failed: %v", i, err)
		} else {
			log.Printf("[Test] DATA #%d delivered (RTT %v)", i, time.Since(start))
		}
		time.Sleep(2 * time.Second)
	}
	log.Println("[Test] Done - count PINGs above (should be 0)")
}