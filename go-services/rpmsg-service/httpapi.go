package main

import (
	"crypto/tls"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"net/url"
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
func StartHTTPAPI(p *Protocol, store *Store, joins *joinRegistry, cfg HTTPConfig) error {
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		handleCommand(p, store, joins, cfg, w, r)
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
func handleCommand(p *Protocol, store *Store, joins *joinRegistry, cfg HTTPConfig, w http.ResponseWriter, r *http.Request) {
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

	case strings.Contains(req, "command=listjoins"):
		handleListJoins(joins, w, r)

	case strings.Contains(req, "command=approvejoin"):
		handleApproveJoin(p, store, joins, req, w, r)

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

// handleApproveJoin commissions a pending node: factory= identifies which JOIN
// (from listjoins), name= is the user's label. We allocate a pool address, record
// the node, and push JOIN_ACCEPT down to it. Request fields ride query+body as
// "factory=<hex>&name=<label>" (url-encoded).
func handleApproveJoin(p *Protocol, store *Store, joins *joinRegistry, req string, w http.ResponseWriter, r *http.Request) {
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

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{
		"factory": factory, "name": name, "type": pj.NodeType, "address": addr,
	})
	log.Printf("[HTTP] approvejoin: factory %s (type %d) -> addr 0x%02X name %q, JOIN_ACCEPT sent",
		factory, pj.NodeType, addr, name)
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
