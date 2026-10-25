# Replay file format

Plain text, one command per line, processed top to bottom against a **fresh, empty** engine.
Same file => byte-identical output on any platform, on either engine implementation.

```
# comment (also allowed after a command)
NEW <id> <BUY|SELL> <price|MKT> <quantity> [GTC|IOC|FOK]
CANCEL <id>
AMEND <id> <price> <quantity>
SNAPSHOT [depth]
```

| Field | Type | Notes |
|---|---|---|
| `id` | unsigned 64-bit | Unique among live orders (see matching_rules.md) |
| side | `BUY`/`SELL` (or `B`/`S`), case-insensitive | |
| `price` | signed 64-bit integer **ticks**, or `MKT` | Must be 1 ... 4611686018427387903 to be accepted, but out-of-range values *parse* and are rejected by the engine, so rejection behaviour is replayable |
| `quantity` | unsigned 64-bit | `0` parses and is rejected by the engine |
| TIF | `GTC`, `IOC`, `FOK` | optional; defaults to `GTC` for limit orders and `IOC` for `MKT`. `MKT ... GTC` parses and is rejected by the engine |
| `depth` | unsigned integer | Levels per side to print; omitted => `--depth` (default 5) |

`AMEND` changes a resting order's price and **open** quantity (see matching_rules.md for when time
priority is kept). Keywords are case-insensitive. Blank lines, `#` comments, extra spaces/tabs and CRLF
are ignored.

**Parsing is strict.** Unknown keywords, wrong argument counts, non-numeric text, `+`/`-` where not
valid, decimals, values that overflow their type, unknown TIF words, and trailing junk are errors reported
as `file:line: message` (exit code 1). Nothing is silently skipped.

## Text output
Written to stdout, deterministic:

```
ACCEPTED id=4 Buy 101 x 6                  # plain GTC limit: short form
ACCEPTED id=8 Buy MKT x 10 IOC             # market orders print MKT; non-GTC orders append the TIF
TRADE maker=2 taker=4 Buy 100 x 4          # price is always the maker's
EXPIRED id=8 remaining=7                   # unfilled IOC/market remainder, or a killed FOK
CANCELLED id=3 remaining=10
AMENDED id=4 90 x 6 -> 90 x 3 priority=kept
REJECTED id=3 UnknownOrder
BOOK depth=3                                # from SNAPSHOT and (unless --no-final) at the end
  ASK 101 x 5 (1)                           # price x total quantity (order count), worst ask first
  ASK 100 x 4 (1)
  ----
  BID 99 x 10 (1)                           # best bid first
END
SUMMARY commands=9 accepted=4 rejected=2 trades=2 cancelled=1 traded_qty=6 resting=1 expired=3 amended=3
```
`commands` counts SNAPSHOT lines too. `expired=` and `amended=` appear only when non-zero, so outputs for
files that do not use those features are unchanged. In a `--check` violation message, "command N" is
the 1-based index among commands (comments/blank lines excluded).

## CSV output (`--csv`)
`lob_replay file --csv > events.csv` writes a pure event log: a header line and one row per event,
with no snapshots and no SUMMARY (the summary goes to stderr). Eleven columns:

`seq,cmd,event,id,side,price,quantity,maker_id,taker_id,remaining,detail`

| event | filled columns |
|---|---|
| ACCEPTED | id, side, price (or `MKT`), quantity, detail = TIF |
| TRADE | id = taker id, side = taker side, price, quantity, maker_id, taker_id |
| CANCELLED / EXPIRED | id, remaining |
| AMENDED | id, price and quantity = the NEW values, detail = `oldprice:oldqty:kept|lost` |
| REJECTED | id, detail = reason |

`seq` numbers events from 1; `cmd` is the 1-based index of the command that caused the event.

## Multi-instrument files (`lob_mreplay`, Phase 7)
Prefix any command above with `@SYMBOL`:

```
@AAPL NEW 1 BUY 100 5
@MSFT NEW 1 SELL 50 10          # order ids are per symbol: this is unrelated to AAPL's order 1
@AAPL CANCEL 1                  # a cancel/amend applies to the symbol it names
@MSFT SNAPSHOT 3
```
Symbols are 1-16 characters from `A-Z a-z 0-9 _ . -`, case-sensitive, at most 65,536 distinct per file. A line
**without** a prefix is a parse error unless `--default-symbol S` is given. The rest of the line is parsed by the
same code as a single-book file, so every rule above (strictness, comments, CRLF, TIF) applies unchanged.
Symbols are numbered by first appearance; final books and summaries are printed in that order.

Output mirrors the input syntax: every event line is prefixed `@SYMBOL `, snapshots print as `@SYMBOL BOOK ...`, and the
end of the output has one `SUMMARY @SYMBOL ...` line per symbol plus `SUMMARY symbols=N ...` for the total
(see `data/sample/multi.expected`). With `--csv` the log gets a leading `symbol` column
(`symbol,seq,cmd,event,...`); `seq` numbers events globally and `cmd` is the 1-based command index.

```bash
./build/lob_mreplay data/sample/multi.txt --check               # sequential, independent checker per symbol
./build/lob_mreplay big.txt --threads 4                         # sharded across 4 worker threads
./build/lob_mreplay big.txt --threads 4 --verify                # ...and prove it equals the sequential run (exit 2 if not)
./build/lob_mgen --seed 7 --count 100000 --symbols 16 --hot-pct 50 > big.txt
```
`--threads N` output is byte-identical to sequential output, **except** that `SNAPSHOT` commands are skipped (reported on
stderr; final books are still printed). `--check` needs sequential mode; `--verify` needs `--threads`. Exit codes as above.

## Tools
```bash
./build/lob_replay data/sample/basic.txt                  # events + book
./build/lob_replay data/sample/orders.txt --check         # verify every step
./build/lob_replay data/sample/orders.txt --csv > events.csv
./build/lob_replay data/sample/basic.txt --engine pooled  # same output, arena allocator
./build/lob_gen --seed 42 --count 100000 --band 5 > /tmp/w.txt
./build/lob_gen --seed 42 --count 100000 --ioc-pct 15 --fok-pct 10 --market-pct 10 --amend-pct 15 > /tmp/x.txt
./build/lob_replay /tmp/x.txt --check --quiet
./build/lob_shell                                         # interactive terminal view
```
Exit codes of `lob_replay`: 0 ok, 1 usage/IO/parse error, 2 consistency violation.

`lob_gen` options: `--seed --count --mid --band --max-qty --cancel-pct --bad-pct --sweep-pct
--snapshot-every --ioc-pct --fok-pct --market-pct --amend-pct`. The last four default to 0 and draw
no random numbers when 0, so older option sets produce **byte-identical** files (a unit test pins
this). A narrow `--band` makes most orders cross. `lob_gen` uses `std::mt19937_64` with plain modulo (not
`std::*_distribution`, which differs between standard libraries), so a seed produces the same file everywhere.

## What `--check` verifies
An independent `ConsistencyChecker` rebuilds the book from the event stream alone and, after every
command, checks that: trades hit the best price first and FIFO within a price; trade price is the maker's
and respects the taker's limit; no quantity is over-executed; IOC/market remainders expire only when no
crossing liquidity remains; a FOK is either completely filled or untouched and was killed only if it
truly could not fill; amends report correct old values and the correct priority outcome; cancels and
rejections are justified; the engine's book passes its structural `validate()`; is never crossed;
has strictly sorted levels; and matches the event-sourced ledger order-for-order including queue
position. The property tests also check the global conservation law in matching_rules.md.
