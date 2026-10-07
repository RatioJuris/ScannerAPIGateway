# Scanner API Gateway for Windows
*Unified Command-Line Interface for WIA, TWAIN, and eSCL Scanners*

**Scanner API Gateway** (by RatioJuris) is a lightweight, high-performance command-line utility for Windows. It bridges Windows Image Acquisition (WIA), 64-bit TWAIN, and network eSCL (AirScan) protocols into a single, predictable executable. 

By abstracting the complexities of underlying scanner drivers, it allows developers and system administrators to trigger scans, configure hardware, and receive structured JSON outputs directly from the terminal. It is built specifically for seamless integration into automated workflows, batch scripts, or parent backend applications.

---

## Key Features

* **Multi-Backend Support:** Communicates natively with WIA (Windows default), 64-bit TWAIN Data Source Managers, and eSCL/AirScan network scanners.
* **JSON Standardized Output:** Prints exactly one structured JSON object to standard output upon completion, making programmatic parsing effortless.
* **Granular Control:** Offers deep hardware adjustments including DPI, color mode, brightness, contrast, binarization threshold, and crop areas.
* **Automated Document Feeder (ADF) Logic:** Supports multi-page pulls, duplex scanning, and intelligent blank-page skipping.
* **Silent & Script-Safe:** Strict separation of data (stdout) and logging/progress (stderr).

---

## Installation & Requirements

1. **OS:** Windows 10, Windows 11, or Windows Server (64-bit recommended).
2. **Drivers:** Ensure your target scanner drivers (WIA or 64-bit TWAIN) are installed, or the scanner is accessible over the network via eSCL.
3. **Deployment:** Download the latest release from the repository and place `ScannerAPIGateway.exe` in a directory included in your system's PATH.

---

## Usage & Commands

The tool uses three primary commands: `list`, `scan`, and `help`.

### 1. Device Discovery
Discover and list all connected and available scanner devices.

* `ScannerAPIGateway.exe list`
* `ScannerAPIGateway.exe list --backends twain,escl` (Restricts the search to specific backends to speed up discovery)

Discovered devices are returned with a backend prefix. 
Examples: `wia:ScannerName`, `twain:TWAIN2Driver`, `escl:http://192.168.1.10:80/eSCL`

### 2. Initiating a Scan
Trigger a scan job. The `--out` argument is strictly required; all other parameters will fall back to sensible defaults.

* `ScannerAPIGateway.exe scan --out C:\Scans\`

### 3. Help and Version
Display tool version or usage syntax.

* `ScannerAPIGateway.exe help`
* `ScannerAPIGateway.exe version`

---

## Scan Options Reference

When using the `scan` command, you can append the following arguments to customize the job:

* **`--out <path>`** (Required)
  Destination path. If a directory (with a trailing backslash) or a path without an extension is provided, files are auto-named. Otherwise, saves as the exact file path provided.
* **`--device <id or auto>`** 
  The target scanner ID retrieved from the `list` command. Default: `auto`.
* **`--backends <list>`** 
  Comma-separated list of protocols to restrict communication to (`wia`, `twain`, `escl`). Default: all backends.
* **`--mode <color, gray, bw>`** 
  Color mode. `bw` indicates 1-bit black and white. Default: `color`.
* **`--dpi <75 to 1200>`** 
  Resolution in Dots Per Inch. Default: `300`.
* **`--source <auto, flatbed, adf, duplex>`** 
  Target paper source. Default: `auto`.
* **`--format <png, jpeg, bmp, tiff>`** 
  Output image format. Auto-inferred from `--out` if a file extension is provided; otherwise defaults to `png`.
* **`--quality <1 to 100>`** 
  Compression quality when outputting JPEG format. Default: `90`.
* **`--area <l,t,w,h>`** 
  Crop box dimensions in millimeters (Left, Top, Width, Height). Default: full page.
* **`--brightness <-1000 to 1000>`** 
  Hardware brightness adjustment (WIA and TWAIN only).
* **`--contrast <-1000 to 1000>`** 
  Hardware contrast adjustment (WIA and TWAIN only).
* **`--threshold <0 to 255>`** 
  Binarization threshold, applies only when `--mode` is set to `bw`.
* **`--blank-skip <0.0 to 1.0>`** 
  Drops pages where the dark-pixel ratio is less than or equal to the provided decimal value.
* **`--pages <0 to 1000>`** 
  Number of pages to pull. `0` pulls everything until the feeder is empty. Default: `0`.
* **`--timeout <5 to 600>`** 
  Idle seconds to wait for a hardware response before aborting. Default: `60`.
* **`--verbose`** 
  Streams real-time progress information to `stderr`.

---

## Output and Exit Codes

To ensure seamless integration with wrapper scripts and applications, Scanner API Gateway strictly separates payload data from logs.

* **Standard Output (stdout):** Prints exactly one JSON object containing job details, saved file paths, and success metrics.
* **Standard Error (stderr):** Prints progress text (only if `--verbose` is used).

### Standard Exit Codes

Catch these return codes in your parent scripts to properly handle hardware states:

* **`0` (OK):** Scan completed successfully. JSON payload is available on stdout.
* **`1` (Failure):** General execution failure or unexpected application crash.
* **`2` (Bad Arguments):** Invalid CLI syntax, missing required arguments, or out-of-bounds parameters.
* **`3` (No Scanner):** The requested device ID was not found, or no device was detected for `auto`.
* **`4` (Timeout):** The scanner did not respond within the timeframe specified by the timeout option.
* **`5` (Hardware State):** Device is currently unavailable (e.g., paper jam, empty ADF, cover open, or busy).
* **`6` (Backend Unavailable):** The specified subsystem (e.g., TWAIN DSM or WIA service) is unavailable or corrupt.

---

## Examples

**Example 1: Basic Single-Page Scan**
Scans the default device in color at 300 DPI, saving as a PNG to the desktop.
```cmd
ScannerAPIGateway.exe scan --out C:\Users\Admin\Desktop\document.png
```

**Example 2: Multi-Page ADF Batch to Directory**
Scans both sides of a document stack in grayscale, skipping blank pages, and saves the sequence of JPEGs to a specific folder.
```cmd
ScannerAPIGateway.exe scan --source duplex --mode gray --format jpeg --blank-skip 0.05 --out C:\Scans\Batch01\ --verbose
```

**Example 3: Targeted Network Scanner (eSCL)**
Targets a specific network scanner using its eSCL endpoint, scanning in black-and-white mode with a custom threshold.
```cmd
ScannerAPIGateway.exe scan --device "escl:http://192.168.1.50:80/eSCL" --mode bw --threshold 128 --out scan.tiff
```

---

## License
(c) 2026 RatioJuris. All rights reserved.
