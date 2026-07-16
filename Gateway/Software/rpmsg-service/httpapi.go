package main

import (
	"crypto/tls"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
)

// defaultAuthToken matches the Android app (NetworkClient.AUTH_TOKEN) and gen1
// httpsServerTask. Shared secret carried in every request (query or body).
// The real defence is the pinned TLS cert; this token mirrors the legacy app.
const defaultAuthToken = "c228cecbca32894a526092abd305cddc"

// HTTPConfig configures the phone-facing HTTPS API.
type HTTPConfig struct {
	Addr      string // e.g. ":9443"
	CertFile  string // MUST be the exact cert the app pins (res/raw/cert.pem)
	KeyFile   string // its matching private key
	AuthToken string
}

// StartHTTPAPI runs the phone-facing HTTPS API and blocks. Replicates the gen1
// contract (httpsServerTask): a POST to "/" whose request text contains
// command=<X> and authToken=<token>. The Android client pins our leaf cert by
// SHA-256, so CertFile/KeyFile MUST be the same pair the app was built against.
func StartHTTPAPI(p *Protocol, store *Store, joins *joinRegistry, hub *WSHub, cfg HTTPConfig) error {
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		handleCommand(p, store, joins, hub, cfg, w, r)
	})
	// WebSocket live channel (same TLS + token); see wshub.go.
	mux.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		hub.serveWS(cfg.AuthToken, w, r)
	})
	srv := &http.Server{
		Addr:      cfg.Addr,
		Handler:   mux,
		TLSConfig: &tls.Config{MinVersion: tls.VersionTLS12},
	}
	log.Printf("[HTTP] phone API listening on %s (TLS 1.2+, cert=%s)", cfg.Addr, cfg.CertFile)
	return srv.ListenAndServeTLS(cfg.CertFile, cfg.KeyFile)
}

// handleCommand mirrors gen1's permissive matching: the command and token can be
// in the query OR the body (the app posts form-urlencoded for pump/getrules and
// a text/plain "command=setrules&rules=..." body for setrules). We match over
// query+body uniformly instead of relying on Content-Type parsing.
func handleCommand(p *Protocol, store *Store, joins *joinRegistry, hub *WSHub, cfg HTTPConfig, w http.ResponseWriter, r *http.Request) {
	bodyBytes, _ := io.ReadAll(io.LimitReader(r.Body, 256*1024))
	body := string(bodyBytes)
	req := r.URL.RawQuery + "&" + body

	if !strings.Contains(req, "authToken="+cfg.AuthToken) {
		w.WriteHeader(http.StatusUnauthorized)
		io.WriteString(w, "Odmowa")
		log.Printf("[HTTP] 401 from %s (bad/missing token)", r.RemoteAddr)
		return
	}

	switch {
	case strings.Contains(req, "command=PUMP_ON"):
		respondPump(p, true, w, r)
	case strings.Contains(req, "command=PUMP_OFF"):
		respondPump(p, false, w, r)

	case strings.Contains(req, "command=getrules"):
		j, err := store.GetRulesJSON()
		if err != nil {
			w.WriteHeader(http.StatusInternalServerError)
			io.WriteString(w, "DB error\n")
			log.Printf("[HTTP] getrules DB error: %v", err)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		io.WriteString(w, j)
		log.Printf("[HTTP] getrules -> %d bytes", len(j))

	case strings.Contains(req, "command=setrules"):
		handleSetRules(p, store, body, w, r)

	case strings.Contains(req, "command=listnodes"):
		handleListNodes(store, w, r)

	case strings.Contains(req, "command=state"):
		handleState(store, w, r)

	case strings.Contains(req, "command=listjoins"):
		handleListJoins(joins, w, r)

	case strings.Contains(req, "command=approvejoin"):
		handleApproveJoin(p, store, joins, hub, req, w, r)

	case strings.Contains(req, "command=removenode"):
		handleRemoveNode(p, store, hub, req, w, r)

	default:
		w.WriteHeader(http.StatusNotFound)
		io.WriteString(w, "Not Found")
		log.Printf("[HTTP] 404 from %s (no known command)", r.RemoteAddr)
	}
}

// handleSetRules parses the app's "rules=<json>" field, persists it, and pushes
// the ruleset to the M4F engine. The app posts a text/plain body shaped
// "command=setrules&rules=<JSON array>&authToken=<token>" (rules= NOT url-encoded).
func handleSetRules(p *Protocol, store *Store, body string, w http.ResponseWriter, r *http.Request) {
	rulesJSON, ok := extractRulesField(body)
	if !ok {
		w.WriteHeader(http.StatusBadRequest)
		io.WriteString(w, "missing rules=\n")
		log.Printf("[HTTP] setrules: no rules= field")
		return
	}
	rules, err := parseAppRules(rulesJSON)
	if err != nil {
		w.WriteHeader(http.StatusBadRequest)
		io.WriteString(w, "bad rules JSON\n")
		log.Printf("[HTTP] setrules: parse failed: %v", err)
		return
	}
	if err := store.SetRules(rules); err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		io.WriteString(w, "DB error\n")
		log.Printf("[HTTP] setrules: persist failed: %v", err)
		return
	}
	if err := p.PushRules(rules); err != nil {
		// Persisted but not applied: report so the user retries; the next serve
		// start (or retry) re-pushes from the DB.
		w.WriteHeader(http.StatusServiceUnavailable)
		io.WriteString(w, "saved, engine push failed\n")
		log.Printf("[HTTP] setrules: saved %d rules but push failed: %v", len(rules), err)
		return
	}
	w.Header().Set("Content-Type", "text/plain")
	io.WriteString(w, "OK")
	log.Printf("[HTTP] setrules from %s: %d rules saved + pushed", r.RemoteAddr, len(rules))
}

