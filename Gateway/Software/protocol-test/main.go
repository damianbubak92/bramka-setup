package main

/*
#include "protocol.h"
*/
import "C"

import (
	"flag"
	"fmt"
	"io"
	"log"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

const (
	m4fHwId      = "5000000.m4fss"
	chrdevSuffix = "rpmsg_chrdev"
)

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

func encodeMessage(msgType uint8, seq uint16, payload []byte) []byte {
	bufSize := C.MSG_MAX_TOTAL
	buf := make([]byte, bufSize)

	var payloadPtr *C.uint8_t
	if len(payload) > 0 {
		payloadPtr = (*C.uint8_t)(unsafe.Pointer(&payload[0]))
	}

	encLen := C.protocol_encode(
		(*C.uint8_t)(unsafe.Pointer(&buf[0])),
		C.uint8_t(msgType),
		C.uint16_t(seq),
		payloadPtr,
		C.uint16_t(len(payload)),
	)

	if encLen == 0 {
		return nil
	}
	return buf[:encLen]
}

func decodeMessage(buf []byte) (uint8, uint16, []byte, error) {
	var outType C.uint8_t
	var outSeq C.uint16_t
	var outPayloadPtr *C.uint8_t
	var outPayloadLen C.uint16_t

	rc := C.protocol_decode(
		(*C.uint8_t)(unsafe.Pointer(&buf[0])),
		C.size_t(len(buf)),
		&outType,
		&outSeq,
		&outPayloadPtr,
		&outPayloadLen,
	)

	if rc != 0 {
		return 0, 0, nil, fmt.Errorf("decode error rc=%d", int(rc))
	}

	var payload []byte
	if outPayloadLen > 0 {
		payload = C.GoBytes(unsafe.Pointer(outPayloadPtr), C.int(outPayloadLen))
	}

	return uint8(outType), uint16(outSeq), payload, nil
}

func main() {
	onceMode := flag.Bool("once", false, "Send one HELLO, wait for HELLO_ACK, exit")
	flag.Parse()

	log.SetFlags(log.LstdFlags | log.Lmicroseconds)
	log.Println("=== rpmsg-bridge starting (binary protocol) ===")

	if *onceMode {
		log.Println("Mode: ONCE (single exchange then exit)")
	} else {
		log.Println("Mode: SERVICE (run until Ctrl+C)")
	}

	devPath, err := findM4FChrdev()
	if err != nil {
		log.Fatalf("Cannot find M4F device: %v", err)
	}
	log.Printf("Detected M4F chrdev: %s", devPath)

	f, err := os.OpenFile(devPath, os.O_RDWR, 0)
	if err != nil {
		log.Fatalf("Cannot open %s: %v", devPath, err)
	}
	defer func() {
		log.Println("Closing device...")
		f.Close()
		log.Println("Device closed")
	}()
	log.Printf("Opened %s", devPath)

	// Signal handler dla Ctrl+C
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)

	// Channel dla received messages (komunikacja reader → main)
	rxChan := make(chan rxMsg, 16)
	readerDone := make(chan struct{})
	go reader(f, rxChan, readerDone)

	// Daj reader czas na start
	time.Sleep(200 * time.Millisecond)

	// Wyślij HELLO
	if err := sendHello(f); err != nil {
		log.Printf("sendHello failed: %v", err)
		os.Exit(1)
	}

	// Czekaj na HELLO_ACK (z timeoutem) lub Ctrl+C
	helloAckReceived := false
	timeout := time.After(3 * time.Second)

waitLoop:
	for {
		select {
		case msg := <-rxChan:
			if msg.msgType == C.MSG_HELLO_ACK {
				log.Printf("✓ HELLO_ACK received: %q (latency from TX)", string(msg.payload))
				helloAckReceived = true
				if *onceMode {
					break waitLoop // w once mode wychodzimy po HELLO_ACK
				}
			} else {
				log.Printf("? Unexpected msg type 0x%02X (continuing)", msg.msgType)
			}
		case <-timeout:
			if !helloAckReceived {
				log.Println("✗ HELLO_ACK timeout (3s) - M4F not responding")
				if *onceMode {
					os.Exit(2)
				}
			}
			// W service mode po timeout - timeout się ustawia ponownie? Nie, raz.
			// Po timeout reszta wiadomości będzie przychodzić bez tej obsługi.
			// To OK dla teraz.
		case sig := <-sigChan:
			log.Printf("Received signal: %v", sig)
			break waitLoop
		}

		// W service mode kontynuujemy nawet po HELLO_ACK
		if *onceMode && helloAckReceived {
			break waitLoop
		}
	}

	log.Println("Stopping reader...")

	// Daj reader chwilę na zakończenie (po close file będzie EOF)
	select {
	case <-readerDone:
		log.Println("Reader stopped cleanly")
	case <-time.After(500 * time.Millisecond):
		log.Println("Reader didn't stop in time (will be killed by close)")
	}

	log.Println("=== rpmsg-bridge stopped ===")
}

type rxMsg struct {
	msgType uint8
	seq     uint16
	payload []byte
}

func sendHello(f *os.File) error {
	payload := []byte("Linux v1 ready")
	msg := encodeMessage(C.MSG_HELLO, 1, payload)
	if msg == nil {
		return fmt.Errorf("encode failed")
	}

	log.Printf("[TX] HELLO seq=1 (%d bytes total)", len(msg))
	log.Printf("[TX] bytes: % X", msg)

	n, err := f.Write(msg)
	if err != nil {
		return fmt.Errorf("write: %w", err)
	}
	log.Printf("[TX] sent %d bytes", n)
	return nil
}

func reader(f *os.File, rxChan chan<- rxMsg, done chan<- struct{}) {
	defer close(done)
	defer close(rxChan)
	buf := make([]byte, 512)

	log.Println("[READER] Started, waiting for protocol messages...")

	for {
		n, err := f.Read(buf)
		if err != nil {
			if err == io.EOF {
				log.Println("[READER] EOF (device closed)")
				return
			}
			// Po close() Go zwraca "file already closed" - normalne przy shutdown
			if strings.Contains(err.Error(), "already closed") || strings.Contains(err.Error(), "use of closed") {
				log.Println("[READER] Device closed by main, exiting")
				return
			}
			log.Printf("[READER] Read error: %v", err)
			time.Sleep(100 * time.Millisecond)
			continue
		}
		if n == 0 {
			continue
		}

		log.Printf("[RX] %d bytes raw: % X", n, buf[:n])

		msgType, msgSeq, payload, err := decodeMessage(buf[:n])
		if err != nil {
			log.Printf("[RX] decode failed: %v", err)
			continue
		}

		log.Printf("[RX] type=0x%02X seq=%d payload_len=%d", msgType, msgSeq, len(payload))

		// Wyślij do main przez channel (non-blocking, with fallback log if buffer full)
		select {
		case rxChan <- rxMsg{msgType: msgType, seq: msgSeq, payload: payload}:
		default:
			log.Printf("[RX] WARNING: rx channel full, dropping message type=0x%02X seq=%d",
				msgType, msgSeq)
		}
	}
}