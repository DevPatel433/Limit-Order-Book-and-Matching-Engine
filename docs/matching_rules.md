# Matching rules

Single instrument, continuous limit-order book, price-time priority. Implemented by
`MatchingEngine` (and the behaviourally identical `PooledMatchingEngine`). This document is the
specification the tests check; every example here is executable (`data/sample/*.txt`,
`tests/regression/*.txt`).

## Inputs
| Operation | Meaning |
|---|---|
| `submit(Order)` | New order: `id`, `side`, `price` (integer ticks), `quantity`, `time in force`, `type` |
| `cancel(id)` | Remove a resting order |
| `amend(id, price, quantity)` | Change a resting order's price and/or open quantity |

**Order types.** *Limit* orders carry a price. *Market* orders have no price limit and sweep the
opposite side. **Time in force** (`GTC`, `IOC`, `FOK`) decides what happens to quantity that cannot
execute immediately.

| TIF | Executes | Unfilled remainder |
|---|---|---|
| GTC | what it can immediately | rests at its limit price, at the back of that level's queue |
| IOC | what it can immediately | discarded (`Expired`); never rests |
| FOK | all of its quantity immediately, or nothing | nothing executes; whole quantity `Expired` |

A market order must be IOC (default) or FOK: a GTC market order could never rest and is rejected.

## Validation (in this order; failure = one `Rejected` event, book untouched)
1. `quantity == 0` -> `InvalidQuantity`
2. market order with GTC -> `InvalidTimeInForce`
3. limit order with price outside `[1, 4611686018427387903]` -> `InvalidPrice` (a market order's price field is ignored)
4. ID equals a **live resting** order's ID -> `DuplicateId`
5. (GTC orders that do not cross) resting would overflow the level's `uint64` total -> `QuantityOverflow`
6. cancel/amend of an ID that is not live -> `UnknownOrder`

"Live" means resting in the book. IDs of fully filled, expired or cancelled orders may be reused, and
a taker that never rests (filled, IOC, FOK, market) never occupies an ID.

## Crossing
A buy crosses an ask when `ask_price <= buy_limit`; a sell crosses a bid when
`bid_price >= sell_limit`. Equal prices cross. A market buy's limit is "infinity", a market sell's is
"zero": everything crosses.

## Matching loop
```
emit Accepted
if FOK and fillable quantity within the limit < quantity:   emit Expired(quantity); stop   # nothing traded
while remaining > 0:
    maker = front order at the best opposing price
    if none, or maker does not cross the taker's limit: stop
    q = min(remaining, maker.quantity)
    execute q against maker; emit Trade(maker, taker, maker.price, q)
    remaining -= q
if remaining > 0:   GTC -> rest at the taker's limit, back of that price's queue
                    IOC / market -> emit Expired(remaining)
```
* **Price priority:** best opposing price first (lowest ask for a buy, highest bid for a sell).
* **Time priority:** FIFO within a price, by arrival at that price level.
* **Execution price:** always the resting (maker) order's price. A taker with a more aggressive limit
  (or a market order) gets price improvement.
* **Partial fills:** a partially filled maker keeps its queue position. A partially filled GTC taker's
  remainder goes to the *back* of its own price level.
* **Trade granularity:** one `Trade` per maker order touched.
* **FOK is atomic:** availability is computed first (`OrderBook::fillable_quantity`, O(levels scanned));
  if it is insufficient nothing executes, so a killed FOK leaves the book exactly as it was.
* **No self-trade prevention:** orders are anonymous.
* **Never over-executes:** `q <= min(remaining, maker.quantity)` by construction.

## Amend
`amend(id, new_price, new_quantity)` applies to a **resting** order and `new_quantity` is the new
**open** quantity (after any partial fills), not the original size. It cannot remove an order
(`new_quantity == 0` is `InvalidQuantity`; use cancel).

| Change | Time priority | Mechanics |
|---|---|---|
| same price, quantity not increased | **kept** | reduced in place (`Amended ... priority=kept`) |
| quantity increased, or price changed | **lost** | atomically removed and re-entered as a fresh GTC order with the same ID: `Amended ... priority=lost`, then trades if the new price crosses (the amended order is the taker), then any remainder rests at the back of the new level |

Rationale: a queue position is only worth keeping if the change cannot disadvantage anyone behind
it; adding size or moving price does. Amending to identical values is a priority-keeping no-op.
Amends are atomic: if the replacement could not be placed (level quantity overflow) the amend is
rejected and the original order is untouched (the check happens *before* removal).

## Events (in order of occurrence)
| Event | Meaning |
|---|---|
| `Accepted{id, side, price, quantity, tif, type}` | Valid order; original quantity. Always precedes its trades. Plain GTC limit orders print as before; others append the TIF; market orders print `MKT`. |
| `Trade{maker, taker, taker_side, price, quantity}` | One execution. Both sides' fills are derivable from it. |
| `Expired{id, remaining}` | Unfilled remainder of an IOC/market order, or the whole of a killed FOK. Always the last event for that order. |
| `Cancelled{id, remaining}` | Resting order removed; `remaining` is what was still open. |
| `Amended{id, old/new price and quantity, keeps_priority}` | Resting order modified. Trades may follow. |
| `Rejected{id, reason}` | Request refused, no state change. |

A fully resting GTC order produces just `Accepted`. There is no separate "rested" event: after
`Accepted`, anything not covered by `Trade`s (and not `Expired`) is resting.

## Invariants (checked by tests after every operation)
* The book is never crossed: best bid < best ask whenever both exist.
* `OrderBook::validate()` holds (levels, index, totals consistent).
* Only GTC limit orders ever rest.
* Conservation: `accepted + amend_added == 2 x traded + cancelled + expired + resting + amend_removed`
  (each traded unit leaves both a maker and a taker; amends add or remove quantity from the open order).
* An order's events never continue after `Expired`.
* Same input sequence -> identical event sequence, on both implementations.

## Verification strategy
* Example-based tests for every rule above, run on both engines (`tests/test_matching_engine.cpp`,
  `tests/test_extensions.cpp`).
* Randomized sequences compared **event-for-event** against two deliberately naive reference engines (flat
  vector, linear scans, no shared code), one for the limit-order core and one for the full semantics.
* The independent `ConsistencyChecker` rebuilds the book from events alone and checks every rule
  above after every command (including "FOK killed although fillable", "IOC expired while crossing
  liquidity remained", "amend claims the wrong priority").
* Mutation testing of that checker (documented in [phase6_design.md](phase6_design.md)): injected
  engine bugs must be detected.

## Known limitations
No post-only / stop / iceberg orders, self-trade prevention, trade busting, auctions, multiple
instruments, or concurrency. See [limitations.md](limitations.md).