// extractRulesField pulls the raw JSON after "rules=" up to "&authToken=" (or end).
// The app sends the JSON un-encoded, so no URL-decoding (that would corrupt it).
func extractRulesField(body string) (string, bool) {
	i := strings.Index(body, "rules=")
	if i < 0 {
		return "", false
	}
	v := body[i+len("rules="):]
	if j := strings.Index(v, "&authToken="); j >= 0 {
		v = v[:j]
	}
	return v, true
}

// handleListJoins returns the nodes that pressed JOIN and await approval, as a
// JSON array the phone renders for the user (provisioning step 4).
func handleListJoins(joins *joinRegistry, w http.ResponseWriter, r *http.Request) {
	list := joins.List()
	b, err := json.Marshal(list)
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		io.WriteString(w, "marshal error\n")
		log.Printf("[HTTP] listjoins marshal error: %v", err)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Write(b)
	log.Printf("[HTTP] listjoins -> %d pending", len(list))
}

// handleListNodes returns the provisioned nodes for the phone's device screen.
// handleState answers "command=state" with the LAST KNOWN telemetry of every
// node (node_param). The phone calls this once on open so the UI (temperatures,
// pump triangles, the aux-pump toggle) reflects reality immediately instead of
// waiting up to 2 minutes for the next WS telemetry push.
func handleState(store *Store, w http.ResponseWriter, r *http.Request) {
	st, err := store.ListState()
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		io.WriteString(w, "DB error\n")
		log.Printf("[HTTP] state DB error: %v", err)
		return
	}
	b, err := json.Marshal(st)
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		io.WriteString(w, "marshal error\n")
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Write(b)
	log.Printf("[HTTP] state -> %d node(s)", len(st))
}

func handleListNodes(store *Store, w http.ResponseWriter, r *http.Request) {
	nodes, err := store.ListNodes()
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		io.WriteString(w, "DB error\n")
		log.Printf("[HTTP] listnodes DB error: %v", err)
		return
	}
	b, err := json.Marshal(nodes)
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		io.WriteString(w, "marshal error\n")
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Write(b)
	log.Printf("[HTTP] listnodes -> %d node(s)", len(nodes))
}

