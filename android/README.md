# Android development APK

This port runs the wiliwili C++ application through borealis and SDL2. It targets
Android 6.0 (API 23) and newer. The default build is `arm64-v8a`, uses OpenGL ES,
enables MediaCodec copy decoding, and stores settings in the application's private
files directory. It also exposes an Android TV launcher entry.

Native code is built with Release optimizations. `assembleDebug` signs the APK
with the local Android debug key so it can be installed directly. This is a
development build, not a Google Play release.

## Build on Windows

Install CMake, Ninja, Visual Studio 2022 C++ tools, JDK 21, and Android SDK platform
36/build-tools 36.0.0. Initialize the repository's submodules first.

The dependencies prepared for this workspace are in `../../toolchain` relative to
this directory:

| Directory | Dependency |
| --- | --- |
| `android-ndk-r27c` | Google NDK 27.2.12479018 |
| `sources/mbedtls-3.6.7` | mbedTLS 3.6.7 source release |
| `sources/curl-8.21.0` | curl 8.21.0 source release |
| `mpv-android/include/mpv` | mpv v0.40.0 public headers, client API 2 |
| `mpv-android/lib/arm64-v8a` | Native libraries from mpv-android 2026-09-17 |

From the repository root:

```powershell
.\scripts\build_android.ps1 `
  -SdkRoot 'C:\Program Files (x86)\Android\android-sdk' `
  -JdkRoot 'C:\Program Files\Android\openjdk\jdk-21.0.8'
```

Pass `-ToolchainRoot` for a different dependency location. The script builds the
host resource packer, mbedTLS, curl, SDL2, borealis, and wiliwili, then packages
and signs the APK. The APK is copied to `../artifacts` beside this repository.
`-NativeOnly` skips Gradle packaging. `-Abi x86_64` or `-Abi armeabi-v7a` additionally
requires the corresponding mpv libraries under `mpv-android/lib/<abi>`.

The native code uses flexible 16 KB page alignment. The packaged C++ runtime is
the one supplied with mpv-android to match its FFmpeg/mpv binaries. No mpv-android
Java application code or `libplayer.so` is included. The public mpv client API is
ABI compatible within major version 2.

OpenCC conversion and WebP decoding are disabled, as in other embedded builds;
image requests use the application's JPEG fallback. TLS verification stays enabled.
Android's default X509 trust manager exports its trusted public certificates to a
private PEM bundle on startup, which is passed to mpv for video and audio streams.
The application's HTTP client uses Android's system certificate directory.

Android 13+ system back gestures and legacy back keys are dispatched to borealis
navigation, so closing a player, dialog, or settings page returns within the app.

## Dependency sources and checksums

- NDK: <https://dl.google.com/android/repository/android-ndk-r27c-windows.zip>
  (SHA-1 `ac5f7762764b1f15341094e148ad4f847d050c38`, verified against Google's repository metadata).
- mbedTLS: <https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7>
  (`mbedtls-3.6.7.tar.bz2`, SHA-256
  `a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`).
- curl: <https://github.com/curl/curl/releases/tag/curl-8_21_0>
  (`curl-8.21.0.tar.xz`, SHA-256
  `aa1b66a70eace83dc624508745646c08ae561de512ab403adffb93ac87fc72e6`).
- mpv headers: <https://github.com/mpv-player/mpv/tree/v0.40.0/include/mpv>,
  tag commit `e48ac7ce08462f5e33af6ef9deeac6fa87eef01e`.
- mpv/FFmpeg binaries: <https://github.com/mpv-android/mpv-android/releases/tag/2026-09-17>,
  `app-default-arm64-v8a-release.apk`, SHA-256
  `0b0e0a534dd6b8d2b117f06952e0600064ad97702e48bff02543e3e32107803d`.
  Extract only `lib/arm64-v8a/*.so`, excluding `libplayer.so`.
  The release page links the matching dependency revisions and source projects.

wiliwili remains GPL-3.0; borealis/SDL, curl, mbedTLS, mpv, and FFmpeg retain their
respective licenses. Dependency source archives and the mpv header source checkout
are retained locally under `toolchain` for inspection.
