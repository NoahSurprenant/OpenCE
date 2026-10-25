# syntax=docker/dockerfile:1
#
# The dedicated server (port/linux/README.md, "Dedicated server") as a container image:
#
#   docker build -f docker/server/Dockerfile -t opence-server .
#   docker run --rm --read-only -p 27015:27015/udp \
#     -v /path/to/halo:/data:ro --tmpfs /state:uid=10001,gid=10001 opence-server
#
# The game data is never in the image: mount the folder that contains maps/ at /data, read-only.
# Optionally, your own converted Skate 3 data (port/skate/README.md, "What you need": the
# skate-data folder, with assets/ in it) at /skate-data, read-only too: the server's bots then
# ride boards (port/skate/README.md, "Bots on boards"); without it they stay on foot. It is never
# in the image either.
# /state is the only folder the server writes (config.toml, the save root and its map cache), so
# the root filesystem can be read-only. The executable is 32-bit x86 and runs on amd64 hosts.
#
# A release build (assertions are not checked, as in the retail game and the builds that players
# download) with Skate 3 mode's engine (--skate, built by a pinned Rust toolchain): a server draws
# no board, and with no Skate 3 data it still relays skaters' states, applies their bounds and
# decides their hits (tools/test_linux_port.py's test_dedicated_server_skaters); with the data its
# bots skate too, each in a skate session of its own (tools/test_skating_bots.py). No link-time or
# profile-guided optimisation: the server sleeps most of each frame, and Debian's clang is older
# than the profiles need.
#
# --portable (code for any x86-64 processor, as the cluster's nodes differ) also builds against
# Debian 11's glibc and builds its own SDL 3 (tools/linux_sysroot.py), hence CMake, pkgconf and
# wayland-scanner here; the image runs the executable with Debian's 32-bit SDL 3 below.
#
# Built, pushed and signed by .github/workflows/server-image.yml.

