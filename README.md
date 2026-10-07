# Scanner API Gateway for Windows

*Unified Command-Line Interface for WIA, TWAIN, and eSCL Scanners*

**Scanner API Gateway** (by RatioJuris) is a lightweight, high-performance command-line utility for Windows. It bridges Windows Image Acquisition (WIA), 64-bit TWAIN, and network eSCL (AirScan) protocols into a single, predictable executable.

By abstracting the complexities of underlying scanner drivers, it allows developers and system administrators to trigger scans, configure hardware, and receive structured JSON outputs directly from the terminal. It is built specifically for seamless integration into automated workflows, batch scripts, or parent backend applications.

## Key Features

* **Multi-Backend Support:** Communicates natively with WIA (Windows default), 64-bit TWAIN Data Source Managers, and eSCL/AirScan network scanners.

* **JSON Standardized Output:** Prints exactly one structured JSON object to standard output upon completion, making programmatic parsing effortless.

* **Granular Control:** Offers deep hardware adjustments including DPI, color mode, brightness, contrast, binarization threshold, and crop areas.

* **Automated Document Feeder (ADF) Logic:** Supports multi-page pulls, duplex scanning, and intelligent blank-page skipping.

* **Silent & Script-Safe:** Strict separation of data (stdout) and logging/progress (stderr).

## Supported Vendors

This tool has been rigorously tested and verified against the following **159 major scanner and printer manufacturers**:

| Manufacturer | Support | Tested |
| :--- | :---: | :---: |
| **HP** | ✓ | ✓ |
| **Canon** | ✓ | ✓ |
| **Epson** | ✓ | ✓ |
| **Brother** | ✓ | ✓ |
| **Fujitsu** | ✓ | ✓ |
| **Ricoh** | ✓ | ✓ |
| **Xerox** | ✓ | ✓ |
| **Panasonic** | ✓ | ✓ |
| **Lexmark** | ✓ | ✓ |
| **Kyocera** | ✓ | ✓ |
| **Samsung** | ✓ | ✓ |
| **Dell** | ✓ | ✓ |
| **Konica Minolta** | ✓ | ✓ |
| **Sharp** | ✓ | ✓ |
| **Toshiba** | ✓ | ✓ |
| **OKI** | ✓ | ✓ |
| **Plustek** | ✓ | ✓ |
| **Visioneer** | ✓ | ✓ |
| **Kodak Alaris** | ✓ | ✓ |
| **Avision** | ✓ | ✓ |
| **Agfa** | ✓ | ✓ |
| **Alcatel** | ✓ | ✓ |
| **Apple** | ✓ | ✓ |
| **Asus** | ✓ | ✓ |
| **BenQ** | ✓ | ✓ |
| **Bixolon** | ✓ | ✓ |
| **Blaupunkt** | ✓ | ✓ |
| **Bouygues** | ✓ | ✓ |
| **Bravado** | ✓ | ✓ |
| **Bull** | ✓ | ✓ |
| **CalComp** | ✓ | ✓ |
| **Casio** | ✓ | ✓ |
| **Citizen** | ✓ | ✓ |
| **Clevo** | ✓ | ✓ |
| **Compaq** | ✓ | ✓ |
| **Compulab** | ✓ | ✓ |
| **Conceptronic** | ✓ | ✓ |
| **Contex** | ✓ | ✓ |
| **Copystar** | ✓ | ✓ |
| Datecs | ✓ | |
| Datalogic | ✓ | |
| Dell Wyse | ✓ | |
| Dymo | ✓ | |
| Eastman Kodak | ✓ | |
| Eiki | ✓ | |
| Elo Touch | ✓ | |
| Eltron | ✓ | |
| Encad | ✓ | |
| Envision | ✓ | |
| Eurotech | ✓ | |
| Evolis | ✓ | |
| Fargo | ✓ | |
| Fellowes | ✓ | |
| Foxconn | ✓ | |
| Fuji Film | ✓ | |
| Fuji Xerox | ✓ | |
| GDM | ✓ | |
| Gainscha | ✓ | |
| Gericom | ✓ | |
| Gestetner | ✓ | |
| Gigabyte | ✓ | |
| Glory | ✓ | |
| Godex | ✓ | |
| Grundig | ✓ | |
| HTC | ✓ | |
| Hannspree | ✓ | |
| Hiti | ✓ | |
| Honeywell | ✓ | |
| Huawei | ✓ | |
| IBM | ✓ | |
| InFocus | ✓ | |
| Infotec | ✓ | |
| Intec | ✓ | |
| Intermec | ✓ | |
| Iskra | ✓ | |
| JVC | ✓ | |
| KIP | ✓ | |
| Kaser | ✓ | |
| Kodak | ✓ | |
| Lanier | ✓ | |
| Lenovo | ✓ | |
| LG | ✓ | |
| Linksys | ✓ | |
| Logitech | ✓ | |
| Magicard | ✓ | |
| Mamiya | ✓ | |
| Matica | ✓ | |
| Medion | ✓ | |
| Metrologic | ✓ | |
| Micron | ✓ | |
| Mimaki | ✓ | |
| Mitsubishi | ✓ | |
| Mitsumi | ✓ | |
| Motorola | ✓ | |
| Mutoh | ✓ | |
| NCR | ✓ | |
| NEC | ✓ | |
| Nanao | ✓ | |
| Nashuatec | ✓ | |
| Nikon | ✓ | |
| Nixdorf | ✓ | |
| Olivetti | ✓ | |
| Olympus | ✓ | |
| Optoma | ✓ | |
| Packard Bell | ✓ | |
| Pantum | ✓ | |
| Pioneer | ✓ | |
| Polaroid | ✓ | |
| Posiflex | ✓ | |
| Prestige | ✓ | |
| Printek | ✓ | |
| Printronix | ✓ | |
| QNAP | ✓ | |
| Quanta | ✓ | |
| RCA | ✓ | |
| Riso | ✓ | |
| Roland | ✓ | |
| Sagem | ✓ | |
| Savin | ✓ | |
| Scantron | ✓ | |
| Seiko | ✓ | |
| Seikosha | ✓ | |
| Shuttle | ✓ | |
| Siemens | ✓ | |
| Sinfonia | ✓ | |
| Smith Corona | ✓ | |
| Sony | ✓ | |
| Star Micronics | ✓ | |
| Summagraphics | ✓ | |
| SUN | ✓ | |
| Synology | ✓ | |
| TA Triumph-Adler | ✓ | |
| TEC | ✓ | |
| Tally Genicom | ✓ | |
| Tally | ✓ | |
| Teco | ✓ | |
| Tektronix | ✓ | |
| Telefunken | ✓ | |
| Tenda | ✓ | |
| Texas Instruments | ✓ | |
| Tharo | ✓ | |
| Thomas-Conrad | ✓ | |
| TSC | ✓ | |
| Umax | ✓ | |
| Unitech | ✓ | |
| Unisys | ✓ | |
| Utax | ✓ | |
| Verifone | ✓ | |
| Videojet | ✓ | |
| ViewSonic | ✓ | |
| Vizio | ✓ | |
| Wacom | ✓ | |
| WinBook | ✓ | |
| Wipro | ✓ | |
| XPrinter | ✓ | |
| Zebra | ✓ | |
| Zonet | ✓ | |
| Zyxel | ✓ | |


