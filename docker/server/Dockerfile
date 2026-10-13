# syntax=docker/dockerfile:1
#
# The dedicated server (port/linux/README.md, "Dedicated server") as a container image:
#
#   docker build -f docker/server/Dockerfile -t opence-server .
#   docker run --rm --read-only -p 27015:27015/udp \
#     -v /path/to/halo:/data:ro --tmpfs /state:uid=10001,gid=10001 opence-server
#
# The game data is never in the image: mount the folder that contains maps/ at /data, read-only.
# /state is the only folder the server writes (config.toml, the save root and its map cache), so
# the root filesystem can be read-only. The executable is 32-bit x86 and runs on amd64 hosts.
#
# A release build (assertions are not checked, as in the retail game and the builds that players
# download) without Skate 3 mode's engine: a server draws no board, and a build without --skate
# still relays skaters' states, applies their bounds and decides their hits, with no Skate 3 data
# (tools/test_linux_port.py's test_dedicated_server_skaters). No link-time or
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
      gcc-multilib cmake pkgconf libwayland-bin \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# The run number of a build of main (the workflow passes it), as tools/ci_build.py gives it: the
# server logs it. 0 for any other build.
ARG HALO_BUILD_NUMBER=0
ARG CONFIGURE_FLAGS="--release --lto=off --pgo=off --portable"
RUN HALO_BUILD_NUMBER="${HALO_BUILD_NUMBER}" python3 configure.py ${CONFIGURE_FLAGS} \
 && ninja linux \
 && test -x build/linux/halo && test -f build/linux/brokers.txt

FROM debian:trixie-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f
# SDL 3's 32-bit library (the server uses its dummy video and audio drivers; the package also
# pulls in X11 and ALSA libraries, which it never opens), and the 32-bit C runtime.
RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends libc6:i386 libgcc-s1:i386 libsdl3-0:i386 \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd -g 10001 opence \
 && useradd -u 10001 -g 10001 -d /state -M -s /usr/sbin/nologin opence \
 && mkdir -p /data /state \
 && chown 10001:10001 /state
COPY --from=build /src/build/linux/halo /src/build/linux/brokers.txt /opt/opence/
# config.toml is read and written next to the executable: a link into the writable /state. The
# server writes the defaults through it at its first start.
RUN ln -s /state/config.toml /opt/opence/config.toml
ENV HALO_DEDICATED=1 \
    HALO_DATA_ROOT=/data \
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
