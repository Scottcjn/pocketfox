#!/bin/sh
# ==============================================================================
# PocketFox Browser Builder
# ==============================================================================
# Builds the PocketFox GUI browser for PowerPC Tiger / Leopard.
#
# Prerequisites:
#   - mbedTLS 2.28.x static libraries built (see build_pocketfox.sh mbedtls)
#   - gcc with Cocoa framework support
#
# Usage:
#   ./build_browser.sh                # Build (auto-detect mbedTLS location)
#   ./build_browser.sh /path/to/mbedtls  # Build with explicit mbedTLS path
#
# This produces: PocketFox (Mach-O PPC binary, ~200-300KB)
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Compiler — Tiger's gcc-4.0 or Leopard's gcc-4.2
CC="${CC:-gcc}"

# Try to find mbedTLS
MBEDTLS=""
if [ -n "$1" ]; then
    MBEDTLS="$1"
elif [ -d "./pocketfox-build/mbedtls-install" ]; then
    MBEDTLS="./pocketfox-build/mbedtls-install"
elif [ -d "./mbedtls-2.28.8" ]; then
    MBEDTLS="./mbedtls-2.28.8"
elif [ -d "../mbedtls-2.28.8" ]; then
    MBEDTLS="../mbedtls-2.28.8"
fi

if [ -z "$MBEDTLS" ]; then
    echo "[PocketFox] ERROR: Cannot find mbedTLS. Build it first with:"
    echo "  ./build_pocketfox.sh mbedtls"
    echo "Or pass the path: ./build_browser.sh /path/to/mbedtls"
    exit 1
fi

# Determine include and lib paths
if [ -d "$MBEDTLS/include/mbedtls" ]; then
    MBEDTLS_INC="$MBEDTLS/include"
elif [ -d "$MBEDTLS/library" ] && [ -d "$MBEDTLS/include" ]; then
    MBEDTLS_INC="$MBEDTLS/include"
else
    echo "[PocketFox] ERROR: Cannot find mbedTLS headers at $MBEDTLS"
    exit 1
fi

if [ -f "$MBEDTLS/lib/libmbedtls.a" ]; then
    MBEDTLS_LIB="$MBEDTLS/lib"
elif [ -f "$MBEDTLS/library/libmbedtls.a" ]; then
    MBEDTLS_LIB="$MBEDTLS/library"
else
    echo "[PocketFox] ERROR: Cannot find libmbedtls.a at $MBEDTLS"
    exit 1
fi

echo "=== PocketFox Browser Build ==="
echo "Compiler:  $CC"
echo "mbedTLS:   $MBEDTLS"
echo "  Include: $MBEDTLS_INC"
echo "  Lib:     $MBEDTLS_LIB"
echo ""

# Detect architecture
ARCH_FLAGS=""
MACHINE=$(uname -m 2>/dev/null || echo "unknown")
case "$MACHINE" in
    Power*|ppc*)
        ARCH_FLAGS="-arch ppc -mcpu=7450"
        echo "Target: PowerPC"
        ;;
    x86_64|i386)
        echo "Target: x86 (cross-compile or testing)"
        ;;
    *)
        echo "Target: $MACHINE"
        ;;
esac

CFLAGS="-O2 $ARCH_FLAGS -DHAVE_MBEDTLS -I$MBEDTLS_INC"
LDFLAGS="-L$MBEDTLS_LIB -lmbedtls -lmbedx509 -lmbedcrypto -framework Cocoa"

echo "Compiling pocketfox_ssl_tiger.c..."
$CC $CFLAGS -std=c99 -c pocketfox_ssl_tiger.c -o pocketfox_ssl_tiger.o

echo "Compiling pocketfox_http.c..."
$CC $CFLAGS -std=c99 -c pocketfox_http.c -o pocketfox_http.o

echo "Compiling pocketfox_tiger_gui.m..."
$CC $CFLAGS -c pocketfox_tiger_gui.m -o pocketfox_tiger_gui.o

echo "Linking PocketFox..."
$CC $ARCH_FLAGS -o PocketFox \
    pocketfox_tiger_gui.o pocketfox_http.o pocketfox_ssl_tiger.o \
    $LDFLAGS

echo ""
SIZE=$(wc -c < PocketFox | tr -d ' ')
echo "Built: PocketFox ($SIZE bytes)"
file PocketFox 2>/dev/null || true

echo ""
echo "Run with: ./PocketFox"
echo "Done."
