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
)

// Transport encapsulates RPMsg device access.
// Provides Send (synchronous write) and a channel of received raw bytes.
type Transport struct {
	devPath string
	file    *os.File
	rxChan  chan []byte
	stopCh  chan struct{}
	wg      sync.WaitGroup
}

// OpenTransport finds the M4F RPMsg chrdev and opens it for RDWR.
func OpenTransport() (*Transport, error) {
	devPath, err := findM4FChrdev()
	if err != nil {
		return nil, fmt.Errorf("findM4FChrdev: %w", err)
	}

	f, err := os.OpenFile(devPath, os.O_RDWR, 0)
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", devPath, err)
	}

	t := &Transport{
		devPath: devPath,
		file:    f,
		rxChan:  make(chan []byte, 32),
		stopCh:  make(chan struct{}),
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
				log.Printf("[Transport] FATAL: device disappeared (%v) - exiting reader", err)
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