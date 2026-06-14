package main

import (
	"log"
	"net"
	"os"
	"sync/atomic"
)

// DEBUG: when true, sdNotify("WATCHDOG=1") becomes a no-op.
// Used to simulate a hang for testing systemd watchdog reaction.
var debugDisableWatchdog atomic.Bool

func sdNotify(state string) {
	// DEBUG: simulate hang by ignoring WATCHDOG kicks
	if state == "WATCHDOG=1" && debugDisableWatchdog.Load() {
		return
	}

	socketPath := os.Getenv("NOTIFY_SOCKET")
	if socketPath == "" {
		return
	}

	conn, err := net.DialUnix("unixgram", nil, &net.UnixAddr{
		Name: socketPath,
		Net:  "unixgram",
	})
	if err != nil {
		log.Printf("[systemd] notify dial failed: %v", err)
		return
	}
	defer conn.Close()

	if _, err := conn.Write([]byte(state)); err != nil {
		log.Printf("[systemd] notify write failed: %v", err)
	}
}