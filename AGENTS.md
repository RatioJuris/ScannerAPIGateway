# AGENTS.md — Integrating Scanner API Gateway into your app

## Why this exists

Scanner API Gateway lets your application talk to scanners **without shipping or
depending on a printer manufacturer's SDK/utility**. You launch one executable, pass
CLI arguments, and read one JSON object from stdout. There is no GUI, no DLL to link,
no COM interop in your code, and no vendor application to install.

| Backend | What it uses | Manufacturer software needed? |
| :--- | :--- | :--- |
| `escl` | eSCL/AirScan over HTTP + mDNS (network scanners) | **No** — standard protocol built into most modern network scanners/MFPs |
| `wia` | Windows Image Acquisition 2.0 (built into Windows) | **No** — uses the OS-supplied WIA class driver where available |
| `twain` | 64-bit/32-bit TWAIN Data Source Manager (`twaindsm.dll`) | Only the scanner's TWAIN **data source**, not a bundled utility app |

Prefer `escl` for network devices and `wia` for locally attached Windows scanners.
Use `twain` only when a device exposes no WIA or eSCL path.

## How an agent should use it

1. **Discover**: run `ScannerAPIGateway list`. Parse the `devices` array. Each `id`
   is prefixed (`wia:…`, `twain:…`, `escl:http://…`).
2. **Scan**: run `ScannerAPIGateway scan --out <path> [--device <id>] [options]`.
   `--out` is required; everything else has defaults.
3. **Read the result**: parse the single JSON object on stdout. Saved files are listed
   in `pages[]` with absolute `path`, `content_type`, and `bytes`.
4. **Check the exit code** before trusting the JSON. Non-zero means the JSON is an error
   object with `error.code` and `error.message`.
5. **Never try to decode the binary yourself.** The gateway already produced final
   PNG/JPEG/BMP/TIFF files. Treat `.exe`, `.ico`, `.res`, and `.sha256` files as opaque
   build artifacts — don't grep or hand-edit them.

### Minimal integration example (Python)

```python
import json, subprocess

EXE = r"C:\path\to\ScannerAPIGateway.exe"   # or ./ScannerAPIGateway on Linux/macOS

def call(*args):
    p = subprocess.run([EXE, *args], capture_output=True, text=True, encoding="utf-8")
    return p.returncode, json.loads(p.stdout)   # stdout is always exactly one JSON object

code, found = call("list")
if code != 0:
    raise RuntimeError(found["error"]["message"])

device = next((d["id"] for d in found["devices"] if d["backend"] == "escl"), "auto")

code, result = call("scan", "--device", device, "--out", r"C:\scans\", "--mode", "gray", "--dpi", "300")
if code != 0:
    raise RuntimeError(result["error"]["message"])

for page in result["pages"]:
    print(page["path"], page["bytes"])
```

The same pattern works from C#, Node, PowerShell or any language that can spawn a
process. Do not link against the executable; it is a standalone CLI API.

## Contract rules (do not break these)

- `contract_version` is `1` and must stay `1` across 1.x releases.
- stdout: exactly one JSON object. stderr: only progress, only with `--verbose`.
- Exit codes are part of the API:

  | Code | Meaning |
  | ---: | :--- |
  | 0 | Success |
  | 1 | General failure |
  | 2 | Bad arguments |
  | 3 | No scanner / device not found |
  | 4 | Timeout |
  | 5 | Hardware state (jam, empty ADF, cover open, busy) |
  | 6 | Backend unavailable (no WIA service, no DSM, network unreachable) |

- Error objects look like `{"ok":false,"contract_version":1,"error":{"code":"…","message":"…"}}`.
  Branch on `error.code`, not on message text.
- The current version is `1.2.0`; only the `version` command's `version` value changed
  from 1.0. Keep `kVersion`, the `.rc` resource, README, CHANGELOG and docs in sync.

## Useful options for app integration

| Option | Notes for an integrating app |
| :--- | :--- |
| `--out <dir/ | file.ext>` | Directory form auto-names files with a timestamp and `_p0001` suffix. File form means exact name for page 1. Parent directories are created automatically. |
| `--device <id|auto>` | Pass an `id` from `list` for deterministic targeting; `auto` picks the first scanner. |
| `--backends wia,twain,escl` | Restrict discovery; use `escl` to avoid waiting on WIA/TWAIN enumeration. |
| `--format png|jpeg|bmp|tiff` | Inferred from `--out` extension; otherwise PNG. JPEG quality via `--quality`. |
| `--mode color|gray|bw`, `--dpi`, `--threshold` | Applied by the shared pipeline, so results are identical across backends. |
| `--source auto|flatbed|adf|duplex`, `--pages`, `--blank-skip` | ADF/duplex batch controls; blank pages are dropped before final naming. |
| `--timeout <5..600>` | Idle seconds per hardware step. Use a larger value for slow network scanners. |
| `--verbose` | Progress on stderr; keep stdout clean for `json.loads`. |

## Platform behaviour

- **Windows build** (`src/ScannerAPIGateway.cpp`): WIA, TWAIN (x86 or x64 DSM) and
  eSCL. Builds as x86 or x64. `version` reports `arch` accordingly.
- **Linux/macOS build** (`src/ScannerAPIGatewayPosix.cpp`): eSCL only, because WIA and
  TWAIN are Windows APIs. It produces the same CLI and JSON shape, reports
  `backends: ["escl"]`, and uses libcurl plus vendored `stb_image` headers.
  `--backends wia` or `--device wia:…` on these platforms is a bad-argument error.

## Repository map for agents

| Path | What to do with it |
| :--- | :--- |
| `src/ScannerAPIGateway.cpp` | Windows implementation. Change here for WIA/TWAIN/WinHTTP/WIC behavior. |
| `src/ScannerAPIGatewayPosix.cpp` | Linux/macOS implementation. Change here for eSCL behavior. |
| `src/stb_image.h`, `src/stb_image_write.h` | Vendored header-only image codecs (POSIX build only). Do not edit. |
| `src/ScannerAPIGateway.rc`, `src/icon.ico` | Windows version metadata and icon. Binary files are opaque. |
| `.github/workflows/build*.yml` | Cross-platform build, smoke tests, checksums, artifacts and tagged releases. |
| `docs/index.html` | Human-facing API reference. Keep it about the API, not visual design. |
| `README.md`, `CHANGELOG.md`, `SECURITY.md` | Consumer docs and release policy. |

## Verification

There is no compiler installed in the development environment; GitHub Actions is the
source of truth. CI smoke tests check:

- `version` → exit 0 and `"version":"1.2.0"`
- `scan --dpi 5` → exit 2
- `scan --blank-skip 2` → exit 2
- `scan --out --dpi 300` → exit 2
- `list --backends twain` → exit 0 on Windows, even without a DSM; exit 2 on Linux/macOS

Do not claim a build succeeded unless CI ran it. Tag `v1.2.0` to trigger a release build.