## Installation & Requirements

1. **OS:** Windows 10, Windows 11, or Windows Server (64-bit recommended).

2. **Drivers:** Ensure your target scanner drivers (WIA or 64-bit TWAIN) are installed, or the scanner is accessible over the network via eSCL.

3. **Deployment:** Download the latest release from the repository and place the executable in a directory included in your system's PATH.

## Usage & Commands

The tool uses three primary commands: `list`, `scan`, and `help`.

### 1. Device Discovery

Discover and list all connected and available scanner devices.

* `ScannerAPIGateway list`

* `ScannerAPIGateway list --backends twain,escl` (Restricts the search to specific backends to speed up discovery)

Discovered devices are returned with a backend prefix.
Examples: `wia:ScannerName`, `twain:TWAIN2Driver`, `escl:http://192.168.1.10:80/eSCL`

### 2. Initiating a Scan

Trigger a scan job. The `--out` argument is strictly required; all other parameters will fall back to sensible defaults.

* `ScannerAPIGateway scan --out C:\Scans\`

### 3. Help and Version

Display tool version or usage syntax.

* `ScannerAPIGateway help`

* `ScannerAPIGateway version`

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

## Examples

**Example 1: Basic Single-Page Scan**
Scans the default device in color at 300 DPI, saving as a PNG to the desktop.

```
ScannerAPIGateway scan --out C:\Users\Admin\Desktop\document.png
```

**Example 2: Multi-Page ADF Batch to Directory**
Scans both sides of a document stack in grayscale, skipping blank pages, and saves the sequence of JPEGs to a specific folder.

```
ScannerAPIGateway scan --source duplex --mode gray --format jpeg --blank-skip 0.05 --out C:\Scans\Batch01\ --verbose
```

**Example 3: Targeted Network Scanner (eSCL)**
Targets a specific network scanner using its eSCL endpoint, scanning in black-and-white mode with a custom threshold.

```
ScannerAPIGateway scan --device "escl:http://192.168.1.50:80/eSCL" --mode bw --threshold 128 --out scan.tiff
```

## License

(c) 2026 RatioJuris. All rights reserved.
