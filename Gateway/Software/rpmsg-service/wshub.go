package main

// WebSocket hub — the in-app, foreground live channel (phone <-> gateway).
// Distinct from FCM (background alerts, later): WS carries live telemetry,
// provisioning events (JOIN pending, status changes) and — in Phase B — commands
// with request/response correlation (reqId). Runs on the SAME TLS :9443 as the
// HTTP API (same pinned cert), auth by the shared token on the upgrade request.
//
// Standard gorilla hub pattern: one run() goroutine owns the client set; each
// client has a buffered send channel drained by its writePump. Slow clients are
// dropped (never block the broadcaster). Phase A is push-only (server->client);
// readPump consumes control frames + reserves client->server handling for Phase B.
//
// On the bramka the module must be fetched once (internet present):
//   cd <service dir> && go get github.com/gorilla/websocket && go build

import (
	"encoding/json"
	"io"
	"log"
	"net/http"
	"time"

	"github.com/gorilla/websocket"
)

const (
	wsWriteWait  = 10 * time.Second
	wsPongWait   = 60 * time.Second
	wsPingPeriod = 50 * time.Second // must be < wsPongWait
	wsSendBuf    = 64               // per-client outbound queue; overflow => drop client
	wsReadLimit  = 8192
)

var wsUpgrader = websocket.Upgrader{
	ReadBufferSize:  1024,
	WriteBufferSize: 4096,
	// Native app, not a browser: the pinned cert + token are the real auth.
	CheckOrigin: func(r *http.Request) bool { return true },
}

type wsClient struct {
	conn *websocket.Conn
	send chan []byte
}

// WSHub fans gateway events out to all connected phones.
type WSHub struct {
	register   chan *wsClient
	unregister chan *wsClient
	broadcast  chan []byte
	clients    map[*wsClient]bool
}

func newWSHub() *WSHub {
	return &WSHub{
		register:   make(chan *wsClient),
		unregister: make(chan *wsClient),
		broadcast:  make(chan []byte, 256),
		clients:    make(map[*wsClient]bool),
	}
}

func (h *WSHub) run() {
	for {
		select {
		case c := <-h.register:
			h.clients[c] = true
			log.Printf("[WS] client connected (%d total)", len(h.clients))
		case c := <-h.unregister:
			if h.clients[c] {
				delete(h.clients, c)
				close(c.send)
				log.Printf("[WS] client disconnected (%d total)", len(h.clients))
			}
		case msg := <-h.broadcast:
			for c := range h.clients {
				select {
				case c.send <- msg:
				default: // slow client: drop it rather than block everyone
					delete(h.clients, c)
					close(c.send)
				}
			}
		}
	}
}

// publishJSON marshals an event envelope and queues it for all clients
// (non-blocking: if the broadcast buffer is full the event is dropped — live
// data, the next one supersedes it).
func (h *WSHub) publishJSON(v map[string]interface{}) {
	if h == nil {
		return
	}
	b, err := json.Marshal(v)
	if err != nil {
		log.Printf("[WS] marshal event failed: %v", err)
		return
	}
	select {
	case h.broadcast <- b:
	default:
		log.Printf("[WS] broadcast buffer full - dropped a %s event", v["type"])
	}
}

// --- event constructors (server -> phone) ---

func (h *WSHub) PublishJoinPending(factoryHex string, nodeType uint8) {
	h.publishJSON(map[string]interface{}{
		"type": "join_pending", "factory": factoryHex, "nodeType": nodeType,
	})
}

func (h *WSHub) PublishTelemetry(address, nodeType uint8, params map[string]float64, ts int64) {
	h.publishJSON(map[string]interface{}{
		"type": "telemetry", "address": address, "nodeType": nodeType,
		"params": params, "ts": ts,
	})
}

// PublishDBEvent feeds the DB monitor's journal (dev tool). Row-level, straight
// from SQLite's update hook - see dbmonitor.go.
func (h *WSHub) PublishDBEvent(ev DBEvent) {
	h.publishJSON(map[string]interface{}{
		"type": "db_event", "op": ev.Op, "table": ev.Table, "rowid": ev.RowID, "ts": ev.Ts,
	})
}

func (h *WSHub) PublishNodeStatus(address uint8, status string) {
	h.publishJSON(map[string]interface{}{
		"type": "node_status", "address": address, "status": status,
	})
}

// serveWS upgrades an authenticated request to a WebSocket and attaches it.
func (h *WSHub) serveWS(authToken string, w http.ResponseWriter, r *http.Request) {
	if r.URL.Query().Get("token") != authToken {
		w.WriteHeader(http.StatusUnauthorized)
		io.WriteString(w, "Odmowa")
		log.Printf("[WS] 401 from %s (bad/missing token)", r.RemoteAddr)
		return
	}
	conn, err := wsUpgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("[WS] upgrade failed from %s: %v", r.RemoteAddr, err)
		return
	}
	c := &wsClient{conn: conn, send: make(chan []byte, wsSendBuf)}
	h.register <- c
	go c.writePump()
	go c.readPump(h)
}

func (c *wsClient) writePump() {
	ticker := time.NewTicker(wsPingPeriod)
	defer func() {
		ticker.Stop()
		c.conn.Close()
	}()
	for {
		select {
		case msg, ok := <-c.send:
			c.conn.SetWriteDeadline(time.Now().Add(wsWriteWait))
			if !ok { // hub closed the channel
				c.conn.WriteMessage(websocket.CloseMessage, []byte{})
				return
			}
			if err := c.conn.WriteMessage(websocket.TextMessage, msg); err != nil {
				return
			}
		case <-ticker.C:
			c.conn.SetWriteDeadline(time.Now().Add(wsWriteWait))
			if err := c.conn.WriteMessage(websocket.PingMessage, nil); err != nil {
				return
			}
		}
	}
}

func (c *wsClient) readPump(h *WSHub) {
	defer func() {
		h.unregister <- c
		c.conn.Close()
	}()
	c.conn.SetReadLimit(wsReadLimit)
	c.conn.SetReadDeadline(time.Now().Add(wsPongWait))
	c.conn.SetPongHandler(func(string) error {
		c.conn.SetReadDeadline(time.Now().Add(wsPongWait))
		return nil
	})
	for {
		_, msg, err := c.conn.ReadMessage()
		if err != nil {
			return
		}
		// Phase B: dispatch client->server envelopes (cmd/approve/remove with reqId).
		log.Printf("[WS] rx from client (%d bytes) - ignored in Phase A", len(msg))
	}
}
