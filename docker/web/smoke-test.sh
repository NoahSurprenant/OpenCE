#!/usr/bin/env bash
# Smoke test of the web image (docker/web/Dockerfile): ./smoke-test.sh IMAGE
#
# Needs only docker (a remote daemon too: nothing is bind-mounted, and the requests are made with the
# image's own curl, inside the container). Two containers, each as the image's user on a read-only
# root filesystem with /tmp a tmpfs, no capabilities and no privilege escalation:
#   1. the plain site: index.html with the cross-origin isolation headers, halo.wasm as
#      application/wasm (gzipped when asked), a Range request answered 206, /server-data.json and
#      everything under /game-data/ a 404, hidden files a 404, /healthz;
#   2. a private server: tmpfs folders in place of the game data, holding small generated placeholder
#      files (never real game data), the manifest script run in the container, then
#      /server-data.json and /game-data/manifest.json as it wrote them, every file it lists served
#      with its size and SHA-256, a Range request on a map answered 206 and not gzipped, and a
#      symbolic link, a hidden file and anything outside the three folders a 404.
set -euo pipefail

image=${1:?usage: $0 IMAGE}
name=opence-web-smoke-$$
fail() { echo "FAIL: $*" >&2; docker logs "$name" 2>&1 | tail -n 40 >&2 || true; exit 1; }
cleanup() { docker rm -f "$name" >/dev/null 2>&1 || true; }
trap cleanup EXIT
in_web() { docker exec "$name" "$@"; }
# the status of a request (extra curl arguments after the path)
code() { local path=$1; shift; in_web curl -sS -o /dev/null -w '%{http_code}' "$@" "http://127.0.0.1:8080$path"; }
# the response headers of a GET, lower-cased names
headers() { local path=$1; shift; in_web curl -sS -o /dev/null -D - "$@" "http://127.0.0.1:8080$path" | tr -d '\r' | sed 's/^[^:]*:/\L&/'; }
header() { headers "$1" "${@:3}" | sed -n "s/^$2: *//p" | head -n 1; }
expect() { [ "$2" = "$3" ] || fail "$1: got '$2', expected '$3'"; }
start() {
  cleanup
  docker run -d --name "$name" --read-only --cap-drop ALL --security-opt no-new-privileges \
    --tmpfs /tmp "$@" "$image" >/dev/null
  for _ in $(seq 1 50); do
    [ "$(code /healthz 2>/dev/null)" = 200 ] && return 0
    sleep 0.2
  done
  fail "nginx did not answer /healthz"
}
isolation() {
  local h; h=$(headers "$1")
  grep -qx 'cross-origin-opener-policy: same-origin' <<<"$h" || fail "$1: no Cross-Origin-Opener-Policy"
  grep -qx 'cross-origin-embedder-policy: require-corp' <<<"$h" || fail "$1: no Cross-Origin-Embedder-Policy"
  grep -qx 'cross-origin-resource-policy: same-origin' <<<"$h" || fail "$1: no Cross-Origin-Resource-Policy"
  grep -qx 'x-content-type-options: nosniff' <<<"$h" || fail "$1: no X-Content-Type-Options"
}

echo "=== the plain site ($image)"
[ "$(docker image inspect -f '{{.Config.User}}' "$image")" = 101:101 ] || fail "the image's user is not 101:101"
start
expect "uid" "$(in_web id -u)" 101
expect "/" "$(code /)" 200
isolation /
expect "/ Cache-Control" "$(header / cache-control)" no-cache
expect "/ Server" "$(header / server)" nginx
[ -n "$(header /index.html etag)" ] || fail "index.html has no ETag"
expect "/halo.wasm Content-Type" "$(header /halo.wasm content-type)" application/wasm
isolation /halo.wasm
expect "/halo.wasm gzip" "$(header /halo.wasm content-encoding -H 'Accept-Encoding: gzip')" gzip
expect "/halo.wasm Range" "$(code /halo.wasm -H 'Range: bytes=0-7')" 206
expect "/halo.wasm first bytes" "$(in_web sh -c "curl -sS -H 'Range: bytes=0-3' http://127.0.0.1:8080/halo.wasm | od -An -c | tr -d ' '")" '\0asm'
expect "/version.json Content-Type" "$(header /version.json content-type)" "application/json; charset=utf-8"
expect "/server-data.json (no game data)" "$(code /server-data.json)" 404
isolation /server-data.json
expect "/game-data/manifest.json (no game data)" "$(code /game-data/manifest.json)" 404
expect "/game-data/anything" "$(code /game-data/anything)" 404
expect "/game-data/maps/ui.map (no game data)" "$(code /game-data/maps/ui.map)" 404
expect "/.hidden" "$(code /.hidden)" 404
expect "/missing" "$(code /missing)" 404
expect "/healthz" "$(code /healthz)" 200
if in_web touch /srv/site/x 2>/dev/null; then fail "the site is writable"; fi
echo "ok"

