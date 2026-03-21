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

## 运行

```bash
./build/rtsp_h264_server
```

也可以直接指定一个本地 `.h264` 文件作为数据源：

```bash
./build/rtsp_h264_server /path/to/input.h264
```

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

客户端示例默认使用 RTSP over TCP 拉流，并在回调中区分打印视频和音频：

```bash
./build/rtsp_h264_client_demo rtsp://127.0.0.1:5554/live
```
