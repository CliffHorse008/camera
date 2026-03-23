# RTSP H.264 Test Server

一个纯 C 的简易 RTSP 示例工程，包含：

- `core`：RTSP/RTP/H.264 公共能力
- `server`：RTSP 服务端推流
- `client`：RTSP 客户端拉流，并通过回调把音视频数据通知上层

当前服务端必须传入一个外部 `.mp4` 文件作为输入，并依赖系统中的 `ffmpeg/ffprobe` 提取音视频。

## 编译

```bash
cmake -S . -B build
cmake --build build
```

## Docker

先在本地编译出 `server` 可执行文件：

```bash
cmake -S . -B build
cmake --build build --target rtsp_h264_server
```

构建仅包含 `server` 的镜像：

```bash
docker build -t rtsp-h264-server .
```

Docker 镜像内安装 `ffmpeg/ffprobe`，用于服务端加载 `.mp4` 输入。

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
./build/rtsp_h264_server --input /path/to/input.mp4
```

也兼容旧的单个位置参数写法：

```bash
./build/rtsp_h264_server /path/to/input.mp4
```

服务端会在启动时用 `ffmpeg` 提取视频源，并把音频转成 `AAC` 作为 RTSP 音频轨。如果系统缺少 `ffmpeg` 或 `ffprobe`，CMake 配置阶段会直接失败。

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

客户端示例默认使用 RTSP over TCP 拉流，并用内置的最小 `MPEG-TS/HLS` 骨架直接写出 `TS` 切片，同时同步更新 `playlist.m3u8`。视频按关键帧切片，音频轨为 `AAC` 时会直接一起复用进 `TS`：

```bash
./build/rtsp_h264_client_demo rtsp://127.0.0.1:5554/live
```

也可以显式指定输出目录：

```bash
./build/rtsp_h264_client_demo rtsp://127.0.0.1:5554/live ./segments
```

因为切片点会对齐到 `IDR` 关键帧，所以如果源流 `GOP` 比较长，实际分片时长可能会大于 `2` 秒。

客户端不会再做本地 `PCM -> AAC` 转码，而是直接复用 RTSP 拉下来的 `AAC` 音频。如果遇到不支持的音频格式，会打印告警并忽略音频轨。
