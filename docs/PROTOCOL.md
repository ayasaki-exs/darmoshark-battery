# Darmoshark power protocol (the battery slice)

This is the only part of the vendor protocol the module needs: the power query
and the two response layouts. The full reverse-engineering lives in OpenLogi's
Darmoshark branch (`crates/openlogi-device/src/darmoshark.rs` and
`docs/DARMOSHARK.md`); `darmoshark_protocol.h` mirrors that codec and
`tests/protocol_test.c` pins the same wire facts.

## Transport

- Configuration collection: usage page `0xff0a`, usage `0x0001`.
- Non-numbered 64-byte input/output report pair.
- Output endpoint `0x04`, input endpoint `0x84` on the 1K NRF receiver
  (`1915:0723`). The wired mouse (`1915:073c`) exposes the same collection.

## Frame layout

Every command is a 64-byte output report:

| Byte | Meaning |
|---|---|
| `0` | Operation type (`0..63`); bit 6 (`0x40`) = 2.4 GHz work mode |
| `1` | Packet index / sub-index |
| `2` | Command opcode |
| `3` | Sub-command |
| `4..62` | Payload |
| `63` | Checksum: the whole report sums to `0xa1` mod 256 |

The checksum byte is `0xa1 - (sum(bytes[0..62]) & 0xff)`.

`0x40` is set when the command goes through the 2.4 GHz receiver; the wired
device expects bit 6 clear.

## Power query

`cmd_power = [1, 0, 129, 1]` followed by the checksum:

- type `0x01` (`GET_POWER`)
- sub `0x01`
- opcode `129` (`BASE_INFO`)
- payload zeroed
- checksum byte `30`

With the 2.4 GHz bit the first byte is `0x41`.

The response is a 64-byte input report. Match it with
`(resp[0] & 0x3f) == 0x01 && resp[3] == 0x01` — the device echoes the
work-mode bit, so mask it.

The handshake response already carries the power fields; the module sends the
same command for each poll.

## Response layouts

The device family has two layouts. Detect `dms_v2` from the response itself:

```text
dms_v2 = (resp[6] == 0x15 && resp[7] == 0x19)
```

| Field | `dms` | `dms_v2` |
|---|---|---|
| charging state | `resp[5]` | `resp[10]` |
| battery percentage | `resp[6]` | `resp[11]` |
| active profile | `resp[7]` | `resp[12]` |
| max report-rate index | `resp[10]` | `resp[5]` |

Only the percentage is used. A value above 100 is not a reading; the module
keeps its last known value. The charging-state byte is not mapped yet, so the
power supply reports `POWER_SUPPLY_STATUS_UNKNOWN` rather than guessing.

## What the module does with it

1. Every `poll_ms` (default 60 s; `retry_ms` until the first successful read),
   send the power query and wait up to 1.5 s for a matching input report.
2. Parse the percentage (valid values 0–100).
3. On the first successful read, register a `power_supply` named
   `darmoshark_battery_N` with `type=Battery`, `scope=Device`,
   `model_name=Darmoshark M3S Pro ...`, and `capacity=<percentage>`.
4. Call `power_supply_changed()` when the percentage changes, so UPower emits a
   property update. Charging state stays `unknown`.
