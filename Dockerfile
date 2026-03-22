FROM docker.1ms.run/ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive


RUN if [ -f /etc/apt/sources.list.d/ubuntu.sources ]; then \
        sed -i 's|archive.ubuntu.com/ubuntu|mirrors.aliyun.com/ubuntu|g' /etc/apt/sources.list.d/ubuntu.sources; \
        sed -i 's|security.ubuntu.com/ubuntu|mirrors.aliyun.com/ubuntu|g' /etc/apt/sources.list.d/ubuntu.sources; \
    fi

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    tzdata \
    ffmpeg \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY build/rtsp_h264_server /app/
COPY example.mp4 /app/

EXPOSE 5554/tcp 5554/udp

CMD ["/app/rtsp_h264_server", "./example.mp4"]
