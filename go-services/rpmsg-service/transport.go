package main

import (
	"fmt"
	"io"
	"log"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

const (
	m4fHwId      = "5000000.m4fss"
	chrdevSuffix = "rpmsg_chrdev"
	rxBufSize    = 512

	// Cold-boot race: at boot systemd may start us before M4F has announced
	// its rpmsg_chrdev endpoint (and the kernel created /dev/rpmsgN). Wait for
	// the device instead of exiting immediately. Bounded well under the unit's
	// TimeoutStartSec=30s (READY=1 is sent right after OpenTransport).
	deviceWaitTimeout  = 20 * time.Second
	deviceWaitInterval = 500 * time.Millisecond
)

// Transport encapsulates RPMsg device access.
// Provides Send (synchronous write) and a channel of received raw bytes.
type Transport struct {
	devPath string
	file    *os.File
	rxChan  chan []byte
	stopCh  chan struct{}
	wg      sync.WaitGroup

	// deviceGoneCh is closed (once) when the rpmsg device disappears at runtime
	// (M4F crashed/stopped/unloaded). On AM62 there is no per-core M4F reset, so
	// a vanished endpoint always means the peer is dead and a full reset is the
	// only recovery. Created in OpenTransport (before readerLoop), so the writer
	// (reader goroutine) and reader (Protocol) are race-free.
	deviceGoneCh   chan struct{}
	deviceGoneOnce sync.Once
}

// OpenTransport finds the M4F RPMsg chrdev and opens it for RDWR.
func OpenTransport() (*Transport, error) {
	devPath, err := waitForM4FChrdev(deviceWaitTimeout)
	if err != nil {
		return nil, err
	}

	f, err := os.OpenFile(devPath, os.O_RDWR, 0)
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", devPath, err)
	}

	t := &Transport{
		devPath:      devPath,
		file:         f,
		rxChan:       make(chan []byte, 32),
		stopCh:       make(chan struct{}),
		deviceGoneCh: make(chan struct{}),
	}

	// Start reader goroutine
	t.wg.Add(1)
	go t.readerLoop()

	log.Printf("[Transport] Opened %s", devPath)
	return t, nil
}

// Send writes raw bytes to the device. Synchronous.
func (t *Transport) Send(data []byte) error {
	n, err := t.file.Write(data)
	if err != nil {
		return fmt.Errorf("write: %w", err)
	}
	if n != len(data) {
		return fmt.Errorf("short write: %d/%d", n, len(data))
	}
	return nil
}

// Rx returns channel of received raw byte arrays.
func (t *Transport) Rx() <-chan []byte {
	return t.rxChan
}

// DeviceGone returns a channel closed when the rpmsg device disappears at
// runtime. On AM62 this means the M4F is gone and only a full reset recovers it;
// callers should treat it as immediate peer-dead (no need to wait out heartbeat).
func (t *Transport) DeviceGone() <-chan struct{} {
	return t.deviceGoneCh
}

// signalDeviceGone closes deviceGoneCh exactly once. Safe to call concurrently.
func (t *Transport) signalDeviceGone() {
	t.deviceGoneOnce.Do(func() { close(t.deviceGoneCh) })
}

// Close stops reader and closes device.
func (t *Transport) Close() error {
	close(t.stopCh)
	err := t.file.Close()
	t.wg.Wait()
	close(t.rxChan)
	log.Println("[Transport] Closed")
	return err
}

func (t *Transport) readerLoop() {
	defer t.wg.Done()
	buf := make([]byte, rxBufSize)
	log.Println("[Transport] Reader started")

	for {
		select {
		case <-t.stopCh:
			return
		default:
		}

		n, err := t.file.Read(buf)
		if err != nil {
			// EOF lub explicit close = normalny shutdown
			if err == io.EOF {
				log.Println("[Transport] EOF")
				return
			}
			errStr := err.Error()
			
			// Device closed by main (our shutdown path)
			if strings.Contains(errStr, "closed") || strings.Contains(errStr, "use of closed") {
				log.Println("[Transport] Device closed, reader exiting")
				return
			}
			
			// Device went away (M4F crashed / stopped / unloaded)
			// EPIPE = broken pipe, ENODEV = "not pollable" on disappeared rpmsg device
			if strings.Contains(errStr, "broken pipe") ||
			   strings.Contains(errStr, "not pollable") ||
			   strings.Contains(errStr, "no such device") {
				log.Printf("[Transport] FATAL: device disappeared (%v) - signaling peer dead", err)
				t.signalDeviceGone()
				return
			}
			
			// Inne błędy - log i retry z backoff (transient errors)
			log.Printf("[Transport] Read error: %v", err)
			time.Sleep(100 * time.Millisecond)
			continue
		}
		if n == 0 {
			continue
		}

		// Skopiuj bo buf jest reusowany
		msg := make([]byte, n)
		copy(msg, buf[:n])

		select {
		case t.rxChan <- msg:
		default:
			log.Printf("[Transport] WARN: rxChan full, dropping %d bytes", n)
		}
	}
}

// waitForM4FChrdev polls for the M4F rpmsg chrdev until it appears or timeout.
// Handles the cold-boot race where the service starts before M4F has announced
// its rpmsg_chrdev endpoint.
func waitForM4FChrdev(timeout time.Duration) (string, error) {
	deadline := time.Now().Add(timeout)
	attempt := 0
	for {
		devPath, err := findM4FChrdev()
		if err == nil {
			if attempt > 0 {
				log.Printf("[Transport] M4F rpmsg device ready after %v: %s",
					time.Duration(attempt)*deviceWaitInterval, devPath)
			}
			return devPath, nil
		}
		if time.Now().After(deadline) {
			return "", fmt.Errorf("findM4FChrdev: no device within %v: %w", timeout, err)
		}
		if attempt == 0 {
			log.Printf("[Transport] M4F rpmsg device not ready, waiting up to %v...", timeout)
		}
		attempt++
		time.Sleep(deviceWaitInterval)
	}
}

func findM4FChrdev() (string, error) {
	matches, _ := filepath.Glob("/sys/class/rpmsg/rpmsg[0-9]*")
	for _, sysPath := range matches {
		realPath, err := filepath.EvalSymlinks(sysPath + "/device")
		if err != nil {
			continue
		}
		if !strings.Contains(realPath, m4fHwId) || !strings.Contains(realPath, chrdevSuffix) {
			continue
		}
		ueventBytes, err := os.ReadFile(sysPath + "/uevent")
		if err != nil {
			continue
		}
		for _, line := range strings.Split(string(ueventBytes), "\n") {
			if strings.HasPrefix(line, "DEVNAME=") {
				return "/dev/" + strings.TrimPrefix(line, "DEVNAME="), nil
			}
		}
	}
	return "", fmt.Errorf("no M4F rpmsg_chrdev device found")
}