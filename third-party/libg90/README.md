# libg90

C++20 library for the Xiegu G90 body-link serial protocol (DE-9, 115200 8N1):
encode and decode 96-byte head frames and 372-byte body frames, plus a small
POSIX wire layer and the `g90ctl` bench CLI.

## Requirements

- CMake ≥ 3.21
- C++20 compiler (Clang, GCC, or Apple Clang)
- Make (presets use Unix Makefiles)

## Build

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Other presets: `ci`, `sanitize` (ASan+UBSan).

Install exports the static CMake package `libg90::libg90` (ExactVersion **0.1.0**).

```bash
cmake --install build/dev --prefix /usr/local
```

## Usage

```cpp
#include <g90/g90.hpp>

g90::HeadEncodeRequest req;
req.profile = g90::Profile::V181;
req.vfo_a.freq_hz = 14'225'000;
req.vfo_a.mode = g90::Mode::Usb;

auto encoded = g90::encode_head(req);
if (!encoded.ok()) {
  // encoded.error
}

auto decoded = g90::decode_head(req.profile, *encoded.value);
```

Live serial I/O:

```cpp
g90::De9Link link;
link.open("/dev/cu.usbserial-XXXX");
auto body = link.exchange(req, std::chrono::milliseconds{500});
```

See [`docs/API.md`](docs/API.md) for the full surface.

## g90ctl

```bash
./build/dev/cli/g90ctl --version
./build/dev/cli/g90ctl encode --profile 1.81 --mode USB --freq 14225000
./build/dev/cli/g90ctl decode head --profile 1.81 <hex>
./build/dev/cli/g90ctl decode body <hex>
./build/dev/cli/g90ctl live --port /dev/cu.usbserial-XXXX --profile 1.81 \
  --mode USB --freq 14225000 --confirm-bench
```

`live` requires `--port` or `--auto-port` and `--confirm-bench` (or
`--i-know-what-im-doing`). TX is refused unless `--allow-tx`. Details:
[`docs/CLI.md`](docs/CLI.md), bench notes in [`docs/BENCH-LOG.md`](docs/BENCH-LOG.md).

## Lint

```bash
bash scripts/clang_format.sh check   # or: fix
cmake --preset dev
bash scripts/clang_tidy.sh
```

On macOS, install Homebrew LLVM for `clang-tidy`:

```bash
brew install llvm
export PATH="$(brew --prefix llvm)/bin:$PATH"
```

## Tests and fixtures

Checked-in DE-9 hex (static wire bytes) lives under `tests/fixtures/`. Design
notes: [`docs/DESIGN.md`](docs/DESIGN.md).

## Documentation

| Doc | Topic |
|-----|--------|
| [`docs/API.md`](docs/API.md) | Public API overview |
| [`docs/CLI.md`](docs/CLI.md) | `g90ctl` |
| [`docs/DESIGN.md`](docs/DESIGN.md) | Design decisions |
| [`docs/ENCODE-PADDING.md`](docs/ENCODE-PADDING.md) | Stock head padding |
| [`docs/BENCH-LOG.md`](docs/BENCH-LOG.md) | Live bench session log |

## License

MIT — see [`LICENSE`](LICENSE).
