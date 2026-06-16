package main

/*
#include "protocol.h"
*/
import "C"

import (
	"fmt"
	"log"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"
)

// ProtocolState represents connection state.
type ProtocolState int

const (
	StateDisconnected ProtocolState = iota
	StateHelloSent
	StateConnected
	StateDead
)

// MSG_DEBUG_CRASH is DEBUG-ONLY message type to force M4F hard fault.
// NEVER use in production code.
//const MSG_DEBUG_CRASH = 0xF0

func (s ProtocolState) String() string {
	switch s {
	case StateDisconnected:
		return "DISCONNECTED"
	case StateHelloSent:
		return "HELLO_SENT"
	case StateConnected:
		return "CONNECTED"
	case StateDead:
		return "DEAD"
	}
	return "UNKNOWN"
}

// pendingAck represents an outstanding message awaiting ACK.
type pendingAck struct {
	seq        uint16
	msgType    uint8
	payload    []byte
	encoded    []byte // pre-encoded bytes for retry (incl. CRC)
	sentAt     time.Time
	retryCount uint8
	doneCh     chan error // nil = success, error = giveup
}

// Protocol implements the binary protocol state machine.
type Protocol struct {
	transport *Transport
	state     ProtocolState
	stateMu   sync.Mutex

    // === DEBUG: drop first N ACKs (for retry testing) ===
    debugDropAcks atomic.Int32

	// Sequence number counters (separate TX and RX)
	mySeq         atomic.Uint32 // counter for our outgoing messages
	theirLastSeq  uint16        // last seq we received (for idempotency)
	theirSeqMu    sync.Mutex

	// Pending ACK table - keyed by seq
	pending   map[uint16]*pendingAck
	pendingMu sync.Mutex

	// Channels for application layer
	dataRxCh  chan ReceivedMessage // application data messages
	eventRxCh chan ReceivedMessage // events from M4F

// Lifecycle
	stopCh chan struct{}
	wg     sync.WaitGroup

	// === HEARTBEAT (smart, idle-triggered) ===

	// heartbeatIdle: how long peer must be silent before we send PING.
	// Configurable via NewProtocol parameter.
	heartbeatIdle time.Duration

	// lastRxTime: unix nano timestamp of last received message of any type.
	// Updated in handleIncoming for every successfully decoded message.
	lastRxTime atomic.Int64

	// pingInFlight: prevents duplicate heartbeat PINGs when retries pending.
	// Set when PING added to pending table, cleared on ACK or giveup.
	pingInFlight atomic.Bool

	// peerDeadCh: closed when heartbeat PING gives up (peer unreachable).
	// Main goroutine should select on PeerDeadCh() for recovery action.
	peerDeadCh   chan struct{}
	peerDeadOnce sync.Once
}

// ReceivedMessage is what gets passed to application layer.
type ReceivedMessage struct {
	Type    uint8
	Seq     uint16
	Payload []byte
}

func NewProtocol(t *Transport, heartbeatIdle time.Duration) *Protocol {
	p := &Protocol{
		transport:     t,
		state:         StateDisconnected,
		pending:       make(map[uint16]*pendingAck),
		dataRxCh:      make(chan ReceivedMessage, 16),
		eventRxCh:     make(chan ReceivedMessage, 16),
		stopCh:        make(chan struct{}),
		heartbeatIdle: heartbeatIdle,
		peerDeadCh:    make(chan struct{}),
	}
	p.mySeq.Store(0) // we'll increment to 1 on first use
	p.lastRxTime.Store(time.Now().UnixNano())

	// Start dispatcher, retry, heartbeat, device-gone workers
	p.wg.Add(4)
	go p.dispatchLoop()
	go p.retryLoop()
	go p.heartbeatLoop()
	go p.deviceGoneWatcher()

	log.Printf("[Protocol] Initialized (heartbeat idle: %v)", heartbeatIdle)
	return p
}

// PeerDeadCh returns channel closed when peer detected unreachable
// (heartbeat PING gave up after retries). Main goroutine should select on this.
func (p *Protocol) PeerDeadCh() <-chan struct{} {
	return p.peerDeadCh
}

// State returns current connection state.
func (p *Protocol) State() ProtocolState {
	p.stateMu.Lock()
	defer p.stateMu.Unlock()
	return p.state
}

// SetDebugDropAcks tells the protocol to silently drop the next N ACKs
// received (simulating lost transport). Used for retry testing.
func (p *Protocol) SetDebugDropAcks(n int32) {
    p.debugDropAcks.Store(n)
    log.Printf("[Protocol] DEBUG: will drop next %d ACKs", n)
}

