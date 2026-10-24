#!/usr/bin/env bash
# Copia as .so do build CMake Android para app/src/main/jniLibs/<abi>/,
# tirando os simbolos de debug (a libAlice.so de debug passa de 250 MB).
#
# Uso: android/collect-native-libs.sh [pasta-do-build] [abi]
#   padrao: out/build/android-arm64 arm64-v8a
# Precisa da variavel ANDROID_NDK (para o llvm-strip).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/out/build/android-arm64}"
ABI="${2:-arm64-v8a}"
DEST="$ROOT/android/app/src/main/jniLibs/$ABI"
STRIP="$(ls "${ANDROID_NDK:?defina ANDROID_NDK}"/toolchains/llvm/prebuilt/*/bin/llvm-strip | head -n1)"

rm -rf "$DEST"
mkdir -p "$DEST"

cp "$BUILD_DIR/libAlice.so" "$DEST/"
# libtbb (libtbb.so no release, libtbb_debug.so no debug) -- a libAlice.so depende dela
find "$BUILD_DIR" -path '*CMakeFiles*' -prune -o -name 'libtbb*.so' -print -exec cp {} "$DEST/" \;

for lib in "$DEST"/*.so; do
	"$STRIP" --strip-unneeded "$lib"
done

echo "Bibliotecas em $DEST:"
ls -la "$DEST"
