# android-app — mirror apki sterującej (gen2)

Lustro **istotnych plików** aplikacji Android `GatewayCommunicatorGen2`. Tak jak
`cc1310-*firmware/` są kopią źródeł budowanych w CCS, tak ten katalog jest kopią
projektu Android Studio budowanego POZA repo:

```
C:\Users\damia\AndroidStudioProjects\GatewayCommunicatorGen2
```

To jest miejsce prawdy do **budowania** apki; tutaj trzymamy wersjonowane źródła
do przeglądu/historii. Po zmianach w Android Studio kopiujemy istotne pliki tutaj
(jak przy firmware).

## Co to za apka
Telefon ↔ bramka gen2 (AM62). `applicationId` / `namespace` =
`com.example.gatewaycommunicatorgen2` (w pełni oddzielone od gen1
`com.example.gatewaycommunicator`, żeby launcher ich nie mylił — patrz historia).
Transport: HTTPS **:9443** (POST komend) + **WebSocket `/ws`** (live), oba na tym
samym **pinningu cert + tokenie**.

## Kluczowe pliki
- `NetworkClient.java` — HTTP komendy (PUMP_ON/OFF, get/setrules, listjoins,
  approvejoin, listnodes, removenode) + wybór LAN/WAN + `baseWsUrl()`.
- `CertPin.java` — pinning leaf-certa po SHA-256 (factory + trust manager dla OkHttp).
- `GatewayWs.java` — singleton klient **WebSocket** (OkHttp + pinning), reconnect,
  parsuje zdarzenia `join_pending` / `node_status` / `telemetry`.
- `GatewayApp.java` — łączy/rozłącza WS wg foreground/background apki.
- `DevicesActivity.java` — ekran „Zarządzaj urządzeniami": lista nodów (live),
  provisioning event-driven (JOIN → okienko само), usuwanie (tap → dialog).
- `AutomationRules*` / `RulesAdapter` — CRUD reguł automatyzacji.
- `MainActivity.java` — ekran główny (pompa, reguły, urządzenia).

## Czego TU NIE MA (świadomie)
- `build/`, `.gradle/`, `.idea/`, `local.properties` (ścieżka SDK lokalna),
  Gradle wrapper (`gradlew*`, `gradle/wrapper/`).
- **`app/src/main/res/raw/cert.pem`** — cert pinowany przez apkę; dostarczany
  out-of-band (ten sam co `/etc/bramka/tls/cert.pem` na bramce). Zgodnie z
  `.gitignore` (`*.pem`) trzymany poza repo.
- `test/` / `androidTest/` (boilerplate).

## Build z tego mirrora (gdyby trzeba odtworzyć)
Skopiować do świeżego projektu AS, dodać Gradle wrapper, `local.properties` z
`sdk.dir`, oraz `app/src/main/res/raw/cert.pem`. Normalnie budujemy z katalogu
off-repo powyżej.

Kontrakt HTTP/WS po stronie bramki: `go-services/rpmsg-service/` (`httpapi.go`,
`wshub.go`, `nodecmd.go`).
