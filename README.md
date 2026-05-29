# Ascend D2H Bench

Small C++ benchmark for comparing Ascend device-to-host copy methods and host buffer allocation methods.

It is intended to answer questions such as:

- Is many-small-copy `aclrtMemcpyAsync + aclrtSynchronizeStream` slower than `aclrtMemcpyBatch`?
- How much does stream count change D2H throughput?
- Does `aclrtMallocHost` behave differently from UCM-style `mmap + mlock + aclrtHostRegister`?
- Does explicit `ACL_HOST_REG_PINNED` registration help on the same copy pattern?

## Build

Run on an Ascend/CANN development environment:

```bash
export ASCEND_HOME_PATH=/usr/local/Ascend/ascend-toolkit/latest
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The CMake probe enables optional modes when the CANN headers support them:

- `register-pinned` requires `aclrtHostRegisterV2`.
- `batch` requires `aclrtMemcpyBatch`.

## Usage

```bash
./build/ascend_d2h_bench \
  --device 0 \
  --mode all \
  --allocator all \
  --io-size 128k \
  --io-count 64 \
  --tensor-size-list 131072,16384,32768 \
  --streams 4 \
  --warmup 5 \
  --iters 50
```

Core parameters:

- `--mode async-loop|batch|sync|all`
  - `async-loop`: UCM-like pattern. Submit `io-count` `aclrtMemcpyAsync` calls round-robin across `--streams`, then synchronize all streams.
  - `batch`: Use `aclrtMemcpyBatch` in batches up to `--batch-size`.
  - `sync`: Use blocking `aclrtMemcpy` as a baseline.
- `--allocator aclrt-malloc-host|ucm-direct|register-pinned|all`
  - `aclrt-malloc-host`: host buffer from `aclrtMallocHost`.
  - `ucm-direct`: UCM direct-IO style: hugepage/mmap fallback, `mlock`, then legacy `aclrtHostRegister(... ACL_HOST_REGISTER_MAPPED ...)`.
  - `register-pinned`: same mmap/mlock allocation, then `aclrtHostRegisterV2(... ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED ...)`.
- `--io-size`: bytes per D2H slice. Suffixes `k`, `m`, and `g` are accepted.
- `--io-count`: number of D2H slices per measured iteration.
- `--tensor-size-list`: optional comma-separated D2H slice pattern. When set, the benchmark repeats this list until `--io-count` slices are generated, so `--tensor-size-list 131072,16384,32768 --io-count 64` simulates a more fragmented tensor layout than fixed-size slices.
- `--streams`: stream count for `async-loop`.
- `--batch-size`: max slice count per `aclrtMemcpyBatch` call.
- `--warmup`: unmeasured warmup iterations.
- `--iters`: measured iterations.
- `--verify`: sample-check copied bytes after warmup and after measured iterations.
- `--csv`: print CSV rows.

## Examples

Compare allocation methods for UCM-like async copy:

```bash
./build/ascend_d2h_bench --mode async-loop --allocator all --io-size 64k --io-count 128 --streams 4
```

Compare async loop and batch copy for 128 KB slices:

```bash
./build/ascend_d2h_bench --mode all --allocator ucm-direct --io-size 128k --io-count 64 --streams 4
```

Simulate a fragmented tensor size list:

```bash
./build/ascend_d2h_bench --mode all --allocator ucm-direct --tensor-size-list 131072,16384,32768 --io-count 192 --streams 4
```

Sweep stream count:

```bash
for s in 1 2 4 8 16; do
  ./build/ascend_d2h_bench --mode async-loop --allocator ucm-direct --io-size 128k --io-count 64 --streams "$s" --csv
done
```

Sweep IO size:

```bash
for size in 4k 8k 16k 32k 64k 128k 256k 512k 1m 2m 4m 8m; do
  ./build/ascend_d2h_bench --mode all --allocator all --io-size "$size" --io-count 64 --streams 4 --csv
done
```

## Metrics

For each mode/allocator case, the benchmark prints:

- `total_us`: measured iteration duration.
- `submit_us`: time spent issuing copy calls.
- `sync_wait_us`: time spent waiting after submit. For `async-loop`, this is the final stream synchronization wait.
- `bandwidth_avg_GBps`: decimal GB/s, computed from the actual copied bytes per iteration divided by average total time.

Timing is per iteration, not per copy, so the measurement path does not add high-frequency timing calls inside the copy loop.
