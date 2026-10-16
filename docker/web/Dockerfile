# syntax=docker/dockerfile:1
#
# The web build (port/web/README.md) as a container image: nginx serving the site with the headers
# it needs, and, for a private server, the game data you mount (charts/opence-web):
#
#   docker build -f docker/web/Dockerfile --build-context site=dist/halo-web-release -t opence-web .
#   docker run --rm --read-only --tmpfs /tmp -p 8080:8080 opence-web
#
# The site is the web job's halo-web-release (.github/workflows/build.yml), given as the build
# context named "site": any folder with the site's files (index.html, halo.js, halo.wasm, ...). The
# image holds code only: no game data, ever. A private server mounts the operator's own copy of the
# maps (and Skate 3 mode's converted skate-data) read-only under /srv/game-data, and
# /opt/opence-web/web_server_data_manifest.py (tools/web_server_data_manifest.py; the chart runs it
# in an init container) writes /srv/generated/manifest.json and /srv/generated/server-data.json,
# which nginx serves at /game-data/manifest.json and /server-data.json. Without them the site is the
# plain site, with no game data: /server-data.json is a 404.
#
#   /srv/site                          the site (read-only)
#   /srv/game-data/maps                the maps folder (ui.map and the others), mounted read-only
#   /srv/game-data/skate-data/assets   Skate 3 mode's converted data, mounted read-only
#   /srv/game-data/skate-data/sounds
#   /srv/generated                     manifest.json and server-data.json (written by the script)
#   /etc/nginx/opence/conf.d           more http-level settings (*.conf), empty: the chart mounts its
#                                      realIp settings here
#   /tmp                               nginx's pid and temporary files: the only writable path
#
# nginx runs as uid/gid 101 (nginx-unprivileged's) on port 8080, so the root filesystem can be
# read-only with /tmp a tmpfs or an emptyDir.
#
# Built, tested, pushed and signed by the web-image job of .github/workflows/build.yml; the Helm
# chart workflow for the web site (.github/workflows/helm-chart-web.yml) builds it with a stub site.

FROM nginxinc/nginx-unprivileged:1.30.5-alpine@sha256:56b8272e43a0525f67aed68d1e0794a712d10e3e45a563e118e9a77c817e6f86
USER root
# Python for the manifest script (the standard library only). The image's entrypoint scripts and
# default server are not used: the configuration below is the whole of it.
RUN apk add --no-cache python3 \
 && rm -f /etc/nginx/conf.d/default.conf \
 && mkdir -p /srv/site /srv/generated /srv/game-data/maps /srv/game-data/skate-data/assets \
      /srv/game-data/skate-data/sounds /opt/opence-web /etc/nginx/opence/conf.d \
 && chown 101:101 /srv/generated
COPY docker/web/nginx.conf /etc/nginx/nginx.conf
COPY docker/web/headers.conf docker/web/game-data.conf /etc/nginx/opence/
COPY tools/web_server_data_manifest.py /opt/opence-web/web_server_data_manifest.py
COPY --from=site / /srv/site/
RUN test -f /srv/site/index.html \
 && python3 /opt/opence-web/web_server_data_manifest.py --help > /dev/null \
 && nginx -t
USER 101:101
EXPOSE 8080
# nginx's graceful stop: requests in progress finish.
STOPSIGNAL SIGQUIT
ENTRYPOINT ["nginx", "-g", "daemon off;"]
