# Security Policy

The **ScannerAPIGateway** maintainers take the security of this project, its dependencies, and the downstream applications that rely on it seriously. This document outlines our policy regarding reporting vulnerabilities, supported versions, and our resolution timeline.

---

## Supported Versions

Only the most recent major and minor releases are actively supported with security updates. 

| Version | Supported          |
| :------ | :----------------- |
| `main`  | :white_check_mark: |
| `v1.x`  | :white_check_mark: |
| `< 1.0` | :x:                |

We strongly encourage all operators and developers using **ScannerAPIGateway** to keep their deployments on the latest patched release.

---

## Reporting a Vulnerability

**Please do not report security vulnerabilities through public GitHub issues, discussions, or pull requests.**

If you believe you have discovered a vulnerability in **ScannerAPIGateway**, please follow the responsible disclosure process below:

### 1. Preferred Channel: GitHub Private Vulnerability Reporting
If available on the repository, submit a report via GitHub's private vulnerability advisory dashboard:
- Navigate to the [ScannerAPIGateway Security Advisories](https://github.com/RatioJuris/ScannerAPIGateway/security/advisories) tab.
- Click **"Report a vulnerability"** to open a confidential report directly to the repository maintainers.

### 2. Alternative Channel: Direct Contact
If private reporting is not configured on the repository, send an encrypted or direct email to the project maintainers:
- **Contact:** `security@ratiojuris.dev` *(or contact the author directly via [RatioJuris GitHub Profile](https://github.com/RatioJuris))*
- **Subject line:** `[SECURITY] Potential vulnerability in ScannerAPIGateway`

---

## What to Include in Your Report

To help us validate and triage your finding as quickly as possible, please include:

1. **Description:** A detailed explanation of the vulnerability and its potential impact.
2. **Affected Components:** The affected versions, endpoints, routing rules, or middleware components.
3. **Reproduction Steps:** Clear, minimal steps or a Proof of Concept (PoC) script demonstrating the issue.
4. **Environment Details:** Relevant operating system, runtime/framework version, and network setup.
5. **Mitigation Ideas:** Any suggested fixes or temporary workarounds (if known).

---

## Disclosure and Resolution Process

Once a report is submitted, we commit to the following workflow:

1. **Acknowledgment:** We will acknowledge receipt of your vulnerability report within **48 hours**.
2. **Investigation & Triage:** Within **5 business days**, we will verify the report, assess the severity (using CVSS guidelines), and determine an appropriate remediation plan.
3. **Patch Development:** Maintainers will create an internal patch and test it against regression suites.
4. **Coordinated Disclosure:**
   - A patched release will be published.
   - A public security advisory will be issued via GitHub Advisories summarizing the issue, impacted versions, and credits.
   - Reporters who practice responsible disclosure will be credited in the release notes and advisory (unless they choose to remain anonymous).

---

## Out of Scope

The following areas are generally considered out of scope unless they demonstrate a direct, high-impact compromise of the gateway itself:

- Denial of Service (DoS/DDoS) attacks that rely solely on saturating bandwidth or volumetric traffic.
- Issues related to third-party dependencies where no working exploit against **ScannerAPIGateway** can be demonstrated.
- Reports from automated static analysis or dynamic vulnerability scanners without practical exploit verification.
- Attacks requiring physical access to a deployed server or compromised privileged host environments.

---

Thank you for helping keep **ScannerAPIGateway** and its users safe!