FROM debian:trixie@sha256:913f6706df59a68922d1dd08f78c2476560a8d367897200a6005b00e5f67c2d5 AS build
RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates python3 ninja-build clang lld \
      gcc-multilib cmake pkgconf libwayland-bin curl \
 && rm -rf /var/lib/apt/lists/*
# The skate engine's Rust (port/skate): rustup's installer, checked against its SHA-256, a pinned
# toolchain, and the game's 32-bit target.
ARG RUSTUP_VERSION=1.28.2
ARG RUSTUP_SHA256=20a06e644b0d9bd2fbdbfd52d42540bdde820ea7df86e92e533c073da0cdd43c
ARG RUST_TOOLCHAIN=1.99.0
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo PATH=/opt/cargo/bin:$PATH
RUN curl -fsSL -o /tmp/rustup-init \
      "https://static.rust-lang.org/rustup/archive/${RUSTUP_VERSION}/x86_64-unknown-linux-gnu/rustup-init" \
 && echo "${RUSTUP_SHA256}  /tmp/rustup-init" | sha256sum -c - \
 && chmod +x /tmp/rustup-init \
 && /tmp/rustup-init -y --no-modify-path --profile minimal --default-toolchain "${RUST_TOOLCHAIN}" \
      --target i686-unknown-linux-gnu \
 && rm /tmp/rustup-init
WORKDIR /src
COPY . .
# The run number of a build of main (the workflow passes it), as tools/ci_build.py gives it: the
# server logs it. 0 for any other build.
ARG HALO_BUILD_NUMBER=0
# (the skate engine for the processors the server runs on: x86-64-v3, AVX2 and FMA, which
# makes each skater's step several times quicker - the same results bit for bit)
ARG CONFIGURE_FLAGS="--release --lto=off --pgo=off --portable --skate --skate-cpu=x86-64-v3"
# (the skate engine's debug information, which cargo's release profile keeps, stripped: about 120 MB;
# the symbols stay, for a crash's backtrace)
RUN HALO_BUILD_NUMBER="${HALO_BUILD_NUMBER}" python3 configure.py ${CONFIGURE_FLAGS} \
 && ninja linux \
 && strip --strip-debug build/linux/halo \
 && test -x build/linux/halo && test -f build/linux/brokers.txt \
 && test -f build/linux/libSDL3.so.0

# The stream (port/linux/README.md, "Streaming"; docker/stream/stream.sh): the same build as a client
# in stream mode, watching a server's game with the auto-director and encoded by ffmpeg to HLS. Built
# with --target stream (the default target is the server, below) and published by
# .github/workflows/stream-image.yml:
#
#   docker build -f docker/server/Dockerfile --target stream -t opence-stream .
#   docker run --rm --read-only --tmpfs /state:uid=10001,gid=10001 --tmpfs /tmp:uid=10001,gid=10001 \
#     --tmpfs /hls:uid=10001,gid=10001 -v /path/to/halo:/data:ro -e STREAM_SERVER=<server> opence-stream
#
# As the server, no game data in the image: the maps at /data, read-only (and optionally the Skate 3
# data at /skate-data, to draw the skaters' boards). /state (settings, the save root and its map
# cache), /tmp (the two FIFOs) and /hls (the playlist and its segments, which a web server beside it
# serves) are the folders it writes. It draws with OpenGL on SDL's offscreen driver through EGL
# (the portable build's own SDL 3, which has the offscreen driver with EGL, beside the executable):
# Mesa's surfaceless EGL platform opens the first DRM render node it finds and otherwise falls back
# to software - so with no /dev/dri (most of this chart's users: stream.gpu is off by default) it is
# llvmpipe (32-bit, as the game is), the same as before, and with /dev/dri/renderD128 passed in
# (stream.gpu, generic-device-plugin's squat.ai/dri) it is radeonsi, the host iGPU. libegl-mesa0:i386
# already pulls in mesa-libgallium:i386, which both drivers are part of - nothing extra to install
# for rendering. ffmpeg is the 64-bit one; mesa-va-drivers (also 64-bit, for ffmpeg, not the game)
# adds VAAPI H.264 encode on the same iGPU through /dev/dri/renderD128, used when stream.sh finds
# the device - encoding falls back to libx264 otherwise. The game logs "OpenGL ... on ..." and
# stream.sh logs which encoder it chose, so either path is visible in the pod's logs.
FROM debian:trixie-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f AS stream
RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends libc6:i386 libgcc-s1:i386 libegl1:i386 libegl-mesa0:i386 \
      libgl1:i386 libopengl0:i386 libgl1-mesa-dri:i386 libgbm1:i386 ffmpeg mesa-va-drivers tini \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd -g 10001 opence \
 && useradd -u 10001 -g 10001 -d /state -M -s /usr/sbin/nologin opence \
 && mkdir -p /data /skate-data /state /hls \
 && chown 10001:10001 /state /hls
COPY --from=build /src/build/linux/halo /src/build/linux/brokers.txt /src/build/linux/libSDL3.so.0 /opt/opence/
COPY docker/stream/stream.sh /opt/opence/stream.sh
RUN ln -s /state/config.toml /opt/opence/config.toml
# LP_NUM_THREADS: llvmpipe's threads (else one for each of the machine's CPUs): the CPUs the
# container may use; set it to its limit.
# The game's log and the custom_maps folder in the save root too: a client's default for both is
# the data root, which is read-only here (a dedicated server already keeps them in its save root).
ENV HALO_DATA_ROOT=/data \
    HALO_SKATE_ASSETS=/skate-data/assets \
    HALO_SAVE_ROOT=/state/saves \
    HALO_LOG_ROOT=/state/saves \
    HALO_CUSTOM_MAPS_ROOT=/state/saves/custom_maps \
    HALO_NET_ONLINE=false \
    HALO_UPDATE_ANSWER=never \
    LP_NUM_THREADS=3 \
    HOME=/state
USER 10001:10001
WORKDIR /state
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/bin/tini", "--", "/opt/opence/stream.sh"]

FROM debian:trixie-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f
# SDL 3's 32-bit library (the server uses its dummy video and audio drivers; the package also
# pulls in X11 and ALSA libraries, which it never opens), and the 32-bit C runtime.
RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends libc6:i386 libgcc-s1:i386 libsdl3-0:i386 \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd -g 10001 opence \
 && useradd -u 10001 -g 10001 -d /state -M -s /usr/sbin/nologin opence \
 && mkdir -p /data /skate-data /state \
 && chown 10001:10001 /state
COPY --from=build /src/build/linux/halo /src/build/linux/brokers.txt /opt/opence/
# config.toml is read and written next to the executable: a link into the writable /state. The
# server writes the defaults through it at its first start.
RUN ln -s /state/config.toml /opt/opence/config.toml
ENV HALO_DEDICATED=1 \
    HALO_DATA_ROOT=/data \
    HALO_SKATE_ASSETS=/skate-data/assets \
    HALO_SAVE_ROOT=/state/saves \
    HALO_NET_TUNNEL_PORT=27015 \
    HOME=/state
VOLUME ["/state"]
# Internet play's tunnel (network.tunnel_port). System link's 5150 and 5151 are for a local
# network only.
EXPOSE 27015/udp
USER 10001:10001
WORKDIR /state
# SIGTERM ends the game in progress, tells the players and exits 0 in about 2 seconds.
STOPSIGNAL SIGTERM
ENTRYPOINT ["/opt/opence/halo"]
