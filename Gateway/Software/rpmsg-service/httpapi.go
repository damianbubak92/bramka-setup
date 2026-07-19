package main

import (
	"crypto/tls"
	"encoding/json"
	"fmt"
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
	// DBMonitor serves the dev DB monitor at /db (whole database + SQL console).
	// Must be OFF in production - see dbmonitor.go.
	DBMonitor bool
	// Restore endpoint (gw-restore.php) for the trash: listtrash + restorenode read
	// the mirror through it. Empty (serve started without -restore-url) => those two
	// commands return "not configured" and everything else is unaffected.
	RestoreURL      string
	RestoreKey      string
	RestoreInsecure bool
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
	if cfg.DBMonitor {
		registerDBMonitor(mux, store, cfg.AuthToken)
	}
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
		respondPump(p, store, true, req, w, r)
	case strings.Contains(req, "command=PUMP_OFF"):
		respondPump(p, store, false, req, w, r)

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

	case strings.Contains(req, "command=history"):
		handleHistory(store, req, w, r)

	case strings.Contains(req, "command=listjoins"):
		handleListJoins(joins, w, r)

	case strings.Contains(req, "command=approvejoin"):
		handleApproveJoin(p, store, joins, hub, req, w, r)

	case strings.Contains(req, "command=replacenode"):
		handleReplaceNode(p, store, joins, hub, req, w, r)

	case strings.Contains(req, "command=updatenode"):
		handleUpdateNode(store, req, w, r)

	case strings.Contains(req, "command=removenode"):
		handleRemoveNode(p, store, hub, req, w, r)

	case strings.Contains(req, "command=listtrash"):
		handleListTrash(store, cfg, w, r)

	case strings.Contains(req, "command=restorenode"):
		handleRestoreNode(store, cfg, req, w, r)

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
// handleHistory answers "command=history&range=day|month|year|total&node=<id>"
// with the solar chart series (see solarhistory.go). Numbers only - labels and
// units are the app's job, and it already formats for the phone's locale.
func handleHistory(store *Store, req string, w http.ResponseWriter, r *http.Request) {
	q, _ := url.ParseQuery(req)
	rng := q.Get("range")
	if rng == "" {
		rng = "day"
	}
	node := int64(atoiOr(q.Get("node"), int(solarDefaultNode)))
	count := atoiOr(q.Get("count"), 0) // 0 = all periods that have data (SolarHistory)

	series, err := store.SolarHistory(node, rng, count)
	if err != nil {
		w.WriteHeader(http.StatusBadRequest)
		io.WriteString(w, err.Error()+"\n")
		log.Printf("[HTTP] history error: %v", err)
		return
	}
	b, err := json.Marshal(series)
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		io.WriteString(w, "marshal error\n")
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Write(b)
	log.Printf("[HTTP] history node %d range %s -> %d period(s)", node, rng, len(series))
}

// solarDefaultNode is the legacy solar controller's stable node_id (= 241, preserved
// from when node_id was the address 0xF1). It is a node_id (history key), NOT an RF
// address - pump commands target the address separately in nodecmd.go. Until the
// legacy sniff node is reflashed there is exactly one solar node, so the phone need
// not pass a node id.
const solarDefaultNode int64 = 241

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

// handleReplaceNode swaps a damaged node for a fresh chip while KEEPING the old
// node's address + history. Request: "command=replacenode&factory=<newHex>&target=<dec addr>".
// The new chip must have JOINed (be pending); it takes over the target address, and
// the target row's factory_id is re-pointed to it - so history (keyed by address)
// follows automatically. Old chip is abandoned.
func handleReplaceNode(p *Protocol, store *Store, joins *joinRegistry, hub *WSHub, req string, w http.ResponseWriter, r *http.Request) {
	vals, _ := url.ParseQuery(req)
	factory := strings.ToLower(strings.TrimSpace(vals.Get("factory")))
	target := uint8(atoiOr(vals.Get("target"), -1))

	pj, ok := joins.Get(factory)
	if !ok {
		w.WriteHeader(http.StatusNotFound)
		io.WriteString(w, "unknown join (new chip must JOIN first)\n")
		return
	}
	fid, ok := factoryHexToBytes(factory)
	if !ok {
		w.WriteHeader(http.StatusBadRequest)
		io.WriteString(w, "bad factory id\n")
		return
	}
	if err := store.ReplaceNode(target, factory, time.Now().Unix()); err != nil {
		w.WriteHeader(http.StatusBadRequest)
		io.WriteString(w, err.Error()+"\n")
		log.Printf("[HTTP] replacenode: %v", err)
		return
	}
	if err := p.SendJoinAccept(fid, pj.NodeType, target); err != nil {
		w.WriteHeader(http.StatusServiceUnavailable)
		io.WriteString(w, "re-pointed, accept push failed\n")
		log.Printf("[HTTP] replacenode: addr 0x%02X re-pointed to %s but accept push failed: %v", target, factory, err)
		return
	}
	joins.Remove(factory)
	hub.PublishNodeStatus(target, "pending_join")

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{
		"factory": factory, "type": pj.NodeType, "address": target, "replaced": true,
	})
	log.Printf("[HTTP] replacenode: addr 0x%02X now chip %s (type %d), history kept, JOIN_ACCEPT sent",
		target, factory, pj.NodeType)
}