func (p *Protocol) setState(s ProtocolState) {
	p.stateMu.Lock()
	old := p.state
	p.state = s
	p.stateMu.Unlock()
	if old != s {
		log.Printf("[Protocol] State: %s -> %s", old, s)
	}
}

// Hello sends HELLO and waits for HELLO_ACK (with timeout).
func (p *Protocol) Hello(timeout time.Duration) error {
	payload := []byte("Linux v1 ready")
	seq := uint16(p.mySeq.Add(1))

	encoded, err := encodeMessage(C.MSG_HELLO, seq, payload)
	if err != nil {
		return fmt.Errorf("encode HELLO: %w", err)
	}

	p.setState(StateHelloSent)
	log.Printf("[Protocol] TX HELLO seq=%d (%d bytes)", seq, len(encoded))

	if err := p.transport.Send(encoded); err != nil {
		p.setState(StateDisconnected)
		return fmt.Errorf("send HELLO: %w", err)
	}

	// Wait for state change to CONNECTED (set by dispatchLoop on HELLO_ACK)
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if p.State() == StateConnected {
			return nil
		}
		time.Sleep(50 * time.Millisecond)
	}

	p.setState(StateDead)
	return fmt.Errorf("HELLO_ACK timeout after %v", timeout)
}

// SendData sends application DATA and waits for ACK. Returns nil on success.
// Implements retry with backoff.
func (p *Protocol) SendData(payload []byte, ackTimeout time.Duration) error {
	if p.State() != StateConnected {
		return fmt.Errorf("not connected (state=%s)", p.State())
	}

	seq := uint16(p.mySeq.Add(1))
	encoded, err := encodeMessage(C.MSG_DATA, seq, payload)
	if err != nil {
		return fmt.Errorf("encode DATA: %w", err)
	}

	// Register in pending table
	doneCh := make(chan error, 1)
	pa := &pendingAck{
		seq:        seq,
		msgType:    C.MSG_DATA,
		payload:    payload,
		encoded:    encoded,
		sentAt:     time.Now(),
		retryCount: 0,
		doneCh:     doneCh,
	}

	p.pendingMu.Lock()
	p.pending[seq] = pa
	p.pendingMu.Unlock()

	log.Printf("[Protocol] TX DATA seq=%d (%d bytes payload)", seq, len(payload))
	if err := p.transport.Send(encoded); err != nil {
		p.removePending(seq)
		return fmt.Errorf("send DATA: %w", err)
	}

	// Wait for ACK or giveup
	select {
	case err := <-doneCh:
		return err
case <-time.After(time.Duration(C.MAX_RETRIES+2) * time.Duration(C.ACK_TIMEOUT_MS) * time.Millisecond):
    // Safety net - this shouldn't happen, retry loop should give up first
    p.removePending(seq)
    return fmt.Errorf("DATA seq=%d giveup (overall timeout)", seq)
	}
}

// SendDataWithSeq is a DEBUG-ONLY function that sends DATA with a specific
// seq number (bypassing the counter). Used for idempotency testing.
func (p *Protocol) SendDataWithSeq(payload []byte, forcedSeq uint16,
    ackTimeout time.Duration) error {
    if p.State() != StateConnected {
        return fmt.Errorf("not connected (state=%s)", p.State())
    }

    encoded, err := encodeMessage(C.MSG_DATA, forcedSeq, payload)
    if err != nil {
        return fmt.Errorf("encode DATA: %w", err)
    }

    doneCh := make(chan error, 1)
    pa := &pendingAck{
        seq:        forcedSeq,
        msgType:    C.MSG_DATA,
        payload:    payload,
        encoded:    encoded,
        sentAt:     time.Now(),
        retryCount: 0,
        doneCh:     doneCh,
    }

    p.pendingMu.Lock()
    p.pending[forcedSeq] = pa
    p.pendingMu.Unlock()

    log.Printf("[Protocol] TX DATA seq=%d (FORCED, %d bytes payload)",
        forcedSeq, len(payload))
    if err := p.transport.Send(encoded); err != nil {
        p.removePending(forcedSeq)
        return fmt.Errorf("send DATA: %w", err)
    }

    select {
    case err := <-doneCh:
        return err
    case <-time.After(ackTimeout + time.Duration(C.MAX_RETRIES)*time.Second):
        p.removePending(forcedSeq)
        return fmt.Errorf("DATA seq=%d giveup", forcedSeq)
    }
}

