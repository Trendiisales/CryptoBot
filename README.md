# CryptoBot C++ — Ultra-Low Latency Spot Trading Engine

## Compile (zero external dependencies beyond OpenSSL + pthreads)

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
# binary: build/cryptobot
```

Or single-command:
```bash
g++ -std=c++20 -O3 -march=native -DBOT_USE_OPENSSL \
    -Iinclude src/main.cpp \
    -lssl -lcrypto -lpthread \
    -o cryptobot
```

## Run

```bash
# Shadow/paper mode on live Binance market data
./cryptobot config/config.ini
```

## Configure

Edit `config/config.ini`:
- `shadow_mode = true`   → paper trade on live data (no real orders)
- `shadow_mode = false`  → blocked in this build
- Fill in `api_key` and `api_secret` under `[exchange]`
- Tune `[fees]`, `[risk]`, `[strategy]` sections
- `trade_log_file = logs/trades.csv` stores every closed trade persistently for evaluation
- `allow_short_entries = false` keeps spot trading long-only; SELL signals flatten longs

## Architecture

```
Feed thread   → SpscRingBuffer<CandleClose> → Signal thread
Signal thread → SpscRingBuffer<Signal>      → Exec thread
Exec thread   → FeeGate (8bps) → RiskManager → ShadowGateway
```

### Key design patterns

| Pattern | Where | Why |
|---|---|---|
| CRTP | `StrategyBase`, `GatewayBase`, mixins | Zero virtual dispatch in hot path |
| SeqLock | `SeqLocked<T>`, `AtomicPortfolio` | Lock-free consistent reads |
| SPSC ring buffer | `SpscRingBuffer<T,N>` | Zero-mutex inter-thread comms |
| MPSC ring buffer | `MpscRingBuffer<T,N>` | Multi-producer scenarios |
| Fixed arrays | `OrderBook`, `CandleBuffer`, `TradeSlot[]` | Zero heap allocation in hot path |
| `alignas(64)` | All hot structs | Eliminate false sharing |
| `atomic<bool>` kill switch | `RiskManager::halted_` | Single-writer, multi-reader halt |
| Bitcast double↔int64 | `AtomicDouble` in ShadowGateway | Atomic double without mutex |

## File structure

```
include/
  core/
    common.hpp          CRTP bases, AtomicIndex, Symbol, macros
    models.hpp          All data structs: Ticker, OrderBook, Signal, Trade…
    spsc_ring_buffer.hpp  SPSC + MPSC lock-free ring buffers
    config_parser.hpp   INI parser + typed Settings structs
    market_state.hpp    SeqLocked per-symbol state, CandleBuffer
    fee_gate.hpp        8 bps net edge enforcement
  strategies/
    strategies.hpp      Momentum, MeanReversion, OrderFlow, PerpBasis + Composer
  risk/
    risk_manager.hpp    Kelly sizing, drawdown kill switches
  execution/
    gateway.hpp         GatewayBase CRTP, ShadowGateway, LiveGateway
    trade_engine.hpp    Full trade lifecycle, templated on GatewayT
src/
  main.cpp              Entry point, thread launch, signal handling
config/
  config.ini            All runtime parameters
CMakeLists.txt          Build system
```

## Market Data Feed

The current build uses Binance HTTPS polling for:
- `bookTicker` every second
- `depth20` every two seconds
- closed `kline_1m` detection every five seconds

For lower latency, replace `MarketDataFeed` in `main.cpp` with a real
WebSocket client and subscribe to:
```
<symbol>@bookTicker       — best bid/ask (low latency)
<symbol>@depth20@100ms    — L2 order book
<symbol>@kline_1m         — 1m candles
<symbol>@aggTrade         — trade prints
wss://fstream.binance.com/<symbol>@markPrice@1s  — funding rate
```

## Disclaimer

Educational purposes. Crypto trading carries substantial risk.
Always run in shadow mode first. Never trade money you cannot afford to lose.

## Trade Journal

Each closed trade is appended to `trade_log_file` as CSV and flushed
immediately. The journal includes:
- symbol, side, signal type, confidence, expected edge
- entry and exit UTC timestamps, hold time, prices, quantity, notional
- order status and simulated latency for entry and exit
- gross P&L, fees, net P&L, P&L in bps, exit reason
- portfolio equity, available cash, cumulative P&L after the trade