// handleRemoveNode unprovisions a node: tell it to drop its identity (CMD_REMOVE,
// it erases its stored address -> unprovisioned) and delete its registry row.
// Request: "command=removenode&address=<dec>". Best-effort to the node (if it's
// unreachable the row is still removed; monotonic allocation prevents the freed
// address from being reused under a returning stale node).
// handleUpdateNode answers "command=updatenode&address=<dec>&name=<enc>&room=<enc>".
// Labels only: no node traffic - the node knows nothing about its name or room.
// An empty room is legal and means "Bez pokoju".
func handleUpdateNode(store *Store, req string, w http.ResponseWriter, r *http.Request) {
	vals, _ := url.ParseQuery(req)
	addr64, err := strconv.ParseUint(strings.TrimSpace(vals.Get("address")), 10, 8)
	if err != nil {
		w.WriteHeader(http.StatusBadRequest)
		io.WriteString(w, "bad address\n")
		return
	}
	name := strings.TrimSpace(vals.Get("name"))
	if name == "" {
		w.WriteHeader(http.StatusBadRequest)
		io.WriteString(w, "empty name\n")
		return
	}
	room := strings.TrimSpace(vals.Get("room"))
	if err := store.UpdateNode(uint8(addr64), name, room); err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		io.WriteString(w, "DB error\n")
		log.Printf("[HTTP] updatenode 0x%02X failed: %v", uint8(addr64), err)
		return
	}
	io.WriteString(w, "OK")
	log.Printf("[HTTP] updatenode 0x%02X -> name=%q room=%q", uint8(addr64), name, room)
}

// handleRemoveNode deletes a node immediately (reactive model, no pending_remove):
// hard-delete locally (freeing the RF address at once) and, best-effort, tell an
// online node to clear its stored identity so it goes silent now. On the mirror the
// row + history are soft-deleted (kept as trash for restore within retention). An
// offline node that returns on the freed address is handled reactively once the wire
// contract lands (MSG_UNREGISTERED, §12.2); until then the best-effort notify covers
// the online case. Request: "command=removenode&address=<dec>".
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

	// Best-effort: tell an online node to erase its identity and go silent now.
	if fid, fok := factoryHexToBytes(factory); fok {
		if err := p.SendRemove(fid, nodeType, addr); err != nil {
			log.Printf("[HTTP] removenode 0x%02X: notify push failed (offline?): %v", addr, err)
		}
	}
	// Immediate hard-delete: frees the address and drops local history; the mirror
	// keeps it as trash (bq_node_d -> archive_node).
	if err := store.DeleteNode(addr); err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		io.WriteString(w, "DB error\n")
		log.Printf("[HTTP] removenode 0x%02X delete failed: %v", addr, err)
		return
	}
	hub.PublishNodeStatus(addr, "removed") // live: device drops from the list

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{"address": addr, "status": "removed"})
	log.Printf("[HTTP] removenode 0x%02X (factory %s) -> deleted (address freed, mirror archived)", addr, factory)
}

