#!/usr/bin/env bash
# End-to-end test of the chart on a real cluster: ./run.sh
#
# Needs kubectl and helm pointed at an empty single-node cluster (k3d or k3s; they enforce
# NetworkPolicies), curl, jq and sha256sum, and runs the web image IMAGE (repository:tag; default the
# published :latest - the workflow builds it from the checkout with e2e/stub-site.sh's stand-in for
# the site, and imports it). The game data are small generated placeholder files, never real ones.
#   1. the plain site (ci/minimal-values.yaml, no gameData): in a namespace that enforces Pod
#      Security's restricted profile, the pod is admitted under the chart's security context and gets
#      ready on its probes, with no init container; in it, user 101, a read-only root filesystem, no
#      service account token. Through the Service: index.html with the cross-origin isolation
#      headers, halo.wasm as application/wasm, a Range request answered 206, /server-data.json and
#      /game-data/... a 404. The NetworkPolicy lets a pod of another namespace in and nothing out (the
#      Kubernetes API, which that other pod reaches, is unreachable from nginx's);
#   2. a private server: placeholder files written on the node (maps/, skate-data/assets and sounds,
#      a decoy beside them and skate-data/conversion.log), a hostPath PersistentVolume and a claim on
#      them, and the release upgraded with gameData.volume naming the claim. The init container
#      writes the manifest; /server-data.json names it; it lists exactly the files, and each one
#      served from its base has the listed size and SHA-256; a Range request gets 206; the decoys are
#      neither listed nor reachable (not over HTTP, not in the pod), and the data and the manifest are
#      read-only in nginx's container;
#   3. a restart: a new pod hashes the data again and gets ready.
set -euo pipefail

chart=$(cd "$(dirname "$0")/.." && pwd)
ns=opence-web-e2e
other=opence-web-e2e-other
release=opence-web
image=${IMAGE:-ghcr.io/noahsurprenant/opence-web:latest}
deploy=deploy/$release
data_dir=/var/lib/opence-web-e2e
local_port=${LOCAL_PORT:-18080}
base=http://127.0.0.1:$local_port
forward_pid=

step() { echo; echo "=== $*"; }
fail() { echo "FAIL: $*" >&2; diagnose; exit 1; }
diagnose() {
  echo "--- pods" >&2
  kubectl get pods -A -o wide >&2 || true
  for pod in $(kubectl -n "$ns" get pods -o name 2>/dev/null); do
    echo "--- $pod" >&2
    kubectl -n "$ns" describe "$pod" | sed -n '/^Events:/,$p' >&2 || true
    kubectl -n "$ns" logs "$pod" --all-containers --tail=60 >&2 || true
  done
}
stop_forward() { if [ -n "$forward_pid" ]; then kill "$forward_pid" 2>/dev/null || true; wait "$forward_pid" 2>/dev/null || true; forward_pid=; fi; }
trap stop_forward EXIT
# a port-forward to the Service (it picks a pod when it starts: started again after a rollout)
forward() {
  stop_forward
  kubectl -n "$ns" port-forward "svc/$release" "$local_port:80" >/dev/null &
  forward_pid=$!
  for _ in $(seq 1 50); do
    [ "$(curl -s -o /dev/null -w '%{http_code}' "$base/healthz")" = 200 ] && return 0
    sleep 0.2
  done
  fail "no answer through the port-forward"
}
code() { local path=$1; shift; curl -sS -o /dev/null -w '%{http_code}' "$@" "$base$path"; }
header() { curl -sS -o /dev/null -D - "${@:3}" "$base$1" | tr -d '\r' | sed -n "s/^$2: *//Ip" | head -n 1; }
expect() { [ "$2" = "$3" ] || fail "$1: got '$2', expected '$3'"; }
isolation() {
  expect "$1 COOP" "$(header "$1" cross-origin-opener-policy)" same-origin
  expect "$1 COEP" "$(header "$1" cross-origin-embedder-policy)" require-corp
  expect "$1 CORP" "$(header "$1" cross-origin-resource-policy)" same-origin
  expect "$1 nosniff" "$(header "$1" x-content-type-options)" nosniff
}
# the newest pod (an old one may still be going)
pod_name() {
  kubectl -n "$ns" get pods -l app.kubernetes.io/instance=$release --sort-by=.metadata.creationTimestamp -o name \
    | tail -n 1 | sed 's|^pod/||'
}
in_pod() { kubectl -n "$ns" exec "$pod" -c nginx -- "$@"; }
# a one-off pod in the other namespace (no NetworkPolicy there) running the image's curl; its
# status code read from the finished pod's log (kubectl run --rm -i's attach can lose or repeat it)
other_curl() {
  local name="curl-$RANDOM" phase=""
  kubectl -n "$other" run "$name" --restart=Never --image="$image" --image-pull-policy=IfNotPresent \
    --command -- curl -s -k -m 10 -o /dev/null -w '%{http_code}' "$@" >/dev/null
  for _ in $(seq 60); do
    phase=$(kubectl -n "$other" get pod "$name" -o jsonpath='{.status.phase}' 2>/dev/null || true)
    case "$phase" in Succeeded|Failed) break ;; esac
    sleep 1
  done
  kubectl -n "$other" logs "$name" 2>/dev/null || true
  kubectl -n "$other" delete pod "$name" --wait=false >/dev/null 2>&1 || true
}

