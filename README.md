# RTSP H.264 Test Server

一个纯 C 的简易 RTSP 示例工程，包含：

- `core`：RTSP/RTP/H.264 公共能力
- `server`：RTSP 服务端推流
- `client`：RTSP 客户端拉流，并通过回调把音视频数据通知上层

默认情况下，程序内置一帧 `1024x768` 的测试图案并循环发送，同时附带一条本地生成的 `L16/8000/1` PCM 单声道音频。

## 编译

```bash
cmake -S . -B build
cmake --build build
```

如果需要 `MP4` 输入能力：

```bash
cmake -S . -B build -DRTSP_ENABLE_FFMPEG_INPUT=ON
cmake --build build
```

如果不需要，也可以显式关闭：

```bash
cmake -S . -B build -DRTSP_ENABLE_FFMPEG_INPUT=OFF
cmake --build build
```

## Docker

先在本地编译出 `server` 可执行文件：

```bash
cmake -S . -B build -DRTSP_ENABLE_FFMPEG_INPUT=ON
cmake --build build --target rtsp_h264_server
```

构建仅包含 `server` 的镜像：

```bash
docker build -t rtsp-h264-server .
```

Docker 镜像默认启用 `MP4` 输入支持，并在镜像内安装 `ffmpeg/ffprobe`。

运行示例：

```bash
docker run --rm -p 5554:5554/tcp -p 5554:5554/udp rtsp-h264-server
```

挂载本地媒体文件作为输入：

```bash
docker run --rm -p 5554:5554/tcp -p 5554:5554/udp \
  -v /path/to/media:/media \
  rtsp-h264-server --input /media/input.mp4
```

## 运行

```bash
./build/rtsp_h264_server
```

也可以直接指定一个本地 `.h264` 文件作为数据源：

```bash
./build/rtsp_h264_server --input /path/to/input.h264
```

也支持直接指定一个本地 `.mp4` 文件。服务端会在启动时用 `ffmpeg` 提取视频源，并把音频转成 `L16/8000/1` 作为 RTSP 音频轨：

```bash
./build/rtsp_h264_server --input /path/to/input.mp4
```

也兼容旧的单个位置参数写法：

```bash
./build/rtsp_h264_server /path/to/input.h264
```

`MP4` 模式依赖构建时启用 `RTSP_ENABLE_FFMPEG_INPUT`，并且系统里存在可执行的 `ffmpeg` 和 `ffprobe`。如果当前二进制不支持 `MP4` 输入，程序会直接提示缺少对应能力。

启动后会监听：

```text
rtsp://127.0.0.1:5554/live
```

## 拉流测试

推荐先用 UDP：

```bash
ffplay -rtsp_transport udp rtsp://127.0.0.1:5554/live
```

也支持 RTSP over TCP：

```bash
ffplay -rtsp_transport tcp rtsp://127.0.0.1:5554/live
```

## Client 示例

客户端示例默认使用 RTSP over TCP 拉流，并用内置的最小 `MPEG-TS/HLS` 骨架直接写出 `TS` 切片，同时同步更新 `playlist.m3u8`。视频按关键帧切片，音频会把 `L16/8000/1 PCM` 实时编码成 `AAC` 后一起复用进 `TS`：

```bash
./build/rtsp_h264_client_demo rtsp://127.0.0.1:5554/live
```

也可以显式指定输出目录：

```bash
./build/rtsp_h264_client_demo rtsp://127.0.0.1:5554/live ./segments
```

因为切片点会对齐到 `IDR` 关键帧，所以如果源流 `GOP` 比较长，实际分片时长可能会大于 `2` 秒。

如果目标平台没有 `libavcodec/libavutil/libswresample`，可以在构建时关闭客户端 AAC 支持：

```bash
cmake -S . -B build -DRTSP_ENABLE_LIBAV_AAC=OFF
cmake --build build --target rtsp_h264_client_demo
```

关闭后客户端仍会生成视频 `TS/HLS`，但收到 `PCM` 音频时只会打印一次告警并忽略，不做 `PCM -> AAC` 转换。
