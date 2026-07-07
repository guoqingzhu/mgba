$cmake = "C:\Program Files\CMake\bin\cmake.exe"
$ndk = "C:/Users/zhuguoqing/AppData/Local/Android/Sdk/ndk/28.2.13676358/build/cmake/android.toolchain.cmake"
Set-Location "C:\Code\mgba"
& $cmake -B build-android-x86_64 -S . `
  -DCMAKE_TOOLCHAIN_FILE=$ndk `
  -DANDROID_ABI=x86_64 `
  -DANDROID_PLATFORM=android-21 `
  -DCMAKE_BUILD_TYPE=Release `
  -DSKIP_LIBRARY=OFF `
  -DBUILD_LIBRETRO=OFF `
  -DBUILD_SHARED=ON `
  -DBUILD_QT=OFF `
  -DBUILD_SDL=OFF `
  -DM_CORE_GBA=ON `
  -DUSE_ZLIB=OFF `
  -DUSE_PNG=OFF `
  -DUSE_MINIZIP=OFF `
  -DUSE_LZMA=OFF `
  -DUSE_SQLITE3=OFF `
  -DUSE_ELF=OFF `
  -DUSE_LUA=OFF `
  -DUSE_FFMPEG=OFF `
  -DUSE_FREETYPE=OFF `
  -DUSE_DISCORD_RPC=OFF `
  -DUSE_EDITLINE=OFF `
  -DENABLE_DEBUGGERS=OFF `
  -DENABLE_GDB_STUB=OFF `
  -DENABLE_SCRIPTING=OFF
& $cmake --build build-android-x86_64 --config Release -j8
Copy-Item "build-android-x86_64\libmgba.so" "C:\Code\gba\android\app\src\main\jniLibs\x86_64\libmgba.so" -Force
Write-Host "DONE"
