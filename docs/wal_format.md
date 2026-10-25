# Write-ahead log format (`LOBWAL01`)

The log records the **sequenced input** of a multi-instrument run: the commands, in the order the sequencer
accepted them. Books are rebuilt by replaying it ([phase8_design.md](phase8_design.md)). Implemented in
`include/lob/wal.hpp` / `src/wal.cpp`; the layout below is also what `tests/test_wal.cpp` builds by hand, so the
document and the code are checked against each other.

All integers are **little-endian** and fixed width, so a log written on one platform reads on another.

## File
```
offset 0   8 bytes   magic "LOBWAL01"
offset 8   records, back to back, until end of file
```

## Record
```
+---------+------+----------------------+
| crc32   | kind | body (depends on kind)|
| 4 bytes | 1    |                      |
+---------+------+----------------------+
```
`crc32` is the standard CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320; `crc32("123456789") = 0xCBF43926`)
computed over `kind` and `body`. The record's length is determined by its kind (and, for symbol definitions, a
length byte), so there is no separate length field that could itself be corrupted.

| kind | name | body | total size |
|---:|---|---|---:|
| 1 | SymbolDef | `id u32`, `len u8` (1..16), `name` (`len` bytes) | 10 + len |
| 2 | New | `seq u64`, `symbol u32`, `order_id u64`, `price i64`, `quantity u64`, `side u8` (0 Buy, 1 Sell), `tif u8` (0 GTC, 1 IOC, 2 FOK), `type u8` (0 Limit, 1 Market) | 44 |
| 3 | Cancel | `seq u64`, `symbol u32`, `order_id u64` | 25 |
| 4 | Amend | `seq u64`, `symbol u32`, `order_id u64`, `price i64`, `quantity u64` | 41 |

Measured average: about 40 bytes per command on a mixed workload (the New record dominates).

## Invariants (what makes a log valid)
1. The header is exactly `LOBWAL01`.
2. Every record's CRC matches.
3. **Symbols** are defined before use, with dense ids `0, 1, 2, ...` in order of first use; each name is valid
   (1-16 characters from `A-Z a-z 0-9 _ . -`) and defined once. Ids are **log-local**: they need not equal the ids a
   run's own symbol table used. Books are identified by name.
4. **Sequence numbers** of command records are exactly `1, 2, 3, ...` with no gap or repeat. `SNAPSHOT` is a view and
   is never logged, so these count logged commands, not input lines.
5. Enumerated fields hold defined values.
Invalid orders and unknown cancels are logged like any other command: replay reproduces their rejections.

## How a reader classifies a file (`wal_scan`)
| Status | Meaning | Recovery |
|---|---|---|
| `Clean` | every byte belongs to a valid record | all commands |
| `TornTail` | the file ends in an incomplete record, or the **final** record fails its checksum (a crash mid-write) | all commands before it; the tail is discarded (`lob_recover --truncate` cuts it off) |
| `Corrupt` | bad data followed by more data, or a record that is checksum-valid but breaks an invariant (sequence gap or repeat, undefined or non-dense symbol, bad enum) | the state just **before** the damage is available, but the log is reported as damaged and is **never auto-repaired** |
| `BadHeader` | a full 8 bytes that are not the magic (not a WAL, or another version) | nothing |

An empty file or a partial header is a `TornTail` with zero valid bytes (a crash before the header finished).

**The one ambiguity, stated plainly.** A torn write is indistinguishable from damage inside the last record. So
corruption within the final ~44 bytes (about the last record) is treated as a torn write and discarded, while
corruption anywhere earlier is detected and reported. Tests flip every bit position of a real log and assert exactly
this boundary. Records are never *silently* dropped from the middle.

## Durability knobs (`WalOptions::sync_every`)
| Value | Behaviour | Loses, on power failure |
|---|---|---|
| 0 | never `fsync`; data may be in user-space buffers | anything not yet flushed by the OS or the process |
| 1 | flush + `fsync` after every record | nothing acknowledged |
| N | flush + `fsync` every N records | up to N-1 acknowledged commands |
Example: `lob_mreplay in.txt --wal run.wal --wal-sync 1000`, then `lob_recover run.wal`.

## Not covered
No directory `fsync` after creating the file (a crash right after creation could lose the file's name on some
filesystems); no log rotation or checkpointing (recovery replays from the start); no encryption or compression.
See [limitations.md](limitations.md).