func (p *Protocol) removePending(seq uint16) {
	p.pendingMu.Lock()
	delete(p.pending, seq)
	p.pendingMu.Unlock()
}

// Close stops all goroutines.
func (p *Protocol) Close() {
	close(p.stopCh)
	p.wg.Wait()
}

// EventRx returns channel of EVENT messages from M4F.
func (p *Protocol) EventRx() <-chan ReceivedMessage {
	return p.eventRxCh
}

// dispatchLoop reads from transport, parses, dispatches based on msg type.
func (p *Protocol) dispatchLoop() {
	defer p.wg.Done()
	log.Println("[Protocol] Dispatcher started")

	for {
		select {
		case <-p.stopCh:
			return
		case raw, ok := <-p.transport.Rx():
			if !ok {
				log.Println("[Protocol] Transport rx channel closed")
				return
			}
			p.handleIncoming(raw)
		}
	}
}

func (p *Protocol) handleIncoming(raw []byte) {
	msgType, seq, payload, err := decodeMessage(raw)
	if err != nil {
		log.Printf("[Protocol] decode failed (%d bytes): %v", len(raw), err)
		return
	}

	// Heartbeat: any valid received message proves M4F is alive
	p.lastRxTime.Store(time.Now().UnixNano())

	log.Printf("[Protocol] RX type=0x%02X seq=%d payload_len=%d",
		msgType, seq, len(payload))

	switch msgType {
	case C.MSG_HELLO_ACK:
		if p.State() == StateHelloSent {
			p.setState(StateConnected)
			log.Printf("[Protocol] HELLO_ACK from M4F: %q", string(payload))
		} else {
			log.Printf("[Protocol] WARN: HELLO_ACK in state %s", p.State())
		}

	case C.MSG_ACK:
		// DEBUG: drop ACKs for retry testing
		if dropsLeft := p.debugDropAcks.Load(); dropsLeft > 0 {
			p.debugDropAcks.Store(dropsLeft - 1)
			log.Printf("[Protocol] DEBUG: DROPPING ACK seq=%d (debug, %d more to drop)",
				seq, dropsLeft-1)
			break
		}
		
		// Find pending entry by seq
		p.pendingMu.Lock()
		pa, exists := p.pending[seq]
		if exists {
			delete(p.pending, seq)
		}
		p.pendingMu.Unlock()

		if exists {
			rtt := time.Since(pa.sentAt)
			log.Printf("[Protocol] ACK seq=%d type=0x%02X (RTT %v, retries=%d)",
				seq, pa.msgType, rtt, pa.retryCount)

			// Clear PING-in-flight if this was the heartbeat PING
			if pa.msgType == C.MSG_PING {
				p.pingInFlight.Store(false)
			}

			select {
			case pa.doneCh <- nil:
			default:
				log.Printf("[Protocol] WARN: doneCh full for seq=%d, success dropped", pa.seq)
			}
		} else {
			log.Printf("[Protocol] WARN: ACK for unknown seq=%d (already acked?)", seq)
		}

	case C.MSG_DATA:
		// Idempotency check
		p.theirSeqMu.Lock()
		isDup := (p.theirLastSeq != 0) && (seq <= p.theirLastSeq)
		if !isDup {
			p.theirLastSeq = seq
		}
		p.theirSeqMu.Unlock()

		if isDup {
			log.Printf("[Protocol] DATA seq=%d duplicate (lastSeq=%d) - re-ACK only",
				seq, p.theirLastSeq)
			p.sendAck(seq)
			return
		}

		// Send ACK first, then dispatch to app
		p.sendAck(seq)
		select {
		case p.dataRxCh <- ReceivedMessage{Type: msgType, Seq: seq, Payload: payload}:
		default:
			log.Printf("[Protocol] WARN: dataRx full, dropping seq=%d", seq)
		}

	case C.MSG_EVENT:
		// Same as DATA but routed to event channel
		p.theirSeqMu.Lock()
		isDup := (p.theirLastSeq != 0) && (seq <= p.theirLastSeq)
		if !isDup {
			p.theirLastSeq = seq
		}
		p.theirSeqMu.Unlock()

		p.sendAck(seq)
		if !isDup {
			select {
			case p.eventRxCh <- ReceivedMessage{Type: msgType, Seq: seq, Payload: payload}:
			default:
				log.Printf("[Protocol] WARN: eventRx full, dropping seq=%d", seq)
			}
		}

	case C.MSG_PING:
		// Smart heartbeat from M4F - reply with generic ACK
		log.Printf("[Protocol] RX heartbeat PING seq=%d - replying ACK", seq)
		p.sendAck(seq)
		// lastRxTime already updated at top of handleIncoming

	case C.MSG_PONG:
		// Deprecated message type - silently ignore for backwards compat
		log.Printf("[Protocol] RX PONG (deprecated, ignored)")

	default:
		log.Printf("[Protocol] Unknown msg type 0x%02X (ignored)", msgType)
	}
}

