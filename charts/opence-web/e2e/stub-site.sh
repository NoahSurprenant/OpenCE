#!/usr/bin/env sh
# Writes a stand-in for the web build's site into DIR: ./stub-site.sh DIR
#
# For building docker/web/Dockerfile without the web job's halo-web-release (the e2e test, a local
# check): an index.html, a halo.wasm that is only the WebAssembly header and 1 MiB of zeros (enough
# for a Range request and gzip), a version.json, a sw.js and a hidden file that must not be served.
set -eu
dir=${1:?usage: $0 DIR}
mkdir -p "$dir"
cat > "$dir/index.html" <<'EOF'
<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>OpenCE stub site</title></head>
<body><p>opence-web stub site</p></body></html>
EOF
{ printf '\000asm\001\000\000\000'; head -c 1048576 /dev/zero; } > "$dir/halo.wasm"
printf '{"build": "stub"}\n' > "$dir/version.json"
printf '// stub service worker\n' > "$dir/sw.js"
printf '{"name": "OpenCE stub"}\n' > "$dir/manifest.webmanifest"
printf 'hidden\n' > "$dir/.hidden"
