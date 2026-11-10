# SeeZero IHD internal link protocol

Everything here was worked out from captures of one geo SeeZero IHD (board `T3ID-A-PCB-002_6`, ©2022) on an EDF Energy SMETS2 electricity + gas install, and checked against the IHD's own screens. Other firmware versions may differ.

## Physical layer

| | |
|---|---|
| Where | Header **J7**, middle pin (silkscreen `TXD`). Rear of the board, top right. |
| Direction | NXP K32W041A Zigbee radio → NXP i.MX RT1062 host (display) |
| Levels | 3.3 V logic, idles high |
| Format | UART **8N1**, **~3,573,000 baud** (one bit = 0.2798 µs, measured with the ESP32-C3's RMT peripheral at 12.5 ns resolution). Not a standard rate; 3,573,360 works. |
| Traffic | Bursty. ~15–30 bytes/s average in steady state, a few kB in a burst after the IHD powers up. |

The Trio II's Zigbee link runs at 115200 baud with NXP `01 … 03` framing. The SeeZero's is ~31× faster and uses different framing, so the Trio II tools don't work on it unchanged.

## Framing

```
AA AA | type | length (uint16 LE) | seq | payload[length] | CRC (uint16, big-endian)
```

* `AA AA` sync header. No byte stuffing: `AA AA` can appear inside payloads, so always use the length field.
* `seq` increments per message (shared across types). Type `01` messages always have `seq = 00`.
* **CRC-16/ARC** (poly 0x8005 reflected, init 0x0000, no final XOR) over `type … end of payload`, sent high byte first. Verified on every frame captured (626/626).

```python
def crc16_arc(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c

frame = bytes.fromhex("AA AA 01 01 00 00 BD 4D FC".replace(" ", ""))
assert crc16_arc(frame[2:-2]) == (frame[-2] << 8 | frame[-1])
```

## Message types

| Type | Meaning | Notes |
|---|---|---|
| `B0` | **Attribute values** | All readings, costs, history, gas CV/CF. See below. |
| `D0` | **Cluster commands** | Publish Price, Publish Tariff Information, Display Message, … |
| `01` | 1-byte value, frequent | Varies 0xB6–0xD4. Possibly link quality / RSSI. Ignored. |
| `7F` | Acknowledgement | `payload[0]` = type being acknowledged (e.g. `E0`, `A1`). |
| `A1` | Five uint32 counters | Every ~50 s, increasing. Probably radio statistics. |
| `A2`, `DA`, `02`, `E0` | Link / network housekeeping | Seen at start-up and every ~2 min. Contain device addresses; ignored. |

### `B0`: attribute values

```
00 | cluster (uint16 LE) | device | { attribute (uint16 LE) | size (uint16 LE) | value[size] (LE) } …
```

`device` is **0 = electricity, 1 = gas**. Clusters and attribute IDs are standard Zigbee Smart Energy, the same ones documented by [Geo-Trio-ii-tap](https://github.com/gaza1994/Geo-Trio-ii-tap) for the Trio II.

| Cluster / attr | Elec (dev 0) | Gas (dev 1) | Scaling | Seen |
|---|---|---|---|---|
| `0702/0000` CurrentSummationDelivered | import kWh | meter m³ | × mult ÷ div (div = 1000) | elec 10 s, gas 120 s |
| `0702/0001` CurrentSummationReceived | **export kWh** | | ÷ 1000 | 10 s |
| `0702/0400` InstantaneousDemand | live power | | **signed** int24, ×1000÷div → W; negative while exporting (confirmed) | 10 s |
| `0702/0401` / `0403` | today / yesterday kWh | | ÷ 1000 | 10 s / start-up |
| `0702/0430` / `0440` | this week / this month kWh | | ÷ 1000 | 10 s |
| `0702/0420…042C` | days 2–8 ago kWh | | ÷ 1000 | start-up |
| `0702/0432…043A`, `0442…044E` | previous weeks / months | | ÷ 1000 | start-up |
| `0702/0C01` / `0C03` | | gas today / yesterday **kWh** | ÷ 1000 | start-up (refresh interval not yet seen) |
| `0702/0C30` / `0C40` | | gas this week / month kWh | ÷ 1000 | start-up |
| `0702/0C20…0C5A` | | gas history (days/weeks/months) | ÷ 1000 | start-up |
| `0702/0301` / `0302` | multiplier / divisor | multiplier / divisor | | start-up |
| `0702/0014` | supply status (2 = on) | | | 10 s |
| `0702/0306` | device type 0x00 | device type 0x80 | | start-up |
| `0702/0307` | **site ID (MPAN)** | **site ID (MPRN)** | string | start-up. Don't publish. |
| `0705/051C` / `051E` | cost today / yesterday | same | ÷ 100,000 → £ (incl. standing charge) | elec ~15 s |
| `0705/0530` / `0540` | cost this week / month | same | ÷ 100,000 → £ | elec ~15 s |
| `0705/0520…055A` | cost history | same | ÷ 100,000 → £ | start-up |
| `0700/0301` | | standing charge | ÷ 100,000 → £/day | start-up |
| `0700/0302` + `0303` | | conversion factor (1.02264) | value ÷ 10^(high nibble of 0303) | start-up |
| `0700/0304` + `0306` | | calorific value (39.3 MJ/m³) | value ÷ 10^(high nibble of 0306) | start-up |
| `000A/0000` | Zigbee time (s since 2000-01-01 UTC) | | | start-up |

Cluster `0708` (Device Management) also appears. It carries supplier and device identifiers, so leave it out of anything you publish.

### `D0`: cluster commands

```
cluster (uint16 LE) | command | device | size (uint16 LE) | body[size]
```

The body is a fixed-layout structure (the host's own struct, not the ZCL wire format). Unused fields are `FF`.

**Price `0700`, cmd `00` Publish Price** (body 96 bytes)

| Offset | Size | Field | Example (elec) |
|---|---|---|---|
| 0–63 | | unused (`FF`), probably the rate label | |
| 64–75 | | IDs and timestamps (Zigbee time), not decoded | |
| **76** | 4 | **price** (uint32 LE) | `AA 64 00 00` = 25770 |
| 84 | 2 | currency | `3A 03` = 826 (GBP) |
| **87** | 1 | **trailing digits** | `05` → £0.25770/kWh |

**Price `0700`, cmd `04` Publish Tariff Information** (body 64 bytes)

| Offset | Size | Field | Example |
|---|---|---|---|
| **16** | 4 | **standing charge** | `EC EA 00 00` = 60140 |
| 28 | 2 | currency | `3A 03` |
| **37** | 1 | **trailing digits** | `05` → £0.60140/day |
| 39 | … | tariff label (null-terminated) | `Tariff` |

**Messaging `0703`, cmd `00` Display Message**

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | message ID |
| 4 | 1 | message control |
| 5 | 4 | start time |
| 9 | 2 | duration (`FF FF` = until changed) |
| 11 | 1 | text length |
| 12 | n | text (e.g. `Welcome to EDF Energy`) |

## Timing observed

* Electricity power, readings, today/week/month: every **10 s**.
* Electricity costs: every **~15 s**.
* Gas meter reading (m³): every **120 s** (it only changes every 30 min).
* Tariff, gas CV/CF, gas daily kWh and costs, and all history: in a burst **~15–40 s after the IHD powers up**.
* "Yesterday" values (`0403`, `0C03`, `051E`) refresh by themselves **just after midnight** (confirmed). Gas today and cost also refresh periodically during the day.

## Cross-checks against the IHD screen

| | Decoded | IHD |
|---|---|---|
| Live power | 297 W | 297 W |
| Electricity import / export | 7,864.869 / 17,526.543 kWh | same |
| Gas meter | 4,193.570 m³ | same |
| Electricity today | 4.256 kWh, £1.698 | 4.25 kWh, £1.69 |
| Gas today | 37.767 kWh, £3.589 | 37.76 kWh, £3.58 |
| Unit rates | 25.770p / 8.631p | same |
| Standing charges | 60.140p / 32.917p | same |
