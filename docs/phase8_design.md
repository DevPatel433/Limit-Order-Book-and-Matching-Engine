# Phase 8 — write-ahead log and crash recovery

Phase 7 built a concurrent pipeline whose state lives only in memory. Phase 8 makes it **recoverable**: every
command is recorded in a checksummed write-ahead log (WAL) *before* it can change a book, and the books are rebuilt
after a crash by replaying the log. As before, the matching core is untouched; the WAL is a layer around it.
On-disk layout: [wal_format.md](wal_format.md). Measurements: [results_phase8.md](results_phase8.md).

## Why log the input, not the state
The engine is a deterministic function of its command sequence. Phases 2-7 test exactly that (same input, same output;
threaded output equals sequential output). So the ordered commands are a **complete and compact description of every
book**: about 40 bytes per command, versus serialising order queues, levels and ID indexes. Recovery is then "replay the log
through the engine we already trust", with no second code path that could disagree with live matching. The price:
recovery time is proportional to the log length (no checkpoints yet), and the log must be exactly the input the engine saw.

## Write-ahead, and where it sits
```
 input -> sequencer --journal(cmd)--> [WAL append] --then--> assign seq, dispatch to shard queue --> workers
```
* `DurableMultiEngine::apply` appends to the log, **then** applies. If the append throws, the command was not applied.
  The books are therefore never ahead of the log. They may be *behind* it (a crash between append and apply), which is
  harmless, because recovery replays that command.
* In the Phase 7 pipeline the same rule is a `journal` hook called on the **sequencer thread** for every non-SNAPSHOT
  command, in input order, before the command gets a sequence number or reaches a worker. One writer thread means the log's
  order *is* the pipeline's order, so a threaded run and a sequential run write **byte-identical logs** (tested).
* `SNAPSHOT` is a view, not a state change, so it is never logged.
* Invalid orders, duplicate IDs and unknown cancels **are** logged: replay reproduces their rejections, and the log stays a
  faithful record of what was submitted.

## Decisions and trade-offs
* **Per-record CRC-32, fixed-size records, no length field.** A length field is itself corruptible; here the kind byte fixes
  the size (symbol names carry a 1-16 byte length that is range-checked). Every single-bit error is caught by CRC-32.
* **Torn tail vs corruption is the core design question.** A crash mid-append leaves a truncated or garbled *last* record;
  that is normal and recoverable. Damage with valid-looking data after it is not normal, and **dropping everything after it
  silently would lose acknowledged commands**. So: incomplete or checksum-failed *final* record = `TornTail` (discard,
  proceed); anything earlier, or a checksum-valid record that breaks an invariant (sequence gap or repeat, undefined or
  non-dense symbol, bad enum) = `Corrupt` (report, never auto-repair). The unavoidable ambiguity is stated rather than hidden:
  damage within the last ~44 bytes looks exactly like a torn write and is treated as one.
* **Sequence numbers are checked on read.** They catch what a CRC cannot: a record that is intact but duplicated, dropped or
  reordered.
* **Log-local symbol ids, symbols by name.** A log is self-describing: it does not depend on the run's `SymbolTable`.
* **Failure poisons the writer.** After any I/O error every later call throws and touches nothing. Otherwise a half-written
  record could be *followed by more data*, turning a recoverable torn tail into mid-log corruption.
* **`create` never clobbers; `open_append` never repairs corruption.** It truncates a torn tail (the crash case) and refuses
  everything else, leaving the file byte-for-byte unchanged (tested).
* **The durability knob is explicit** (`sync_every`): 0, N, or 1 `fsync` per record. The cost is measured, not asserted
  ([results_phase8.md](results_phase8.md)); the guarantee for each setting is in [wal_format.md](wal_format.md).

## Verification
| Layer | What it establishes |
|---|---|
| **Truncate at every byte offset** of a real log (about 2,300 cuts) | recovery always yields **exactly the state after some prefix** of the commands; the recovered count never goes backwards as more bytes survive; the valid prefix on its own is a clean log |
| **Flip a bit at every byte position** (two bit positions each) | no flip goes undetected; any flip more than 44 bytes before the end is reported `Corrupt`, never silently treated as a torn tail; commands before the damage are always recovered, and the state equals that prefix's |
| 12 hand-built invalid logs (valid CRC, broken invariant) | each is `Corrupt`, even at the very end of the file |
| Seeded mini-fuzzer (4,000 mutated, truncated, spliced and garbage logs), also under ASan/UBSan | never crashes, never reads out of bounds, scan results always self-consistent; all four outcomes occur |
| Recovered books and statistics equal the uninterrupted run, both engines | replay is faithful, including IOC/FOK/market/amend |
| Resume tests: a clean log, a torn tail, a torn header | stop, recover and continue produces a file **byte-identical** to never stopping; corrupt and foreign files are refused and left untouched |
| `/dev/full` test | if the log write fails, the command was **not** applied, and the writer then refuses further writes without touching its state |
| Journal tests; sequential vs threaded logs | hook runs on the sequencer in input order, skips SNAPSHOT, a throw submits nothing; logs are byte-identical; recovered books equal the pipeline's |
| CLI test | `lob_mreplay --wal` then `lob_recover` reproduces the run's final books; exit codes; a log cut with `head -c` recovers (exit 3), `--truncate` repairs it, then it is clean |
| ThreadSanitizer | journal-on-sequencer with workers running: no races |
| **Mutation tests**, 12 injected bugs | all 12 killed (table in [milestones.md](milestones.md)); one only after the test was strengthened |

**Not verified:** behaviour under a real power loss or a real failing disk (only simulated truncation, corruption and
`/dev/full`); filesystem-specific `fsync` semantics; a crash *during* `open_append`'s truncation; concurrent writers (the
design has exactly one).

## What this phase does not do
* **No checkpoints or log rotation.** Recovery replays from the beginning, so recovery time grows with the log
  ([results_phase8.md](results_phase8.md) measures the rate). The natural fix is a periodic snapshot of resting orders (per-order,
  in queue order, since price-level aggregates cannot rebuild priority) plus truncating the log behind it.
* **No directory `fsync`** after creating a log, so a crash right after creation may lose the file's name on some filesystems.
* **Not replication.** One process, one file. No second copy, no failover.
* **Events are not logged**, only inputs. Output already delivered to downstream consumers before a crash is regenerated by
  replay (deterministically identical), so exactly-once delivery downstream would need its own sequence-based dedup.
* A crash between "append" and "apply" is benign, but a failure *inside* apply after a successful append (e.g. out of memory)
  leaves the log ahead of the live books until the next recovery.

## Next steps, in order of value
1. Checkpoint plus truncation, to bound recovery time.
2. Group commit: batch records from the sequencer and `fsync` once per batch while still acknowledging only after the sync
   (the measured `fsync` cost is the dominant term).
3. Profile the roughly 200 ns per-command logging overhead, then try the likely suspects: a faster CRC (slice-by-8 or hardware CRC32C), one `write` per batch instead of one `fwrite` per record, an id cache instead of a string-keyed map lookup. None is done.
4. The Phase 7 follow-ups still open: streaming output and load-aware sharding.