func (p *Protocol) sendAck(seq uint16) {
	encoded, err := encodeMessage(C.MSG_ACK, seq, nil)
	if err != nil {
		log.Printf("[Protocol] encode ACK failed: %v", err)
		return
	}
	if err := p.transport.Send(encoded); err != nil {
		log.Printf("[Protocol] send ACK failed: %v", err)
	}
}

func (p *Protocol) sendSimple(msgType uint8, seq uint16, payload []byte) {
	encoded, err := encodeMessage(msgType, seq, payload)
	if err != nil {
		log.Printf("[Protocol] encode 0x%02X failed: %v", msgType, err)
		return
	}
	if err := p.transport.Send(encoded); err != nil {
		log.Printf("[Protocol] send 0x%02X failed: %v", msgType, err)
	}
}

// retryLoop periodically scans pending table and retries timed-out messages.
func (p *Protocol) retryLoop() {
	defer p.wg.Done()
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	log.Println("[Protocol] Retry worker started")

	for {
		select {
		case <-p.stopCh:
			return
		case now := <-ticker.C:
			p.processRetries(now)
		}
	}
}

func (p *Protocol) processRetries(now time.Time) {
	var toRetry []*pendingAck
	var toGiveup []*pendingAck

	p.pendingMu.Lock()
	for seq, pa := range p.pending {
		elapsed := now.Sub(pa.sentAt)
		if elapsed < time.Duration(C.ACK_TIMEOUT_MS)*time.Millisecond {
			continue
		}
		if pa.retryCount >= C.MAX_RETRIES {
			toGiveup = append(toGiveup, pa)
			delete(p.pending, seq)
		} else {
			pa.retryCount++
			pa.sentAt = now
			toRetry = append(toRetry, pa)
		}
	}
	p.pendingMu.Unlock()

	for _, pa := range toRetry {
		log.Printf("[Protocol] RETRY seq=%d count=%d", pa.seq, pa.retryCount)
		if err := p.transport.Send(pa.encoded); err != nil {
			log.Printf("[Protocol] retry send failed seq=%d: %v", pa.seq, err)
		}
	}
	for _, pa := range toGiveup {
		log.Printf("[Protocol] GIVEUP seq=%d (%d retries exhausted)",
			pa.seq, pa.retryCount)
		select {
		case pa.doneCh <- fmt.Errorf("seq=%d: ACK timeout after %d retries",
			pa.seq, pa.retryCount):
		default:
			log.Printf("[Protocol] WARN: doneCh full for seq=%d, error dropped", pa.seq)
		}
	}
}

// heartbeatLoop sends PING when idle threshold reached (and no PING pending).
// Runs as separate goroutine. Driven by 1s ticker.
func (p *Protocol) heartbeatLoop() {
	defer p.wg.Done()
	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()
	log.Printf("[Protocol] Heartbeat loop started (idle threshold: %v)", p.heartbeatIdle)

	for {
		select {
		case <-p.stopCh:
			return
		case <-ticker.C:
			if p.State() != StateConnected {
				continue
			}
			if p.pingInFlight.Load() {
				continue // already pinging, wait for ACK or giveup
			}

			idleNs := time.Now().UnixNano() - p.lastRxTime.Load()
			if time.Duration(idleNs) < p.heartbeatIdle {
				continue // recent traffic, no need to ping
			}

			p.sendHeartbeatPing()
		}
	}
}

