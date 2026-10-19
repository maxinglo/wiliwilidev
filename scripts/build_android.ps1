param(
    [string]$ToolchainRoot = (Join-Path $PSScriptRoot '..\..\toolchain'),
    [string]$SdkRoot = $env:ANDROID_HOME,
    [string]$JdkRoot = $env:JAVA_HOME,
    [ValidateSet('arm64-v8a', 'armeabi-v7a', 'x86_64')]
    [string]$Abi = 'arm64-v8a',
    [int]$Jobs = 6,
    [switch]$NativeOnly
)
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$ToolchainRoot = [IO.Path]::GetFullPath($ToolchainRoot)
$suffix = @{ 'arm64-v8a' = 'arm64'; 'armeabi-v7a' = 'armv7'; 'x86_64' = 'x64' }[$Abi]
$ndkRoot = Join-Path $ToolchainRoot 'android-ndk-r27c'
$nativeToolchain = Join-Path $ndkRoot 'build\cmake\android.toolchain.cmake'
$dependencyPrefix = Join-Path $ToolchainRoot "android-deps\$Abi"
$mpvRoot = Join-Path $ToolchainRoot 'mpv-android'
$hostBuild = Join-Path $repositoryRoot 'build-host\libromfs'
$generator = Join-Path $hostBuild 'Release\libromfs-generator.exe'
$nativeBuild = Join-Path $repositoryRoot "build-android-$suffix"

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

foreach ($requiredPath in @(
    $nativeToolchain,
    (Join-Path $mpvRoot 'include\mpv\client.h'),
    (Join-Path $mpvRoot "lib\$Abi\libmpv.so"),
    (Join-Path $ToolchainRoot 'sources\mbedtls-3.6.7\CMakeLists.txt'),
    (Join-Path $ToolchainRoot 'sources\curl-8.21.0\CMakeLists.txt')
)) {
    if (!(Test-Path -LiteralPath $requiredPath)) {
        throw "Missing $requiredPath. Prepare the dependencies described in android/README.md."
    }
}

if (!(Test-Path -LiteralPath $generator)) {
    Invoke-Checked cmake @('-S', "$repositoryRoot/library/borealis/library/lib/extern/libromfs/generator",
        '-B', $hostBuild, '-G', 'Visual Studio 17 2022', '-A', 'x64')
    Invoke-Checked cmake @('--build', $hostBuild, '--config', 'Release', '--parallel', "$Jobs")
}

$commonOptions = @('-G', 'Ninja', "-DCMAKE_TOOLCHAIN_FILE=$nativeToolchain",
    "-DANDROID_ABI=$Abi", '-DANDROID_PLATFORM=android-23', '-DANDROID_STL=c++_shared',
    '-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON', '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_POSITION_INDEPENDENT_CODE=ON', "-DCMAKE_INSTALL_PREFIX=$dependencyPrefix")
$mbedtlsBuild = Join-Path $ToolchainRoot "build\mbedtls-$suffix"
Invoke-Checked cmake (@('-S', "$ToolchainRoot/sources/mbedtls-3.6.7", '-B', $mbedtlsBuild) + $commonOptions +
    @('-DENABLE_PROGRAMS=OFF', '-DENABLE_TESTING=OFF', '-DUSE_SHARED_MBEDTLS_LIBRARY=OFF'))
Invoke-Checked cmake @('--build', $mbedtlsBuild, '--parallel', "$Jobs")
Invoke-Checked cmake @('--install', $mbedtlsBuild)

$curlBuild = Join-Path $ToolchainRoot "build\curl-$suffix"
Invoke-Checked cmake (@('-S', "$ToolchainRoot/sources/curl-8.21.0", '-B', $curlBuild) + $commonOptions + @(
    "-DCMAKE_PREFIX_PATH=$dependencyPrefix", "-DCMAKE_FIND_ROOT_PATH=$dependencyPrefix",
    '-DBUILD_SHARED_LIBS=OFF', '-DBUILD_STATIC_LIBS=ON', '-DBUILD_CURL_EXE=OFF',
    '-DBUILD_TESTING=OFF', '-DBUILD_LIBCURL_DOCS=OFF', '-DBUILD_MISC_DOCS=OFF',
    '-DCURL_USE_MBEDTLS=ON', '-DCURL_USE_OPENSSL=OFF', '-DCURL_USE_LIBPSL=OFF',
    '-DCURL_USE_LIBSSH2=OFF', '-DCURL_USE_LIBSSH=OFF', '-DCURL_USE_GSSAPI=OFF',
    '-DCURL_USE_PKGCONFIG=OFF', '-DHTTP_ONLY=ON', '-DCURL_CA_BUNDLE=none',
    '-DCURL_CA_PATH=/system/etc/security/cacerts'))
Invoke-Checked cmake @('--build', $curlBuild, '--parallel', "$Jobs")
Invoke-Checked cmake @('--install', $curlBuild)

Invoke-Checked cmake (@('-S', "$repositoryRoot/android/app/jni", '-B', $nativeBuild) + $commonOptions + @(
    "-DMPV_ANDROID_ROOT=$mpvRoot", "-DANDROID_DEPS_PREFIX=$dependencyPrefix",
    "-DLIBROMFS_PREBUILT_GENERATOR=$generator"))
Invoke-Checked cmake @('--build', $nativeBuild, '--target', 'wiliwili', '--parallel', "$Jobs")

$libraryOutput = Join-Path $repositoryRoot "android\app\src\main\jniLibs\$Abi"
New-Item -ItemType Directory -Path $libraryOutput -Force | Out-Null
Copy-Item -Path "$mpvRoot/lib/$Abi/*.so" -Destination $libraryOutput -Exclude libplayer.so
Copy-Item -LiteralPath "$nativeBuild/SDL/libSDL2.so" -Destination $libraryOutput
Copy-Item -LiteralPath "$nativeBuild/wiliwili/libwiliwili.so" -Destination $libraryOutput
$stripTool = Join-Path $ndkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe'
Invoke-Checked $stripTool @('--strip-unneeded', "$libraryOutput/libSDL2.so", "$libraryOutput/libwiliwili.so")
if ($NativeOnly) { return }

if (!$SdkRoot -or !$JdkRoot) { throw 'Set ANDROID_HOME and JAVA_HOME, or pass -SdkRoot and -JdkRoot (JDK 21).' }
$env:JAVA_HOME = $JdkRoot
$sdkProperty = 'sdk.dir=' + [IO.Path]::GetFullPath($SdkRoot).Replace('\', '/').Replace(':', '\:')
Set-Content -LiteralPath "$repositoryRoot/android/local.properties" -Encoding ascii -Value $sdkProperty
Push-Location "$repositoryRoot/android"
try { Invoke-Checked '.\gradlew.bat' @(':app:assembleDebug', '--console=plain', "-PandroidAbis=$Abi") }
finally { Pop-Location }
$artifactDirectory = Join-Path $repositoryRoot '..\artifacts'
New-Item -ItemType Directory -Path $artifactDirectory -Force | Out-Null
$apkPath = Join-Path $artifactDirectory "wiliwili-1.6.0-android-hdr10-$Abi.apk"
Copy-Item -LiteralPath "$repositoryRoot/android/app/build/outputs/apk/debug/app-debug.apk" -Destination $apkPath
Get-FileHash -LiteralPath $apkPath -Algorithm SHA256