echo "=== a private server (placeholder data in tmpfs folders)"
data=uid=101,gid=101
start --tmpfs "/srv/game-data/maps:$data" --tmpfs "/srv/game-data/skate-data/assets:$data" \
  --tmpfs "/srv/game-data/skate-data/sounds:$data" --tmpfs "/srv/generated:$data"
in_web sh -euc '
  cd /srv/game-data
  head -c 300000 /dev/urandom > maps/ui.map
  head -c 2000 /dev/urandom > maps/bloodgulch.map
  printf "not listed" > maps/notes.txt
  printf "hidden" > maps/.hidden.map
  ln -s /etc/passwd maps/link.map
  mkdir -p skate-data/assets/boards
  printf "{\"board\": 1, \"padding\": \"%s\"}" "$(head -c 3000 /dev/zero | tr "\0" x)" > skate-data/assets/boards/board.json
  head -c 5000 /dev/urandom > skate-data/sounds/a.wav'
in_web python3 /opt/opence-web/web_server_data_manifest.py --maps /srv/game-data/maps \
  --skate-data /srv/game-data/skate-data --output /srv/generated/manifest.json \
  --config /srv/generated/server-data.json --manifest-url game-data/manifest.json
expect "/server-data.json" "$(code /server-data.json)" 200
expect "/server-data.json Cache-Control" "$(header /server-data.json cache-control)" no-store
isolation /server-data.json
expect "server-data.json" "$(in_web sh -c 'curl -sS http://127.0.0.1:8080/server-data.json | python3 -c "import json, sys; print(json.load(sys.stdin)[\"manifest\"])"')" game-data/manifest.json
expect "/game-data/manifest.json Cache-Control" "$(header /game-data/manifest.json cache-control)" no-store
# every listed file, fetched from the manifest's bases, has the listed size and hash
in_web python3 - <<'EOF' || fail "the manifest and the served files differ"
import hashlib, json, urllib.parse, urllib.request
page = "http://127.0.0.1:8080/"
manifest_url = urllib.parse.urljoin(page, "game-data/manifest.json")
manifest = json.load(urllib.request.urlopen(manifest_url))
listed = {part: sorted(item["path"] for item in manifest[part]["files"]) for part in ("maps", "skate")}
assert listed == {"maps": ["bloodgulch.map", "ui.map"],
                  "skate": ["assets/boards/board.json", "sounds/a.wav"]}, listed
for part in ("maps", "skate"):
    base = urllib.parse.urljoin(manifest_url, manifest[part]["base"])
    for item in manifest[part]["files"]:
        body = urllib.request.urlopen(urllib.parse.urljoin(base, item["path"])).read()
        assert len(body) == item["size"], item
        assert hashlib.sha256(body).hexdigest() == item["sha256"], item
print("the manifest's files: served, sizes and hashes as listed")
EOF
expect "/game-data/maps/ui.map Range" "$(code /game-data/maps/ui.map -H 'Range: bytes=100-199' -H 'Accept-Encoding: gzip')" 206
isolation /game-data/maps/ui.map
expect "/game-data/maps/ui.map Cache-Control" "$(header /game-data/maps/ui.map cache-control)" no-cache
expect "board.json not gzipped" "$(header /game-data/skate-data/assets/boards/board.json content-encoding -H 'Accept-Encoding: gzip')" ""
expect "/game-data/skate-data/sounds/a.wav" "$(code /game-data/skate-data/sounds/a.wav)" 200
expect "a symbolic link" "$(code /game-data/maps/link.map)" 404
expect "a hidden file" "$(code /game-data/maps/.hidden.map)" 404
expect "a folder" "$(code /game-data/maps/)" 404
expect "outside the folders" "$(code /game-data/skate-data/)" 404
expect "a path out" "$(code '/game-data/maps/../../etc/passwd' --path-as-is)" 404
echo "ok"
echo
echo "PASS"
