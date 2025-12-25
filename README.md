# PocketFox

**Native Cocoa browser for Mac OS X Tiger/Leopard with built-in TLS 1.2**

PocketFox is a lightweight web browser that brings modern HTTPS to PowerPC Macs. Instead of relying on Tiger's broken OpenSSL, it embeds mbedTLS 2.28 LTS directly.

## Features

- Native Cocoa UI (Objective-C)
- TLS 1.2 with ChaCha20-Poly1305
- HTTP and HTTPS support
- Works on Tiger (10.4) and Leopard (10.5)
- PowerPC G4/G5 optimized

## Screenshots

Coming soon!

## Building

### Prerequisites

1. **Xcode 2.5** (Tiger) or **Xcode 3.1** (Leopard)
2. **mbedTLS 2.28** compiled for PowerPC

### Build mbedTLS First

```bash
# Download mbedTLS 2.28 LTS
# (use wget_tiger if you have it, or curl from another machine)
tar xzf mbedtls-2.28.8.tar.gz
cd mbedtls-2.28.8/library

# Compile for PowerPC
gcc -arch ppc -std=c99 -O2 -mcpu=7450 -I../include \
    -DMBEDTLS_NO_PLATFORM_ENTROPY -c *.c

# Create libraries
ar rcs libmbedcrypto.a aes.o arc4.o aria.o asn1parse.o asn1write.o \
    base64.o bignum.o camellia.o ccm.o chacha20.o chachapoly.o \
    cipher.o cipher_wrap.o ctr_drbg.o des.o dhm.o ecdh.o ecdsa.o \
    ecjpake.o ecp.o ecp_curves.o entropy.o entropy_poll.o error.o \
    gcm.o havege.o hkdf.o hmac_drbg.o md.o md5.o memory_buffer_alloc.o \
    nist_kw.o oid.o padlock.o pem.o pk.o pk_wrap.o pkcs12.o pkcs5.o \
    pkparse.o pkwrite.o platform.o platform_util.o poly1305.o \
    ripemd160.o rsa.o rsa_internal.o sha1.o sha256.o sha512.o \
    threading.o timing.o version.o version_features.o xtea.o

ar rcs libmbedx509.a certs.o pkcs11.o x509.o x509_create.o x509_crl.o \
    x509_crt.o x509_csr.o x509write_crt.o x509write_csr.o

ar rcs libmbedtls.a debug.o net_sockets.o ssl_cache.o ssl_ciphersuites.o \
    ssl_cli.o ssl_cookie.o ssl_msg.o ssl_srv.o ssl_ticket.o ssl_tls.o
```

### Build PocketFox

```bash
# Build SSL library first
gcc -arch ppc -std=c99 -O2 -mcpu=7450 -DHAVE_MBEDTLS \
    -I./mbedtls-2.28.8/include -c pocketfox_ssl_tiger.c -o pocketfox_ssl.o

# Build PocketFox app
gcc -arch ppc -O2 -mcpu=7450 -DHAVE_MBEDTLS -framework Cocoa \
    -I./mbedtls-2.28.8/include -o PocketFox \
    pocketfox_tiger_gui.m pocketfox_ssl.o \
    -L./mbedtls-2.28.8/library -lmbedtls -lmbedx509 -lmbedcrypto

# Create app bundle
mkdir -p PocketFox.app/Contents/MacOS
cp PocketFox PocketFox.app/Contents/MacOS/
```

Or use the build script:
```bash
./build_pocketfox.sh mbedtls   # Build mbedTLS
./build_pocketfox.sh browser   # Build PocketFox
./build_pocketfox.sh package   # Create .app bundle
```

## Components

| File | Description |
|------|-------------|
| `pocketfox_tiger_gui.m` | Main Cocoa browser UI |
| `pocketfox_ssl_tiger.c` | mbedTLS integration layer |
| `pocketfox_ssl.h` | SSL header file |
| `pocketfox_browser.m` | Alternative browser implementation |
| `wget_tiger.c` | Standalone wget with TLS 1.2 |
| `build_pocketfox.sh` | Build automation script |

## wget for Tiger

Also included is a standalone wget implementation with HTTPS:

```bash
# Build
gcc -arch ppc -std=c99 -O2 -DHAVE_MBEDTLS \
    -I./mbedtls-2.28.8/include -o wget \
    wget_tiger.c pocketfox_ssl_tiger.c \
    -L./mbedtls-2.28.8/library -lmbedtls -lmbedx509 -lmbedcrypto

# Usage
./wget https://example.com/file.tar.gz
./wget -O output.txt https://site.com/page
```

**Features:**
- TLS 1.2 with modern ciphers
- HTTP 301/302 redirect following
- Chunked transfer encoding support
- Progress bar with percentage
- Downloads from GitHub!

## Why PocketFox?

Tiger's built-in OpenSSL (0.9.7) and Safari can't connect to modern HTTPS sites. Rather than trying to patch the system, PocketFox embeds modern TLS directly.

## Target Platforms

- **Mac OS X Tiger (10.4)** - PowerPC G4/G5
- **Mac OS X Leopard (10.5)** - PowerPC G4/G5 and Intel

## Related Projects

- [rust-ppc-tiger](https://github.com/Scottcjn/rust-ppc-tiger) - Rust compiler for PowerPC
- [ppc-tiger-tools](https://github.com/Scottcjn/ppc-tiger-tools) - Tools for Tiger/Leopard
- [macosx-security-patches](https://github.com/Scottcjn/macosx-security-patches) - CVE patches

## Contributors

- **Scott (Scottcjn)** - Creator, architect, hardware lab, testing
- **Claude (Opus 4.1/4.5)** - Implementation assistance

*Designed by Scott, coded with Claude*

## License

MIT License - Free to use, please keep attribution.

## Community

Join the RustChain Discord:

[![Discord](https://img.shields.io/badge/Discord-RustChain-7289DA?logo=discord&logoColor=white)](https://discord.gg/tQ4q3z4M)

---

## Traffic Note

**600+ clones across 14 repos in under 48 hours. Zero stars.**

This work is being actively scraped by someone — government HPC labs, AI research groups, defense contractors? If you're mirroring for research purposes, feel free to reach out. Otherwise, a star would be nice.

The clone-to-star ratio is the purest form of underground validation. We see you. 👁️

---

*"Modern web on your 2005 Power Mac."*
