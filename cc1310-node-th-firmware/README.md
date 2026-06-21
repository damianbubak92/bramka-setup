# cc1310-node-th-firmware

Maintained source of the gen2 **temperature & humidity node** (CC1310). Mirror of
the authored files only — the full CCS project (board files, `smartrf_settings`,
`RFQueue`, `ccfg`, TI boilerplate) lives off-repo in CCS Theia at
`C:\Users\damia\workspace_v12\SubGHzTempHumNodeGen2_CC1310` (NO `&`/spaces in the
name — `&` silently breaks flashing). Same workflow as `cc1310-firmware/` (gateway):
edit here, copy into the CCS project, **clean rebuild**, flash.

## Files
- `th_sensor_task.c` — app task. 60 s timer (10 s GPT × 6) → random temperature
  (18–26 °C) + humidity (30–70 %) → `MessageStruct` type `NODE_TH_SENSOR` (6),
  `cmd=SEND_DATA_TO_DB`, `length=12` → `radioQueue`. Button = instant send.
  Phase-0 stand-in for a real sensor.
- `rfEchoTx.c` — radio task (RF TX/ACK/retry, RX/ACK). `NODE_ADDRESS=0xF3`
  (Phase-0 fixed addr; provisioning will assign dynamically). Frame to gateway
  `0xF0`: `[0xF0]['D'][src][MessageStruct][seq][crc8-xor]`.
- `main_tirtos.c` — entry: `radioTaskInit()` + `thSensorTaskInit()`.

## Notes / TODO
- `MessageStruct` mirrors `shared/node_protocol.h` (type 6 + `thData`); keep
  byte-identical with the gateway.
- `smartrf_settings` PHY MUST stay identical to the gateway or it won't be heard.
- TODO: **button debounce** (a press currently emits several sends); provisioning
  JOIN/identity (factory ID → assigned address in NVS) — see the project memory.
