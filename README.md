# Limit Order Book Matching Engine

A multi-threaded limit order book and matching engine in C++17.

- **Price-time priority matching** for limit orders, market orders and cancels
- **Two-thread pipeline**: an ingestion thread hands requests to a matching thread through a **lock-free single-producer/single-consumer ring buffer**, so the order book needs no locks
- **37 unit tests**, clean under **ThreadSanitizer**
- No external dependencies (includes a small self-contained test framework)

## Interactive demo

[`docs/index.html`](docs/index.html) is a single-file visual walkthrough of the engine:

- **Order book**: a depth ladder showing each price level's FIFO queue, a step-by-step scenario covering price-time priority, partial fills, cancels and market orders, plus a form for sending your own orders and a random order flow
- **Pipeline**: an animated 16-slot model of the SPSC ring buffer with adjustable producer and consumer rates
- **Benchmarks**: throughput and latency charts from the results below

The page runs a JavaScript port of the `OrderBook` matching logic. To view it, open the file in a browser, or enable GitHub Pages for this repo with the `/docs` folder as the source.

## Build and run

Requires a C++17 compiler (g++ or clang++) and `make`.

```bash
make test     # build and run the 37 unit tests
make tsan     # run the same tests under ThreadSanitizer (data-race detector)
make bench    # throughput and latency benchmark
make clean
```

On macOS, use `make test CXX=clang++` (Apple's clang is installed with the Xcode Command Line Tools). ThreadSanitizer can crash at startup on recent macOS versions even for trivial programs; run `make tsan` on Linux if that happens.

The latency test in the benchmark runs at 50% of measured pipeline throughput by default. To choose another load, run the binary directly, e.g. `./build/benchmark 0.1` for 10%.

## Design

```
ingestion thread                                   matching thread
----------------                                   ---------------
MatchingEngine::submit(request)                    pop request
        |                                          apply to OrderBook (no locks)
        v                                          invoke callback with trades
[ SpscQueue<Request, 65536> ]  ------------------>
```

| File | What it does |
|---|---|
| `include/lob/Types.hpp` | `Order`, `Trade`, `Request`; prices are integer ticks, not doubles |
| `include/lob/SpscQueue.hpp` | Lock-free ring buffer. `head_` is written only by the consumer and `tail_` only by the producer; a release store paired with an acquire load publishes each slot. The two counters sit on separate cache lines to avoid false sharing. |
| `include/lob/OrderBook.hpp`, `src/OrderBook.cpp` | Bids in `std::map<Price, std::list<Order>, std::greater<>>`, asks in `std::map<..., std::less<>>`, so `begin()` is always the best price. Each level is a FIFO `std::list`. An `unordered_map<OrderId, iterator>` makes cancels O(1) once the level is found. |
| `include/lob/MatchingEngine.hpp`, `src/MatchingEngine.cpp` | Owns the queue, the book and the matching thread. `stop()` drains every pending request before joining. |
| `tests/` | 7 queue tests (including a 2-thread stress test), 28 order book tests, 2 pipeline tests (including a 50,000-request randomized run checked trade for trade against a single-threaded reference) |
| `bench/Benchmark.cpp` | Throughput and latency benchmark (see below) |
| `docs/index.html` | Interactive visual demo (see "Interactive demo" above) |

### Matching rules
- An incoming limit order trades against the opposite side while prices cross, best price first. Within a price level, the oldest order trades first.
- Every trade executes at the **resting** order's price.
- Any unfilled remainder of a limit order rests in the book. Any unfilled remainder of a market order is discarded.
- The book rejects orders with zero quantity, a non-positive price, or a duplicate id.

## Benchmark

The workload is a reproducible random mix: 70% limit orders (about a third of them aggressive), 25% cancels, 5% market orders. The book is pre-filled with 20,000 resting orders. Each test runs 5 trials and reports the median.

Results on an Apple Silicon MacBook Pro, inside a 4-vCPU Linux VM, GCC 11, `-O3`:

| Metric | Result |
|---|---|
| Matching only (1 thread, no queue) | ~14.0 M requests/sec |
| Full pipeline (2 threads) | ~6.5–6.8 M requests/sec |
| Submit-to-processed latency at ~0.7 M req/s | p50 **250 ns**, p90 ~750 ns, p99 ~21–28 µs |

Notes:
- Latency is measured at a fixed offered load **below** capacity. Measured while the queue is flooded, latency would mostly reflect time spent waiting in a full queue.
- The p99 tail is dominated by stalls on the matching thread: heap allocation for new resting orders, plus vCPU scheduling in the VM. See "Possible improvements".
- Two fixes found through benchmarking: pre-sizing the order-id hash map (`OrderBook(expectedOrders)`) removed multi-millisecond rehash stalls and roughly doubled single-thread throughput (8.5 → 14 M/sec). Brief busy-polling before `yield()` cut median latency from ~420 ns to 250 ns.

## Possible improvements
- Replace `std::list` nodes with a pre-allocated object pool, so there is no `malloc` on the hot path
- Pin each thread to its own core
- Pop requests from the queue in batches to reduce cross-core cache traffic per request
- Replace the `std::map` price levels with a flat array indexed by tick, for bounded price ranges
