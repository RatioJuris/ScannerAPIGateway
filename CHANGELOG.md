# Changelog

All notable changes to **Scanner API Gateway** are documented here. This project follows semantic versioning.

## [1.2.0] - 2026-11-20

### Changed

- Unified the application version across all build artifacts. The executable, Windows resource metadata, TWAIN application identity, eSCL `User-Agent`, help text and documentation now all report **1.2.0** (previously they disagreed: `2.0.0` in code, `1.0.0.0` in the resource, `1.4.2` on the docs site).
- The JSON contract remains `contract_version: 1`; no output fields were added, removed or renamed.

### Fixed

- **Black-and-white JPEG output** could fail because WIC's JPEG encoder does not support 1-bit pixel formats. Bi-level pages are now encoded as 8-bit grayscale for JPEG, while PNG/BMP/TIFF keep true 1-bit data.
- **Long eSCL URLs** were silently truncated by fixed-size `WinHttpCrackUrl` buffers. Buffers are now sized from the input URL. This affects both device endpoints and scan-job `Location` headers.
- **Missing option values** such as `scan --out --dpi 300` were accepted, with the next option being consumed as the value. These now fail with exit code `2`.
- Encoder errors when writing JPEG quality or image resolution are no longer silently ignored.

### Added

- CI smoke test now verifies the reported version and the missing-value argument check.
- Linux and macOS builds (`src/ScannerAPIGatewayPosix.cpp`) with the same CLI and JSON contract. These platforms expose the `escl` (AirScan) backend only; WIA and TWAIN remain Windows-only.
- GitHub Actions now publishes Windows x86 and x64 executables, a portable Windows ZIP, Linux x64/arm64 builds, macOS x64/arm64 builds, and SHA-256 checksums for every artifact.
- A new `icon.ico` and an API-focused `docs/index.html` reference.
- `AGENTS.md` now explains how an integrating agent can use the CLI/JSON API to add scanning to an app without the printer manufacturer's software.

## [1.0.0]

- Initial public release with WIA 2.0, 64-bit TWAIN and eSCL/AirScan backends, a shared WIC image pipeline and the single-object JSON stdout contract.
