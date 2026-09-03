# engine/ — real-time discipline

This file loads only when you touch `engine/`. Every rule here is constitution II–V
made concrete. If a rule seems to block you, the design is wrong — stop and say so.

## Two kinds of function

| Kind | Examples | May allocate | May throw | Marked |
|---|---|---|---|---|
| **Setup** | constructors, `prepare()`, `setScale()` | yes | yes | — |
| **Real-time** | `process()`, `reset()`, `snap()`, `latencySamples()` | **never** | **never** | `noexcept` |

Everything a real-time function needs is sized and allocated in `prepare()`.
A real-time function that needs more than `prepare()` gave it is a bug in `prepare()`.

## Banned inside real-time functions

`new` `delete` `malloc` `std::vector::push_back/resize` `std::string` `std::function`
`std::shared_ptr` copies `std::mutex` `std::lock_guard` `std::cout` `printf` `fopen`
`throw` `try` any `std::` container that can grow, any call whose cost depends on input.

Allowed: fixed-size arrays sized in `prepare()`, `std::atomic` loads/stores, arithmetic,
`std::array`, `<cmath>`, pre-allocated `std::vector` accessed by index only.

## Block contract

- `process()` receives exactly `n <= maxBlockSize` samples, mono, float, 48 kHz nominal.
- It must produce exactly `n` output samples every call, even before internal buffers
  fill — output silence or passthrough, never nothing.
- It never reads sample `i+1` while producing sample `i`. No lookahead. Constitution III.
- Ring buffers for analysis windows are allocated in `prepare()`, never resized.

## Parameters

UI → audio thread is one direction, via `std::atomic<float>` per parameter or a
lock-free swap of a `Params` snapshot. Never a mutex. Never a callback.

## Compiler flags are strict — expect casts

`-Wconversion -Wsign-conversion -Wdouble-promotion` are errors. Every `double`→`float`,
`size_t`→`int`, or `int`→`float` boundary needs an explicit `static_cast`. This is
deliberate: implicit narrowing produces audio that is *slightly* wrong, which is the
worst kind. Write the cast; don't loosen the flag.

## Numerics

- Frequencies in Hz as `float`. Cents = `1200 * log2(f / ref)`.
- MIDI note `m` ↔ Hz: `440 * 2^((m-69)/12)`. A4 = 69 = 440 Hz. Say so in a comment when used.
- Guard every division by a value that can be zero (silence → zero energy).
- Denormals: flush-to-zero in `prepare()`; document it.

## Tests for this directory

Every real-time function gets three tests minimum: a synthetic known-answer test
(sine in, measured out), a silence test, and a block-boundary test (same input split
into different block sizes must produce identical output). See `tests/support/Signals.h`.