// sendHeartbeatPing sends MSG_PING via pending ACK mechanism (with retries).
// On giveup, signals peerDeadCh.
func (p *Protocol) sendHeartbeatPing() {
	seq := uint16(p.mySeq.Add(1))
	encoded, err := encodeMessage(C.MSG_PING, seq, nil)
	if err != nil {
		log.Printf("[Protocol] heartbeat encode failed: %v", err)
		return
	}

	doneCh := make(chan error, 1)
	pa := &pendingAck{
		seq:        seq,
		msgType:    C.MSG_PING,
		payload:    nil,
		encoded:    encoded,
		sentAt:     time.Now(),
		retryCount: 0,
		doneCh:     doneCh,
	}

	p.pendingMu.Lock()
	p.pending[seq] = pa
	p.pendingMu.Unlock()

	p.pingInFlight.Store(true)

	idleMs := (time.Now().UnixNano() - p.lastRxTime.Load()) / int64(time.Millisecond)
	log.Printf("[Protocol] TX heartbeat PING seq=%d (idle %d ms)", seq, idleMs)

	if err := p.transport.Send(encoded); err != nil {
		log.Printf("[Protocol] heartbeat send failed: %v", err)
		p.removePending(seq)
		p.pingInFlight.Store(false)
		return
	}

	// Async watcher: when doneCh resolves (ACK or giveup), act on result
	go func() {
		err := <-doneCh
		p.pingInFlight.Store(false)
		if err != nil {
			log.Printf("[Protocol] *** HEARTBEAT FAILED: peer unreachable (%v) ***", err)
			p.signalPeerDead()
		}
		// On success: ACK already logged in dispatcher, nothing to do
	}()
}

// deviceGoneWatcher bridges a transport-level device disappearance to the
// peer-dead path. On AM62 a vanished rpmsg endpoint means the M4F is gone and
// only a full reset recovers it, so we signal peer dead immediately instead of
// waiting for the heartbeat to time out (~9s). Same end action (clean reboot).
func (p *Protocol) deviceGoneWatcher() {
	defer p.wg.Done()
	select {
	case <-p.stopCh:
		return
	case <-p.transport.DeviceGone():
		log.Printf("[Protocol] *** TRANSPORT device gone - peer unreachable, signaling peer dead ***")
		p.signalPeerDead()
	}
}

// signalPeerDead transitions to DEAD and closes peerDeadCh (idempotent).
func (p *Protocol) signalPeerDead() {
	p.peerDeadOnce.Do(func() {
		p.setState(StateDead)
		close(p.peerDeadCh)
	})
}

// encodeMessage wraps protocol_encode from C side.
func encodeMessage(msgType uint8, seq uint16, payload []byte) ([]byte, error) {
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
		return nil, fmt.Errorf("encode returned 0 (payload too large?)")
	}
	return buf[:encLen], nil
}

// decodeMessage wraps protocol_decode from C side.
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
		return 0, 0, nil, fmt.Errorf("decode rc=%d", int(rc))
	}

	var payload []byte
	if outPayloadLen > 0 {
		payload = C.GoBytes(unsafe.Pointer(outPayloadPtr), C.int(outPayloadLen))
	}

	return uint8(outType), uint16(outSeq), payload, nil
}

// SendDebugCrash sends a DEBUG_CRASH message to force M4F hard fault.
// Used to test Linux remoteproc auto-recovery.
// DEBUG ONLY - never use in production.
func (p *Protocol) SendDebugCrash() error {
    if p.State() != StateConnected {
        return fmt.Errorf("not connected (state=%s)", p.State())
    }
    
    seq := uint16(p.mySeq.Add(1))
    encoded, err := encodeMessage(C.MSG_DEBUG_CRASH, seq, []byte("CRASH"))
    if err != nil {
        return fmt.Errorf("encode DEBUG_CRASH: %w", err)
    }
    
    log.Printf("[Protocol] TX DEBUG_CRASH seq=%d (forcing M4F hard fault!)", seq)
    if err := p.transport.Send(encoded); err != nil {
        return fmt.Errorf("send DEBUG_CRASH: %w", err)
    }
    
    return nil
}

// SendDebugHang sends a DEBUG_HANG message to force M4F into infinite loop.
// Used to test heartbeat-based silent hang detection.
// DEBUG ONLY - never use in production.
func (p *Protocol) SendDebugHang() error {
	if p.State() != StateConnected {
		return fmt.Errorf("not connected (state=%s)", p.State())
	}

	seq := uint16(p.mySeq.Add(1))
	encoded, err := encodeMessage(C.MSG_DEBUG_HANG, seq, []byte("HANG"))
	if err != nil {
		return fmt.Errorf("encode DEBUG_HANG: %w", err)
	}

	log.Printf("[Protocol] TX DEBUG_HANG seq=%d (forcing M4F infinite loop!)", seq)
	if err := p.transport.Send(encoded); err != nil {
		return fmt.Errorf("send DEBUG_HANG: %w", err)
	}

	return nil
}