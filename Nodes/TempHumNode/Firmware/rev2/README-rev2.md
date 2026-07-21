# rev-2 T&H sensor node — gen2 firmware

Firmware for the custom rev-2 climate sensor: **CC1310 + SHT35 (temp/hum) + MCP3421
(battery voltage → SoC) + LFP 18500 + USB charge**. A **sensor-only** gen2 node —
`capabilities = 0` (no actions; usable in automations only as a condition *source*,
e.g. feedback for a heating rule).

Reuses the proven gen2 mechanism from `SolarControllerNode` (JOIN / `'E'` frame /
factory_id / NVS address / ACK+retry). rev-1's SHT35 read is kept; **BQ35100 and
TPL5111 are gone** (rev-2 is rechargeable LFP with voltage-based SoC).

## Files here (add to the CCS project)

| File | Role |
|---|---|
| `node_identity.{c,h}` | FCFG factory_id + NVS address; `NODE_CAPABILITIES = 0` |
| `mcp3421.{c,h}` | MCP3421 18-bit ADC over I2C → battery µV/mV |
| `battery_soc.{c,h}` | LFP resting-voltage → SoC % LUT (**calibrate!**) |
| `th_sense.{c,h}` | I2C + PERIPH_EN facade: SHT35 + MCP3421 + SoC |
| `th_sensor_task.c` | app task: periodic sense + telemetry + JOIN downlinks |

Plus, **from `SolarControllerNode/Firmware`**, reuse: `rfEchoTx.c` (RF task), the
RF settings (`smartrf_settings`), the board file with the NVS internal region
(`Board_NVSINTERNAL`, `flashBuf @ 0x1A000`), and `main_tirtos.c`.

## Adapt `rfEchoTx.c` (3 edits)

1. **JOIN type** in `buttonCallback2`: `joinMsg.type = NODE_TH_SENSOR;` (6, not
   `SOLAR_CONTROLLER`). `joinData.capabilities = NODE_CAPABILITIES;` (already there;
   it resolves to 0 via this node's `node_identity.h`).
2. **Route RX to our task**: change the two externs it posts downlinks to —
   `solarControllerQueue → thNodeQueue`, `solarControllerEventHandle → thNodeEventHandle`
   (it already posts `EVENT_RECEIVE_CMD = (1<<1)`, which our task waits on).
3. Nothing else — the `'E'` framing, factory_id, ACK/retry and `PAYLOAD_LENGTH` are
   fine (thData telemetry is ~20 B, well under the frame).

`main_tirtos.c` order: `identity_init();` → RF task init → `thSensorTaskInit();`.

## Board file / SysConfig (rev-2 pin map — VERIFY against the final layout)

Schematic (2026-07-06): `SCL=DIO1, SDA=DIO2, UART_TX=DIO3, UART_RX=DIO4,
PERIPH_EN=DIO5, JOIN=DIO6, nCHRGSTAT=DIO7; JTAG TDO=DIO15, TDI=DIO16`.

- `Board_I2C0` → SDA/SCL (SHT35 `0x45`, MCP3421 `0x68`), 100 kHz.
- `Board_PERIPH_EN` (out) → DIO5 — gates the SHT35 rail (TPS22860) **and** the ADC
  divider MOSFET. Add to the GPIO table.
- `Board_nCHRGSTAT` (in, pull-up) → DIO7 — MCP73123 STAT (low = charging).
- `Board_GPTIMER2A` → the measure timer.
- `Board_NVSINTERNAL` → internal-flash region (already in the solar board file).
- JOIN button → DIO6 (rfEchoTx `buttonCallback2`; wake-from-standby later).

Bench-testing on a LaunchXL without the PERIPH_EN rail? Build with
`-DTH_NO_PERIPH_EN` (the gating calls become no-ops).

## Telemetry mapping (`thData`, type 6 — gateway already decodes this)

| field | rev-2 meaning |
|---|---|
| `temperature` | SHT35 °C |
| `humidity` | SHT35 %RH |
| `batt_mv` | LFP mV (MCP3421 × 1:2 divider × cal) |
| `soh_pct` | **repurposed as SoC %** (voltage LUT) |
| `acc_uah` | unused (0) |

Wire layout is unchanged, so the gateway decoder / DB / app need no changes. (A
dedicated `soc`/charging field can be added later if we want them distinct.)

## Bring-up plan (tomorrow)

1. Flash. On boot, unprovisioned → **silent** (no telemetry), only measuring.
   Check the UART log: `[TH] T=.. RH=..` each 60 s, `[TH] batt .. mV, SoC ..%`.
2. **Verify battery reading** against a multimeter on the cell. Adjust `MCP3421_CAL`
   (1-point cal) in `mcp3421.h` until `batt_mv` matches. Confirm the divider ratio.
3. **JOIN**: press the button → gateway shows a pending join (type 6, caps 0) → the
   app (or `approvejoin`) assigns an address → node adopts it (NVS), starts reporting.
   Confirm `node_param` gets temperature/humidity/batt_mv/soh_pct for the new node.
4. In the app, add an automation with a **condition** on this node (e.g.
   temperature < X) — it must appear as a condition source but **not** as an action
   target (caps 0).
5. **Calibrate the SoC LUT** (`battery_soc.c`): the table is a generic LFP curve.
   Replace with the real cell's resting points (coulomb-counted, not OCV-swept).

## Follow-ups (after telemetry is verified)

- Deep **standby**: RTC-wake period instead of a 60 s GPTimer awake loop; gate
  Display/UART; read battery pre-RF only. (Structure is already event-driven.)
- `TH_BATT_EVERY` is 5 for test convenience → raise for production (SoC moves slowly).
- Charging status (`th_sense_charging()`) is read but not yet sent — add a field if
  the app should show "charging".