// handleListTrash returns the soft-deleted nodes (trash) from the mirror as a JSON
// array, so the app can offer restore within the retention window. Request:
// "command=listtrash". Needs the restore endpoint configured (serve -restore-url).
func handleListTrash(store *Store, cfg HTTPConfig, w http.ResponseWriter, r *http.Request) {
	if cfg.RestoreURL == "" {
		w.WriteHeader(http.StatusServiceUnavailable)
		io.WriteString(w, "restore endpoint not configured\n")
		log.Printf("[HTTP] listtrash: no -restore-url configured")
		return
	}
	list, err := store.ListArchivedNodes(cfg.RestoreURL, cfg.RestoreKey, cfg.RestoreInsecure)
	if err != nil {
		w.WriteHeader(http.StatusBadGateway)
		io.WriteString(w, "mirror unreachable\n")
		log.Printf("[HTTP] listtrash failed: %v", err)
		return
	}
	b, err := json.Marshal(list)
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		io.WriteString(w, "marshal error\n")
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Write(b)
	log.Printf("[HTTP] listtrash -> %d archived", len(list))
}

// handleRestoreNode brings one node back from the mirror trash as `detached` (keeps
// its stable id + history, awaits re-pair). Request: "command=restorenode&id=<node_id>".
// The app refreshes its device list after the call (the restored node reappears there
// awaiting a chip). Needs the restore endpoint configured (serve -restore-url).
func handleRestoreNode(store *Store, cfg HTTPConfig, req string, w http.ResponseWriter, r *http.Request) {
	if cfg.RestoreURL == "" {
		w.WriteHeader(http.StatusServiceUnavailable)
		io.WriteString(w, "restore endpoint not configured\n")
		log.Printf("[HTTP] restorenode: no -restore-url configured")
		return
	}
	vals, _ := url.ParseQuery(req)
	id, err := strconv.ParseInt(strings.TrimSpace(vals.Get("id")), 10, 64)
	if err != nil {
		w.WriteHeader(http.StatusBadRequest)
		io.WriteString(w, "bad id\n")
		return
	}
	if err := store.RestoreNodeFromMirror(cfg.RestoreURL, cfg.RestoreKey, cfg.RestoreInsecure, id); err != nil {
		w.WriteHeader(http.StatusBadGateway)
		io.WriteString(w, "restore failed\n")
		log.Printf("[HTTP] restorenode %d failed: %v", id, err)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]any{"id": id, "status": "detached"})
	log.Printf("[HTTP] restorenode %d -> detached (awaiting re-pair)", id)
}

func respondPump(p *Protocol, store *Store, on bool, req string, w http.ResponseWriter, r *http.Request) {
	label := "PUMP_OFF"
	if on {
		label = "PUMP_ON"
	}

	var err error
	target := "legacy 0xF1"
	vals, _ := url.ParseQuery(req)
	if addrStr := strings.TrimSpace(vals.Get("address")); addrStr != "" {
		// Targeted (gen2): resolve the node's factory_id so the CC1310 sends a v2 'E'
		// frame the node validates. Without it the node ignores the command.
		addr64, perr := strconv.ParseUint(addrStr, 10, 8)
		if perr != nil {
			w.WriteHeader(http.StatusBadRequest)
			io.WriteString(w, "bad address\n")
			return
		}
		addr := uint8(addr64)
		factory, _, ok, gerr := store.GetNode(addr)
		if gerr != nil || !ok {
			w.WriteHeader(http.StatusNotFound)
			io.WriteString(w, "no such node\n")
			return
		}
		var fid [8]byte
		if b, fok := factoryHexToBytes(factory); fok {
			fid = b
		}
		err = p.SendPumpTo(addr, fid, on)
		target = fmt.Sprintf("node 0x%02X", addr)
	} else {
		err = p.SendPump(on) // legacy gen1 solar (0xF1, 'D' frame)
	}

	if err != nil {
		w.WriteHeader(http.StatusServiceUnavailable)
		io.WriteString(w, "ERR\n")
		log.Printf("[HTTP] %s (%s) from %s FAILED: %v", label, target, r.RemoteAddr, err)
		return
	}
	w.Header().Set("Content-Type", "text/plain")
	io.WriteString(w, "OK\n")
	log.Printf("[HTTP] %s (%s) from %s -> node (MSG_NODE_CMD ACKed)", label, target, r.RemoteAddr)
}
