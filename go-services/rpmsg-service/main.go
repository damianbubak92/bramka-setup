package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	"os/exec"
	"os/signal"
	"strconv"
	"syscall"
	"time"
)

func main() {
	testMode := flag.String("test", "hello",
		"Test mode: hello|data|spam|retry|replay|retry-drop|event|hang|crash-m4f|heartbeat|silent-hang|heartbeat-busy")
	heartbeatMs := flag.Int("heartbeat-idle-ms", 5000,
		"Idle time before sending heartbeat PING (ms)")
	flag.Parse()

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
func recoverByReboot() {
	// Drop a breadcrumb so the boot-accounting service can attribute the upcoming
	// reboot to us (vs an uninitiated hard reset). Written before Sync so it's
	// flushed to disk along with everything else.
	writeRebootReason("go-peer-dead (M4F unreachable via heartbeat/device-gone)")

	syscall.Sync() // flush to SD before going down

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