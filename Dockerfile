FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

# Build tools + Python deps
RUN apt-get update && apt-get install -y \
        gcc \
        python3 \
        python3-pip \
        python3-requests \
        wget \
        xz-utils \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Install latest static ffmpeg from johnvansickle.com (avoids the Ubuntu 24.04 segfault)
RUN set -eux; \
    wget -q https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz \
         -O /tmp/ffmpeg.tar.xz; \
    tar -xf /tmp/ffmpeg.tar.xz -C /tmp; \
    mv /tmp/ffmpeg-*-amd64-static/ffmpeg  /usr/local/bin/ffmpeg; \
    mv /tmp/ffmpeg-*-amd64-static/ffprobe /usr/local/bin/ffprobe; \
    rm -rf /tmp/ffmpeg*; \
    ffmpeg -version | head -1

# Install MediaMTX (RTSP server) so VLC can pull the stream as a client.
# ffmpeg publishes to it locally; VLC connects to rtsp://<host>:8554/owlet
RUN set -eux; \
    MTX_VER="$(wget -qO- https://api.github.com/repos/bluenviron/mediamtx/releases/latest \
                | grep -oP '\"tag_name\": \"\K[^\"]+')"; \
    wget -q "https://github.com/bluenviron/mediamtx/releases/download/${MTX_VER}/mediamtx_${MTX_VER}_linux_amd64.tar.gz" \
         -O /tmp/mediamtx.tar.gz; \
    tar -xf /tmp/mediamtx.tar.gz -C /tmp; \
    mv /tmp/mediamtx /usr/local/bin/mediamtx; \
    mv /tmp/mediamtx.yml /etc/mediamtx.yml; \
    rm -rf /tmp/mediamtx*; \
    echo "Installed MediaMTX ${MTX_VER}"

WORKDIR /app

COPY . .

# Build binaries inside the container so they link against the container's glibc
RUN gcc -O2 -o owlet_stream owlet_stream.c -ldl -lpthread
RUN gcc -shared -fPIC -o bionic_compat.so bionic_compat.c

ENV TUTK_LIB_DIR=/app/tutk_libs

# RTP default port (UDP) and RTSP server port (TCP, served by MediaMTX)
EXPOSE 5004/udp
EXPOSE 8554/tcp

CMD ["/bin/bash"]