step "install ($image): the plain site, no game data"
kubectl create namespace "$ns"
# (a pod is refused there unless it runs as non-root, without capabilities or privilege escalation,
# under the runtime's seccomp profile, with no hostPath)
kubectl label namespace "$ns" pod-security.kubernetes.io/enforce=restricted pod-security.kubernetes.io/enforce-version=latest
kubectl create namespace "$other"
helm install "$release" "$chart" -n "$ns" -f "$chart/ci/minimal-values.yaml" \
  --set "image.repository=${image%:*}" --set "image.tag=${image##*:}" --set image.pullPolicy=IfNotPresent
kubectl -n "$ns" rollout status "$deploy" --timeout=180s || fail "the pod did not get ready (refused under the restricted profile?)"
pod=$(pod_name)
[ -z "$(kubectl -n "$ns" get pod "$pod" -o jsonpath='{.spec.initContainers}')" ] || fail "an init container without game data"
expect "uid" "$(in_pod id -u)" 101
if in_pod touch /srv/site/x 2>/dev/null; then fail "the root filesystem is writable"; fi
if in_pod test -e /var/run/secrets/kubernetes.io/serviceaccount/token 2>/dev/null; then fail "a service account token is mounted"; fi
forward
expect "/" "$(code /)" 200
isolation /
expect "/ Cache-Control" "$(header / cache-control)" no-cache
expect "/halo.wasm Content-Type" "$(header /halo.wasm content-type)" application/wasm
expect "/halo.wasm Range" "$(code /halo.wasm -H 'Range: bytes=0-7')" 206
expect "/server-data.json" "$(code /server-data.json)" 404
isolation /server-data.json
expect "/game-data/manifest.json" "$(code /game-data/manifest.json)" 404
expect "/game-data/maps/ui.map" "$(code /game-data/maps/ui.map)" 404
expect "/.hidden" "$(code /.hidden)" 404
echo "ready, uid 101, read-only root, no token; headers, wasm, 206 and the 404s: $pod"

step "the NetworkPolicy: in from another namespace, nothing out"
kubectl -n "$ns" get networkpolicy "$release" >/dev/null || fail "no NetworkPolicy"
expect "from another namespace" "$(other_curl "http://$release.$ns.svc/healthz")" 200
api=$(kubectl -n default get svc kubernetes -o jsonpath='{.spec.clusterIP}')
reached=$(other_curl "https://$api/version")
case "$reached" in 2??|4??) ;; *) fail "the other namespace cannot reach the Kubernetes API ($reached): the check below would prove nothing" ;; esac
if in_pod curl -sS -k -m 5 -o /dev/null "https://$api/version" 2>/dev/null; then fail "nginx's pod reached the Kubernetes API"; fi
echo "another namespace gets in; nginx's pod reaches nothing (the API answers others with $reached)"

step "a private server: placeholder game data on the node, a hostPath PersistentVolume, gameData.volume"
# The node's folder, written by a pod of the other namespace (hostPath is refused in the test's).
kubectl -n "$other" apply -f - <<EOF
apiVersion: v1
kind: Pod
metadata:
  name: write-data
