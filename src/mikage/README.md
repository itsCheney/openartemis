# ArtemisRuntime.framework（Mikage 用）

把 openArtemis 编成 iOS framework，作为 [Mikage](https://github.com/itsCheney/Mikage-Next)
的一个引擎运行。Mikage 的构建脚本 `scripts/build-artemis-ios.sh` 会调用这里。

- `src/app/main.cpp` 原样编入，只多一个 `OA_HOST_LIBRARY=1`：SDL 不再提供
  `main()`，Esc（Mikage 的“返回”按钮）改成右键，不再退出游戏。
- 四个 SDL 回调交给 Mikage 的共享 SDL host（`Engine/MikageSDLHost`，
  通过 `MIKAGE_SDL_HOST_DIR` 编入）。事件泵、音频会话、前后台和会话结束后的
  SDL 复位都由它负责，与 KiriKiri 一样。
- `MikagePlatform.cpp` 是 iOS 的 `oa::plat`：窗口加入 Mikage 给的
  UIWindowScene，三指触摸打开 Mikage 菜单，帧节奏由 Mikage 的 display link
  决定（帧尾不 sleep），回到前台时重置 tick 时钟。
- `MikageArtemisRuntime.h` 是对外的 C 接口（`MikageArtemis*`），其余符号全部隐藏。

依赖：

- SDL3：不走 vcpkg，链接 Mikage 打包的共享 `SDL3.framework`（`OA_SDL3_FRAMEWORK`）。
- FFmpeg：不走 vcpkg，用 Mikage 为 KiriKiri 装好的静态库（`OA_FFMPEG_ROOT`）。
- 其余（freetype、libpng、libjpeg-turbo、libvorbis、libtheora、physfs）由 vcpkg
  按 `cmake/triplets/arm64-ios*.cmake` 安装。这两个 triplet 与 KiriKiri 的相同，
  可以共用 vcpkg 二进制缓存。

```sh
cmake -S . -B build/ios-device -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=arm64-ios -DVCPKG_OVERLAY_TRIPLETS=cmake/triplets \
  -DOA_MIKAGE_RUNTIME=ON -DOA_BUILD_TOOLS=OFF -DOA_ENABLE_TESTS=OFF \
  -DOA_SDL3_FRAMEWORK=/path/to/ios-arm64/SDL3.framework \
  -DOA_FFMPEG_ROOT=/path/to/arm64-ios \
  -DMIKAGE_SDL_HOST_DIR=/path/to/Mikage-Next/Engine/MikageSDLHost
cmake --build build/ios-device --config Release --target ArtemisRuntime
```
