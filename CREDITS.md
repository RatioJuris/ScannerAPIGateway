# NOTICES AND LICENSES FOR THIRD-PARTY SOFTWARE

This application incorporates components from the Microsoft Windows SDK and the C++ Standard Library. The licensing terms and attributions for these components are detailed below.

---

## 1. Microsoft Windows SDK

**Component Name:** Microsoft Windows Software Development Kit (SDK)  
**Copyright:** Copyright (c) Microsoft Corporation. All rights reserved.  
**License Type:** Microsoft Software License Terms for Windows SDK  

### Associated Source Declarations & Libraries:
- **Networking & Communications:** `winsock2.h`, `ws2tcpip.h`, `iphlpapi.h`, `winhttp.h`, `ws2_32.lib`, `iphlpapi.lib`, `winhttp.lib`
- **Core OS, COM, & Shell API:** `windows.h`, `objbase.h`, `propidl.h`, `shellapi.h`, `shlwapi.h`, `ole32.lib`, `oleaut32.lib`, `shlwapi.lib`, `shell32.lib`, `user32.lib`, `uuid.lib`
- **Graphics & Windows Imaging Component (WIC):** `wincodec.h`, `windowscodecs.lib`
- **Windows Image Acquisition (WIA) & WRL:** `wia.h`, `wrl/client.h`, `wiaguid.lib`

### License Terms & Conditions:
The components listed above are licensed under the Microsoft Software License Terms for the Windows Software Development Kit. These components are part of the underlying host operating system environment runtime and are utilized by the application for system-level interoperability.

---

## 2. ISO/IEC C++ Standard Library

**Component Name:** C++ Standard Template Library (STL) Run-Time  
**Copyright:** Copyright (c) ISO/IEC Standard ISO/IEC 14882 / Toolchain Maintainers.  
**License Type:** Runtime Exception License (MSVC / LLVM / GCC Runtime Exception)  

### Associated Source Headers:
`<algorithm>`, `<atomic>`, `<charconv>`, `<chrono>`, `<cmath>`, `<cstdint>`, `<cstdio>`, `<cstring>`, `<filesystem>`, `<fstream>`, `<functional>`, `<map>`, `<memory>`, `<mutex>`, `<optional>`, `<set>`, `<sstream>`, `<stdexcept>`, `<string>`, `<string_view>`, `<thread>`, `<vector>`

### License Terms & Conditions:
The C++ Standard Library headers are governed by the runtime licensing exceptions of the respective compiler toolchain utilized during compilation (e.g., Microsoft Visual C++ Runtime License, LLVM Exception to the Apache 2.0 License, or the GNU GCC Runtime Library Exception). These exceptions explicitly permit the unrestricted compilation and distribution of binary applications containing these compiled templates without requiring source attribution or secondary license propagation.

---

## 3. General Liability & Warranty Disclaimer

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
