# BMS protocol reference

The protocol this firmware speaks to a pack. It is implemented in
`src/lucas.cpp`; this document is the reference for the frame layout and the
field offsets that file depends on.

## Transport

* Service `0xFFE0`, characteristic **`0xFFE1`** — written *and* notified on.
  Requests go out on it and replies come back on it.
* Subscribe to notifications on `FFE1` first; a pack that has no subscriber may
  not answer.
* MTU is typically 23, so replies arrive as a stream of **20-byte
  notifications** that must be reassembled. The length field says when a frame
  is complete.
* Packs commonly expose a second service (`0xFFF0`) carrying the unmodified
  sample characteristics of the BLE module's stock firmware. It carries no
  battery data.

## Frame format

```
  7E  SRC DST CMD SUB LEN  [payload ...]  CRC_LO CRC_HI
  0   1   2   3   4   5    6 ...          len-2  len-1
```

| Field | Notes |
|---|---|
| `7E` | start byte |
| `SRC`/`DST` | `FF` = host, `00` = BMS. Swapped in the reply: a request is `FF 00`, its reply `00 FF`. |
| `CMD`,`SUB` | two ASCII characters, e.g. `32 31` = `"21"`. Echoed in the reply, except the event log, which answers `"30"` to a `"31"` request. |
| `LEN` | **total** frame length in bytes, header and CRC included. This is what you use to reassemble the notification stream. |
| `CRC` | **CRC-16/MODBUS** (poly `0x8005` reflected = `0xA001`, init `0xFFFF`, refin/refout, no final xor) over **every byte from the `7E` up to but excluding the CRC**. Transmitted **low byte first**. |

Note the CRC covers the start byte, and that its byte order is little-endian
while most multi-byte payload fields are little-endian too — but the
temperatures are not. See below.

## Commands

| Request | Reply | Meaning |
|---|---|---|
| `7E FF 00 32 31 08 AB 88` | `7E 00 FF 32 31 ...` | **realtime status** |
| `7E FF 00 31 31 08 5B 88` | `7E 00 FF 31 31 ...` | **settings** — protection thresholds |
| `7E FF 00 33 31 08 FA 48` | `7E 00 FF 33 30 ...` | **event log** |
| `7E FF 00 41 30 0D <YY MM DD HH MM> <crc>` | `7E 00 FF 41 30 09 00 ...` | **set clock** |

Only the realtime status request is used by this firmware; the others are
documented because the frame builder in `lucas.cpp` can construct them.

## Realtime status payload (`CMD="21"`)

Offsets are from the start of the payload, i.e. frame byte 6.

| Offset | Type | Meaning |
|---|---|---|
| 2 | u8 | status flag — non-zero while current is flowing |
| 3 | u8 | status flag — non-zero while current is flowing |
| 9-10 | u16 LE | pack voltage, mV |
| 11-14 | **i32 LE** | **current, mA** — negative is discharging |
| 23-24 | u16 LE | runtime to empty, minutes (0 when idle) |
| 25 | u8 | cycle count |
| 27 | u8 | state of charge, % |
| 28-29 | u16 LE | capacity, 0.1 Ah |
| 30 | u8 | cell count |
| 31 + 3n | u16 LE, 3-byte stride | cell *n* voltage, mV |
| after the cell block | i16 **BE**, repeating | temperatures, °C |

Two things are easy to get wrong here:

* **Current is 32-bit, not 16-bit.** A 16-bit read happens to give the right
  answer at low current, then wraps at ±32.7 A. The upper two bytes are sign
  extension.
* **Temperatures are big-endian** even though every other multi-byte field in
  the payload is little-endian.

A good self-check: the cell voltages should sum to the reported pack voltage,
within a millivolt or two of rounding. `lucas::printStatus()` prints both so
the comparison is easy to eyeball.

### Fields still unidentified

Payload bytes 0-1, 4-8, 15-22, 26, and the third byte of every cell entry read
zero on an idle and on a discharging pack. Identifying them needs a pack that
is charging, balancing, or in an alarm state. The two status flags at offsets
2-3 clearly encode something about the current direction, but the individual
bits are not mapped.

## Settings payload (`CMD="11"`)

Not decoded by this firmware. The bulk of it is little-endian u16 millivolts —
pack-level and then cell-level protection thresholds, in
over-voltage / recovery / under-voltage groupings — followed by a run of signed
bytes holding temperature limits in °C. A 4-byte serial number sits near the
start.

## Event log payload (`CMD="31"`, replied to with `"30"`)

A 4-byte header, then fixed 8-byte records:

```
YY MM DD HH MM <3 bytes of event code and flags>
```

Records are ordered oldest first.
