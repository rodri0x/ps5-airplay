# FFmpeg

`lib/libavcodec.a` and `lib/libavutil.a` are FFmpeg 6.0
(<https://ffmpeg.org/releases/ffmpeg-6.0.tar.xz>) built for the PS5 with ps5-payload-sdk,
restricted to the H.264 decoder and parser. LGPL 2.1 or later (see `COPYING.LGPLv2.1`).

To rebuild, from a build directory inside the FFmpeg source tree:

```sh
SDK=/opt/ps5-payload-sdk
../configure --enable-cross-compile --target-os=linux --arch=x86_64 \
  --cc=$SDK/bin/prospero-clang --cxx=$SDK/bin/prospero-clang++ \
  --ar=$SDK/bin/prospero-ar --ranlib=$SDK/bin/prospero-ranlib \
  --nm=$SDK/bin/prospero-nm --strip=$SDK/bin/prospero-strip \
  --extra-cflags="-I$SDK/target/include -O2 -fPIC -fno-stack-protector" \
  --disable-everything --enable-decoder=h264 --enable-parser=h264 \
  --disable-programs --disable-doc --disable-asm --disable-x86asm \
  --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale \
  --disable-swresample --disable-network --disable-pthreads --disable-debug \
  --disable-stripping --disable-iconv --disable-zlib --disable-bzlib \
  --disable-lzma --disable-protocols --disable-bsfs --disable-small
make -j8 libavutil/libavutil.a libavcodec/libavcodec.a
```
