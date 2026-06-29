# Binary Protocol - Linux ↔ M4F Communication

## Cel

Niezawodna komunikacja między userspace na Linux (Go service) a firmware M4F
(NoRTOS) przez RPMsg. Single source of truth: `shared/protocol.h`.

## Wire format

Każda wiadomość ma stałe nagłówki, payload zmiennej długości, CRC16 na końcu:
[1 byte: TYPE] [2 bytes: SEQ] [2 bytes: PAYLOAD_LEN] [N bytes: PAYLOAD] [2 bytes: CRC16]

Wszystkie pola wielobajtowe w **big-endian** (network byte order).
Header struct ma `__attribute__((packed))` — zero padding.
Total overhead: 7 bajtów (5 header + 2 CRC).
Max total message: 487 bajtów (480 payload + 7 overhead).

## CRC16

Polynomial: `0x1021` (CRC16-CCITT, XMODEM variant)
Initial value: `0xFFFF`
Bez reflection, bez xorout.
Pokrywa **header + payload** (nie CRC field).

Test vector: `crc16("123456789") == 0x29B1`.

## Typy wiadomości

| Hex | Nazwa | Kierunek | ACK? | Opis |
|-----|-------|----------|------|------|
| 0x01 | HELLO | L→M | TAK | Connection request |
| 0x02 | HELLO_ACK | M→L | NIE | Connection accepted |
| 0x03 | PING | Bidir | NIE | Heartbeat probe |
| 0x04 | PONG | Bidir | NIE | Heartbeat reply |
| 0x10 | DATA | Bidir | TAK | Application data |
| 0x11 | ACK | Bidir | NIE | Acknowledge DATA seq=N |
| 0x20 | EVENT | M→L | TAK | Async event from M4F |
| 0xFF | ERROR | Bidir | NIE | Protocol error notification |

## Limity i timeouty

- `MAX_PAYLOAD_SIZE`: 480 bajtów
- `MAX_PENDING_ACKS`: 8 (per direction)
- `MAX_RETRIES`: 3
- `ACK_TIMEOUT_MS`: 1000ms
- `HEARTBEAT_IDLE_MS`: 5000ms (wyślij PING jeśli idle)
- `HEARTBEAT_DEAD_MS`: 15000ms (uznaj connection dead)

## Connection lifecycle
Linux startuje                      M4F już lecący
|                                   |
| --- HELLO(seq=1) -----------►     |
|   "Linux v1 ready"                |
|                                   |
|   ◄---- HELLO_ACK(seq=1) ----     |
|        "M4F v1 ready"             |
|                                   |
| === CONNECTED ===                 |
|                                   |
| --- DATA(seq=2, payload) -----►   |
|   ◄---- ACK(seq=2) ----           |
|                                   |
|   ◄---- EVENT(seq=42, ...) ----   |
| --- ACK(seq=42) -----►            |

## Sequence numbers

- Każda strona ma swój counter (mySeq) - osobny dla TX i RX
- Wrap-around: po 0xFFFF wraca do 0x0001 (0x0000 reserved jako "invalid")
- Idempotency: jeśli RX seq <= lastSeq, ponowny ACK bez re-process

## Smart heartbeat (keepalive on idle)

- Każda wiadomość (DATA, ACK, EVENT, etc.) **resetuje** idle timer
- Jeśli idle > 5s → wyślij PING
- Jeśli idle > 15s (3 missed heartbeats) → mark connection DEAD
- Reconnect: nowy HELLO

## Implementacja

### Single source of truth: `shared/protocol.h`

Plik zawiera:
- Stałe (typy, limity, timeouty)
- `msg_header_t` struct (packed)
- `protocol_crc16()` inline function
- `protocol_encode()` inline function
- `protocol_decode()` inline function
- Big-endian helpers

### M4F (C, NoRTOS)

Include `protocol.h` w `ipc_rpmsg_echo.c`. Wywołuj `protocol_encode`/`protocol_decode`
przed/po `RPMessage_send`/callback.

### Go service (Linux userspace)

Używa **cgo** do importu `protocol.h`:

```go
/*
#include "protocol.h"
*/
import "C"
```

Stałe z C dostępne jako `C.MSG_HELLO`, struktury jako `C.msg_header_t`.

## Testowanie

### CRC compatibility check
`testProtocol()` w M4F i `crc_test.go` na bramce wypisują CRC dla
tych samych test vectors. Wartości **muszą być identyczne**.

### Round-trip encode/decode
Encode wiadomości w M4F i Go, porównaj bajty. **Muszą być identyczne**.

### End-to-end
HELLO/HELLO_ACK exchange. Latency ~5-10ms na bramce (RPMsg roundtrip).

## Test modes (rpmsg-service)

Go service obsługuje kilka test modes uruchamianych przez flagę `-test`:
./rpmsg-service -test hello       # Tylko HELLO/HELLO_ACK exchange
./rpmsg-service -test data        # HELLO + pojedyncze DATA z ACK
./rpmsg-service -test spam        # HELLO + 5 DATA w pętli (RTT test)
./rpmsg-service -test retry-drop  # Symulacja zgubionych ACK (retry validation)
./rpmsg-service -test replay      # Duplicate seq (idempotency validation)
./rpmsg-service -test event       # Listener na EVENT od M4F

## State machine (Linux side)

DISCONNECTED  → SendHello() → HELLO_SENT
HELLO_SENT    → RX HELLO_ACK → CONNECTED
CONNECTED     → (heartbeat timeout) → DEAD       # TODO future
DEAD          → SendHello() → HELLO_SENT (reconnect)

M4F nie ma jawnego stanu connection - obecność `gLinuxEndpoint != 0` służy jako proxy.
HELLO **resetuje** sequence counters i pending table (idempotent reconnect handling).

## Retry algorithm

Linux side (Go):
1. `SendData(payload)` zwiększa mySeq, encoded + transport.Send
2. Wpis dodany do `pending[seq]` z timestampem
3. `retryLoop` co 100ms skanuje pending:
   - Jeśli `elapsed > ACK_TIMEOUT_MS` AND `retry_count < MAX_RETRIES`:
     - `retry_count++`, re-send via transport
   - Jeśli `retry_count >= MAX_RETRIES`:
     - GIVEUP, doneCh ← error
4. ACK od peer:
   - Lookup pending[seq], usuń, doneCh ← nil
5. `SendData` zwraca z `<-doneCh`

M4F side (mirror):
- `gPendingAcks[MAX_PENDING_ACKS]` statyczna tablica
- `sendEvent()` znajduje wolny slot, fill, send
- `processEventRetries()` (callable z main loop) skanuje tablicę
- Same flow as Go ale w C bez heap allocations

## Debug features

- `Protocol.SetDebugDropAcks(N)` - drop next N incoming ACKs (Go-side retry test)
- `Protocol.SendDataWithSeq(payload, forcedSeq, ...)` - send DATA z konkretnym seq (idempotency test)