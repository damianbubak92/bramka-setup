package main

import (
	"flag"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	testMode := flag.String("test", "hello",
		"Test mode: 'hello' (HELLO/HELLO_ACK), 'data' (DATA exchange), 'spam' (multiple DATAs)")
	flag.Parse()

	log.SetFlags(log.LstdFlags | log.Lmicroseconds)
	log.Printf("=== rpmsg-service starting (test mode: %s) ===", *testMode)

	// Setup transport
	t, err := OpenTransport()
	if err != nil {
		log.Fatalf("OpenTransport: %v", err)
	}
	defer t.Close()

	// Setup protocol layer
	p := NewProtocol(t)
	defer p.Close()

	// Signal handler
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)

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
		default:
			log.Printf("Unknown test mode: %s", *testMode)
		}
	}()

	// Wait for signal
	sig := <-sigChan
	log.Printf("Received signal: %v, shutting down...", sig)
}

func runHelloTest(p *Protocol) {
	time.Sleep(200 * time.Millisecond) // give dispatcher time to start

	log.Println("[Test] Sending HELLO...")
	if err := p.Hello(3 * time.Second); err != nil {
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