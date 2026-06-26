# cc1310-th-hw-firmware — bateryjny node temp/wilgotność (REALNY HW)

Mirror istotnych źródeł firmware **prawdziwego, bateryjnego noda T&H** (prototyp PCB),
budowanego POZA repo w CCS:

```
C:\Users\damia\workspace_v12\TemperatureHumidityNode_CC1310_tirtos_ccs
```

To NIE jest symulowany node z `cc1310-node-th-firmware/` (tamten generuje losowe
wartości na LAUNCHXL). Tu jest fizyczny układ:
- **CC1310** + **SHT35** (temp/wilgotność, I2C) + **BQ35100** (fuel gauge, I2C),
- zasilanie bateryjne **CR123A** (Li-MnO2, ~1400–1550 mAh),
- power-cycling timerem **TPL5111** (DRVn → TPS22860 load-switch + TPS61291 boost/bypass).

## Architektura zasilania (kluczowe)
Timer rządzi snem; MCU tylko sygnalizuje koniec pracy:
- TPL5111 co interwał (rezystor DELAY, ~3 min) załącza zasilanie → CC1310 **zimny boot**.
- CC1310: pomiar → wyślij → impuls **DONE (DIO23)** → TPL5111 odcina zasilanie (DRVn LOW).
- TPL5111 **VDD/EN/ONE_SHOT/DELAY na always-on (bateria)**; przez bramkę mocy idzie tylko DRVn.
- BQ35100 **VDD na always-on** (liczy zawsze), **GE na VCC_SWITCH** (gauge aktywny tylko w oknie pracy MCU).

## Firmware = liniowy: boot → pomiar → wyślij → DONE
Radio jest **przepięte z EasyLink (oryginał rfWsnNode) na nasz raw-RF + PHY z gen2-node**,
więc gada z bramką gen2 (ramka `[0x00]['D'][src][MessageStruct][seq][crc8]`, `smartrf_settings`).
Adres na razie **stały 0x1A** (=26), provisioning później.

Pliki (hand-written; reszta = SDK/board/RFQueue/smartrf z projektu CCS, nie mirrorowane):
- `rfWsnNode.c` — main + task: boot → SHT35 + BQ35100 → `MessageStruct` type 6 → `radio_send` → DONE. Tryb `NODE_MODE_DONE` (1=power-cycle, 0=bench-loop z JTAG do debugu).
- `radio.c/.h` — jednorazowa blokująca wysyłka (RF_runCmd TX→RX(ACK)+retry), kompatybilna z bramką.
- `sensors.c/.h` — SHT35 + BQ35100. Sekwencja BQ wg drivera u-blox (patrz niżej).
- `node_protocol.h` (= repo `shared/`) — `MessageStruct`, `thData{temperature,humidity,batt_mv}`.

## ⚠️ Lekcje z bring-upu (oszczędź sobie godzin)
- **Power-cycle testuj TYLKO na baterii, bez JTAG.** Debug-probe **zasila płytkę od tyłu** (przez diody IO) → szyna trzyma 3,3V, switch „nie gasi", DRVn „zawsze high" — wszystko zmyłka. Logikę debuguj w `NODE_MODE_DONE 0` (bench-loop), power-cycle na baterii.
- **TPL5111 DONE = krótki IMPULS (idle LOW)**, nie trzymanie HIGH (datasheet). DIO23, jawny zegar GPIO (`GPIO_init()` + PRCM).
- **BQ35100 jest sekwencyjnie wybredny** (driver u-blox): GE high + settle → **GAUGE_START przez MAC `0x3E` ← `0x0011`** → **poll `CONTROL_STATUS` (0x00) bit0 (GA)=1** → dopiero `Voltage()` (`0x08`, 2B, LSB-first). Czytanie „na zimno" daje `0xBEBE`; zły rejestr GAUGE_START (Control 0x00) → NAK. Bo GE pada przy śnie → **GAUGE_START robimy co wybudzenie** (AccumulatedCapacity i tak persystuje na always-on VDD).
- Zasilanie zaprojektowane dobrze — winowajcą braku power-cyklu był JTAG + DONE-hold (nie schemat).

## Bramka — test stałego adresu 0x1A
Bramka ignoruje nieznane nody (provisioning gate), więc na czas testu bez provisioningu wstaw wiersz:
```
sqlite3 /var/lib/bramka/bramka.db "INSERT OR REPLACE INTO node(node_id,node_type,name,factory_id,status) VALUES(26,6,'TH-proto','aabbccddeeff0011','active');"
```
Sukces: `[Serve] telemetry node 26 type 6: 3 param(s) stored`; w DB `temperature/humidity/batt_mv`.

## TODO
- Coulomb/EOS: AccumulatedCapacity `0x02` (4B µAh) + GAUGE_STOP/sesja; remaining vs ~1400-1550 mAh.
- Provisioning przy power-cyclingu (button → dłuższe okno na JOIN).
- Produkcja: wyciąć `System_printf`/UART, zmierzyć pobór w oknie aktywnym, zminimalizować czas GA-poll.