spec:
  restartPolicy: Never
  containers:
    - name: write
      image: $image
      imagePullPolicy: IfNotPresent
      securityContext: { runAsUser: 0, runAsGroup: 0 }
      command:
        - sh
        - -euc
        - |
          cd /data
          rm -rf ./* ./.[!.]*
          mkdir -p maps skate-data/assets/boards skate-data/sounds
          head -c 2000000 /dev/urandom > maps/ui.map
          head -c 3000 /dev/urandom > maps/bloodgulch.map
          printf 'hidden' > maps/.hidden.map
          printf '{"board": 1}' > skate-data/assets/boards/board.json
          head -c 5000 /dev/urandom > skate-data/sounds/a.wav
          printf 'decoy' > decoy.txt
          printf 'not served' > skate-data/conversion.log
          chmod -R a+rX /data
      volumeMounts:
        - { name: data, mountPath: /data }
  volumes:
    - name: data
      hostPath: { path: $data_dir, type: DirectoryOrCreate }
EOF
kubectl -n "$other" wait --for=jsonpath='{.status.phase}'=Succeeded pod/write-data --timeout=120s \
  || { kubectl -n "$other" logs write-data >&2 || true; fail "the placeholder data were not written"; }
kubectl apply -f - <<EOF
apiVersion: v1
kind: PersistentVolume
metadata:
  name: opence-web-e2e-games
spec:
  capacity: { storage: 1Gi }
  accessModes: [ReadOnlyMany]
  persistentVolumeReclaimPolicy: Retain
  storageClassName: ""
  claimRef: { namespace: $ns, name: games }
  hostPath: { path: $data_dir, type: Directory }
---
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: games
  namespace: $ns
spec:
  accessModes: [ReadOnlyMany]
  storageClassName: ""
  volumeName: opence-web-e2e-games
  resources: { requests: { storage: 1Gi } }
EOF
helm upgrade "$release" "$chart" -n "$ns" --reuse-values \
  --set-json 'gameData.volume={"persistentVolumeClaim":{"claimName":"games","readOnly":true}}'
kubectl -n "$ns" rollout status "$deploy" --timeout=180s || fail "the pod with game data did not get ready"
pod=$(pod_name)
log=$(kubectl -n "$ns" logs "$pod" -c manifest)
echo "$log"
grep -qx 'maps: 2 files, 2003000 bytes' <<<"$log" || fail "the init container did not list the 2 maps"
grep -qx 'skate: 2 files, 5012 bytes' <<<"$log" || fail "the init container did not list the 2 skate files"
forward
expect "/server-data.json" "$(code /server-data.json)" 200
expect "/server-data.json Cache-Control" "$(header /server-data.json cache-control)" no-store
isolation /server-data.json
config=$(curl -sS "$base/server-data.json")
expect "server-data.json" "$(jq -c . <<<"$config")" '{"format":"opence-server-data","version":1,"manifest":"game-data/manifest.json"}'
manifest=$(curl -sS "$base/game-data/manifest.json")
expect "the maps listed" "$(jq -c '[.maps.files[].path]' <<<"$manifest")" '["bloodgulch.map","ui.map"]'
expect "the skate files listed" "$(jq -c '[.skate.files[].path]' <<<"$manifest")" '["assets/boards/board.json","sounds/a.wav"]'
# each file from its base (relative to the manifest's address, /game-data/), its size and hash as listed
for part in maps skate; do
  part_base=$(jq -r ".$part.base" <<<"$manifest")
  while IFS=$'\t' read -r path size hash; do
    url="$base/game-data/$part_base$path"
    got_size=$(curl -sS -o /dev/null -w '%{size_download}' "$url")
    got_hash=$(curl -sS "$url" | sha256sum | cut -d' ' -f1)
    expect "$part/$path size" "$got_size" "$size"
    expect "$part/$path sha256" "$got_hash" "$hash"
  done < <(jq -r ".$part.files[] | [.path, .size, .sha256] | @tsv" <<<"$manifest")
done
expect "ui.map Range" "$(code /game-data/maps/ui.map -H 'Range: bytes=1000-1999')" 206
isolation /game-data/maps/ui.map
expect "the decoy beside the folders" "$(code /game-data/decoy.txt)" 404
expect "skate-data/conversion.log" "$(code /game-data/skate-data/conversion.log)" 404
expect "a hidden map" "$(code /game-data/maps/.hidden.map)" 404
expect "/halo.wasm Content-Type" "$(header /halo.wasm content-type)" application/wasm
if in_pod sh -c 'ls -a /srv/game-data /srv/game-data/skate-data' | grep -qE 'decoy|conversion'; then fail "the decoys are visible in the pod"; fi
if in_pod touch /srv/game-data/maps/x 2>/dev/null; then fail "the maps are writable"; fi
if in_pod touch /srv/generated/x 2>/dev/null; then fail "the manifest's folder is writable in nginx's container"; fi
expect "uid" "$(in_pod id -u)" 101
echo "manifest and files match; 206; decoys unreachable; data read-only: $pod"

step "restart: a new pod hashes the data again and gets ready"
kubectl -n "$ns" rollout restart "$deploy"
kubectl -n "$ns" rollout status "$deploy" --timeout=180s || fail "the new pod did not get ready"
[ "$(pod_name)" != "$pod" ] || fail "no new pod"
pod=$(pod_name)
forward
expect "/server-data.json after the restart" "$(code /server-data.json)" 200
echo "new pod $pod ready"

step "uninstall"
stop_forward
helm uninstall "$release" -n "$ns" --wait
kubectl -n "$ns" delete pvc games --wait=false
kubectl delete pv opence-web-e2e-games --wait=false
kubectl -n "$other" delete pod write-data --wait=false
echo
echo "PASS"
