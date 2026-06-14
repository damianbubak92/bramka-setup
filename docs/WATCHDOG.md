# Watchdog Architecture

Bramka ma wielowarstwowy stack watchdog dla maksymalnej niezawodności
24/7 production. Każda warstwa łapie inny typ awarii.

## Stack overview

| Warstwa | Mechanizm | Wykrywa | Reakcja | Status |
|---|---|---|---|---|
| **A** | systemd Type=notify + WatchdogSec | Go service crash/hang | Auto-restart procesu (~2-3s) | ✅ |
| **B** | M4F RTI peripheral | M4F infinite loop/fault | Reset M4F core, remoteproc reload | ⏸️ |
| **C** | M4F→DMSC reset | Linux kernel hang (no kicks) | Reset main domain | ⏳ |
| **D** | Linux RTI HW watchdog | Total system freeze | HW reset całego SoC (60s timeout) | ✅ |

## Warstwa A: systemd watchdog (Go service)

### Mechanizm

Go service używa systemd protocol `sd_notify` przez `$NOTIFY_SOCKET`.

- Wysyła `READY=1` po starcie
- Wysyła `WATCHDOG=1` co `WATCHDOG_USEC/2` (5s w naszym przypadku)
- Wysyła `STOPPING=1` przed shutdown

systemd unit:
- `Type=notify` - czeka na `READY=1`
- `WatchdogSec=10s` - kill po brak 2 kicks
- `Restart=always` - auto-restart po crash/timeout
- `RestartSec=2s` - opóźnienie między restartami

### Detection

| Awaria | Detection method | Time to detect |
|---|---|---|
| `kill -9` | Process exit signal | ~natychmiast |
| segfault/panic | Process exit | ~natychmiast |
| Hang/deadlock | Brak `WATCHDOG=1` przez 10s | 10s |

### Recovery

systemd SIGABRT (dla watchdog timeout) lub kontekst exit (dla crash),
potem `Restart=always` triggers nowy start.

End-to-end: ~2-3s (RestartSec=2 + bootup time).

### Implementacja

- `systemd_notify.go` - minimalna sd_notify implementation
- `watchdogKicker()` goroutine w main.go - klepie co 5s
- `debugDisableWatchdog` flag dla testing (test mode `hang`)

## Warstwa D: Linux HW watchdog (/dev/watchdog)

### Mechanizm

Kernel driver `ti,j7-rti-wdt` exposes `/dev/watchdog0` jako character device.
Hardware peripheral counts down. systemd (PID 1) pisze do device co 30s,
reset countdown. Jeśli przestanie - po 60s **sprzętowy reset całego SoC**.

### Konfiguracja

W `/etc/systemd/system.conf`:
RuntimeWatchdogSec=30
Bez tego, `/dev/watchdog` jest aktywny po boot (kernel auto-start)
ale **nikt nie klepie** → bramka resetuje się co 60s!

### Detection

| Awaria | Detection method | Time to detect |
|---|---|---|
| Kernel panic | systemd nie żyje, brak kick | ~60s |
| Total freeze | Brak jakichkolwiek kick | ~60s |
| systemd crash (PID 1) | Brak kick | ~60s |

### Recovery

**Hardware reset całego SoC**. U-Boot ładuje kernel od nowa,
systemd startuje, services wstają, M4F firmware loaded.

End-to-end: ~60-90s (60s timeout + 30s boot).

### Test verification

```bash
# Sprawdź czy systemd trzyma watchdog:
lsof /dev/watchdog0
# Powinno pokazać: systemd PID 1

# Symulacja kernel panic (UWAGA: resetuje bramkę!):
echo c > /proc/sysrq-trigger
# Czekaj ~60s, bramka się zrestartuje automatycznie
```

## Lessons learned

### `nowayout` = nie ma odwrotu

AM62 watchdog ma flag `nowayout` w kernel - po starcie nie można
zatrzymać. Plus device jest tworzony automatycznie przy boot
(`/dev/watchdog0`). Czyli:

1. Bramka boot up
2. Watchdog hardware countdown rozpoczyna (60s)
3. **Jeśli nikt nie klepie → reset za 60s**

To powodowało nasze losowe reboot'y które wcześniej obwinialiśmy
o "niestabilność" - to był po prostu **właściwie działający
watchdog bez kicker'a**.

### systemd ma wbudowany kicker

Wystarczy `RuntimeWatchdogSec=N` w `/etc/systemd/system.conf`.
PID 1 sam klepie /dev/watchdog0. **Zero kodu** do napisania.

Bonus: jeśli PID 1 (systemd) padnie, watchdog wybucha → reset SoC.
Ostatnia linia obrony.

### Layered defense - dlaczego nie jedna warstwa

Każda warstwa wykrywa **inną klasę** awarii:

- Go service hang (deadlock, infinite loop) - **A wykrywa**, D nie
  (bo Linux żyje, klepie watchdog, ale aplikacja zawiesiła się)
- Linux kernel panic - **D wykrywa**, A nie (bo systemd też nie żyje)
- M4F infinite loop - **B wykrywa** (kiedy zaimplementujemy), A częściowo
  (Go widzi że M4F nie odpowiada, ale to nie powoduje crash Go)

Warstwy są ortogonalne - razem dają ~95% pokrycia.