// handleApproveJoin commissions a pending node: factory= identifies which JOIN
// (from listjoins), name= is the user's label. We allocate a pool address, record
// the node, and push JOIN_ACCEPT down to it. Request fields ride query+body as
// "factory=<hex>&name=<label>" (url-encoded).
func handleApproveJoin(p *Protocol, store *Store, joins *joinRegistry, hub *WSHub, req string, w http.ResponseWriter, r *http.Request) {
	vals, _ := url.ParseQuery(req)
	factory := strings.ToLower(strings.TrimSpace(vals.Get("factory")))
	name := strings.TrimSpace(vals.Get("name"))

	pj, ok := joins.Get(factory)
	if !ok {
		w.WriteHeader(http.StatusNotFound)
		io.WriteString(w, "unknown join\n")
		log.Printf("[HTTP] approvejoin: no pending join for factory %q", factory)
		return
	}
	fid, ok := factoryHexToBytes(factory)
	if !ok {
		w.WriteHeader(http.StatusBadRequest)
		io.WriteString(w, "bad factory id\n")
		log.Printf("[HTTP] approvejoin: bad factory id %q", factory)
		return
	}

	addr, err := store.ProvisionNode(pj.NodeType, name, factory, time.Now().Unix())
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		io.WriteString(w, "provision failed\n")
		log.Printf("[HTTP] approvejoin: provision factory %s failed: %v", factory, err)
		return
	}

	if err := p.SendJoinAccept(fid, pj.NodeType, addr); err != nil {
		// Provisioned in the DB but the accept didn't reach the M4F: report so the
		// user retries (the node keeps sending JOIN; re-approve reuses the address).
		w.WriteHeader(http.StatusServiceUnavailable)
		io.WriteString(w, "provisioned, accept push failed\n")
		log.Printf("[HTTP] approvejoin: factory %s got addr 0x%02X but accept push failed: %v", factory, addr, err)
		return
	}
	joins.Remove(factory)
	hub.PublishNodeStatus(addr, "pending_join") // live: new device appears (awaiting confirm)

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{
		"factory": factory, "name": name, "type": pj.NodeType, "address": addr,
	})
	log.Printf("[HTTP] approvejoin: factory %s (type %d) -> addr 0x%02X name %q, JOIN_ACCEPT sent",
		factory, pj.NodeType, addr, name)
}

// handleRemoveNode unprovisions a node: tell it to drop its identity (CMD_REMOVE,
// it erases its stored address -> unprovisioned) and delete its registry row.
// Request: "command=removenode&address=<dec>". Best-effort to the node (if it's
// unreachable the row is still removed; monotonic allocation prevents the freed
// address from being reused under a returning stale node).
func handleRemoveNode(p *Protocol, store *Store, hub *WSHub, req string, w http.ResponseWriter, r *http.Request) {
	vals, _ := url.ParseQuery(req)
	addr64, err := strconv.ParseUint(strings.TrimSpace(vals.Get("address")), 10, 8)
	if err != nil {
		w.WriteHeader(http.StatusBadRequest)
		io.WriteString(w, "bad address\n")
		return
	}
	addr := uint8(addr64)

	factory, nodeType, ok, err := store.GetNode(addr)
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		io.WriteString(w, "DB error\n")
		log.Printf("[HTTP] removenode get 0x%02X failed: %v", addr, err)
		return
	}
	if !ok {
		w.WriteHeader(http.StatusNotFound)
		io.WriteString(w, "no such node\n")
		return
	}

	// Graceful remove: mark pending_remove (KEEP the row so the address stays
	// reserved) and tell the node to clear itself. The row is deleted + address
	// freed only when the node confirms (drain: a node->gw CMD_REMOVE) - see
	// runServe. If the node is offline now, it confirms when it next speaks
	// (drain re-sends REMOVE on hearing from a pending_remove address).
	if err := store.SetPendingRemove(addr); err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		io.WriteString(w, "DB error\n")
		log.Printf("[HTTP] removenode 0x%02X set pending failed: %v", addr, err)
		return
	}
	if fid, fok := factoryHexToBytes(factory); fok {
		if err := p.SendRemove(fid, nodeType, addr); err != nil {
			log.Printf("[HTTP] removenode 0x%02X: REMOVE push failed (will retry on contact): %v", addr, err)
		}
	}
	hub.PublishNodeStatus(addr, "pending_remove") // live: device shows "removing"

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{"address": addr, "status": "pending_remove"})
	log.Printf("[HTTP] removenode 0x%02X (factory %s) -> REMOVE sent, awaiting node confirm", addr, factory)
}

func respondPump(p *Protocol, on bool, w http.ResponseWriter, r *http.Request) {
	label := "PUMP_OFF"
	if on {
		label = "PUMP_ON"
	}
	if err := p.SendPump(on); err != nil {
		w.WriteHeader(http.StatusServiceUnavailable)
		io.WriteString(w, "ERR\n")
		log.Printf("[HTTP] %s from %s FAILED: %v", label, r.RemoteAddr, err)
		return
	}
	w.Header().Set("Content-Type", "text/plain")
	io.WriteString(w, "OK\n")
	log.Printf("[HTTP] %s from %s -> node (MSG_NODE_CMD ACKed)", label, r.RemoteAddr)
}
