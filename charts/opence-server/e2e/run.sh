#!/usr/bin/env bash
# End-to-end test of the chart on a real cluster: ./run.sh [minimal]
#
# Needs kubectl and helm pointed at an empty cluster (k3d or k3s; they enforce NetworkPolicies), and
# runs the server image IMAGE (repository:tag; default the published :latest - the workflow builds it
# from the checkout and imports it). There are no maps in CI (they are never published), and the
# server needs real ones (ui.map and the maps it hosts, which it loads), so the release runs with
# ci/minimal-values.yaml's emptyDir in their place and the server refuses to start. That is the first
# check; the second puts a stand-in for the server in the chart's pod to check the rest:
#   1. no maps: in a namespace that enforces Pod Security's restricted profile, the pod is admitted
#      under the chart's security context; the server writes its settings to /state, then exits with
#      status 2 at once, saying which maps it looked for where, before its network starts; the
#      kubelet restarts it into CrashLoopBackOff, and the probes never pass: the pod is never ready
#      and the Service has no endpoint;
#   2. the pod's hardening and its probes: the Deployment patched so that its container sleeps in
#      place of the server (the chart's image, user, security context and volumes still), beside a
#      sidecar that binds the tunnel port as the server does. The probes pass (the bound port in the
#      pod's /proc/net/udp) and the pod gets ready; in it, user 10001, a read-only root filesystem,
#      no service account token, /state writable and /data read-only;
#   3. the Service (with the ready pod its endpoint) and the NetworkPolicy exist as rendered;
#   4. a restart: the old pod goes and a new one gets ready (Recreate: never two at once).
# Not here: the server running, and its clean stop on SIGTERM (exit 0 in about 2 s, the players
# told): tools/test_linux_port.py's test_dedicated_server checks them with maps, on the owner's
# runner, and test_dedicated_server_without_maps the refusal.
set -euo pipefail

scenario=${1:-minimal}
case "$scenario" in minimal) ;; *) echo "usage: $0 [minimal]" >&2; exit 2 ;; esac
chart=$(cd "$(dirname "$0")/.." && pwd)
ns=opence-e2e
release=opence-server
image=${IMAGE:-ghcr.io/noahsurprenant/opence-server:latest}
# the sidecar that holds the tunnel port in step 2 (Python's official image, pinned)
port_image=${PORT_IMAGE:-python:3.13-alpine@sha256:2d9aefe2fef018a7eb2c13064c89c71929800fd2e5dccdbf52ea5da5bb8d929a}
deploy=deploy/$release
expected="server: no usable maps in /data/maps (looked for ui.map, bloodgulch.map); exiting"

step() { echo; echo "=== $*"; }
fail() { echo "FAIL: $*" >&2; diagnose; exit 1; }
diagnose() {
  echo "--- pods" >&2
  kubectl -n "$ns" get pods -o wide >&2 || true
  for pod in $(kubectl -n "$ns" get pods -o name 2>/dev/null); do
    echo "--- $pod" >&2
    kubectl -n "$ns" describe "$pod" | sed -n '/^Events:/,$p' >&2 || true
    kubectl -n "$ns" logs "$pod" --all-containers --tail=60 >&2 || true
    kubectl -n "$ns" logs "$pod" -c opence-server --previous --tail=60 >&2 2>/dev/null || true
  done
}
# the newest pod (an old one may still be going)
pod_name() {
  kubectl -n "$ns" get pods -l app.kubernetes.io/instance=$release --sort-by=.metadata.creationTimestamp \
    -o jsonpath='{.items[-1:].metadata.name}'
}
# a field of the server container's status in the pod
status() { kubectl -n "$ns" get pod "$1" -o jsonpath="{.status.containerStatuses[?(@.name=='opence-server')]$2}"; }
endpoint() { kubectl -n "$ns" get endpointslices -l kubernetes.io/service-name=$release -o jsonpath='{.items[*].endpoints[*].addresses[0]}'; }

step "install ($scenario, $image): no maps"
kubectl create namespace "$ns"
# (a pod is refused there unless it runs as non-root, without capabilities or privilege escalation,
# under the runtime's seccomp profile)
kubectl label namespace "$ns" pod-security.kubernetes.io/enforce=restricted pod-security.kubernetes.io/enforce-version=latest
helm install "$release" "$chart" -n "$ns" -f "$chart/ci/$scenario-values.yaml" \
  --set "image.repository=${image%:*}" --set "image.tag=${image##*:}" --set image.pullPolicy=IfNotPresent \
  --set terminationGracePeriodSeconds=5
# (the kubelet waits 10 s, then 20 s, before it starts the server again: CrashLoopBackOff from its
# second exit)
pod=
for _ in $(seq 1 90); do
  pod=$(pod_name)
  if [ -n "$pod" ]; then
    [ "$(status "$pod" .ready)" != true ] || fail "the server got ready without maps"
    if [ "$(status "$pod" .lastState.terminated.exitCode)" = 2 ] && [ "$(status "$pod" .state.waiting.reason)" = CrashLoopBackOff ]; then
      break
    fi
  fi
  sleep 2
done
[ -n "$pod" ] || fail "no pod (refused under the restricted profile?)"
[ "$(status "$pod" .lastState.terminated.exitCode)" = 2 ] || fail "the server did not exit with status 2"
[ "$(status "$pod" .state.waiting.reason)" = CrashLoopBackOff ] || fail "the pod is not in CrashLoopBackOff"
log=$(kubectl -n "$ns" logs "$pod" -c opence-server --previous)
echo "$log"
grep -qF "$expected" <<<"$log" || fail "the log does not say which maps were missing where"
grep -q 'settings: a dedicated server, hosting bloodgulch:slayer' <<<"$log" || fail "the log does not say it is a dedicated server hosting bloodgulch"
grep -qE 'settings: (wrote the defaults to )?/opt/opence/config.toml$' <<<"$log" || fail "config.toml was not written to /state (through the image's link)"
if grep -q 'Internet play' <<<"$log"; then fail "the server's network started"; fi
[ "$(status "$pod" .ready)" = false ] || fail "the pod is ready"
[ -z "$(endpoint)" ] || fail "the Service has an endpoint"
echo "admitted under the restricted profile; exit 2, CrashLoopBackOff ($(status "$pod" .restartCount) restarts), never ready: $pod"

step "the pod's hardening and its probes: the server swapped for sleep, a sidecar holding the tunnel port"
port=$(kubectl -n "$ns" get "$deploy" -o jsonpath='{.spec.template.spec.containers[0].ports[0].containerPort}')
security=$(kubectl -n "$ns" get "$deploy" -o jsonpath='{.spec.template.spec.containers[0].securityContext}')
kubectl -n "$ns" patch "$deploy" --type=json -p "[
  {\"op\": \"add\", \"path\": \"/spec/template/spec/containers/0/command\", \"value\": [\"sleep\", \"infinity\"]},
  {\"op\": \"add\", \"path\": \"/spec/template/spec/containers/-\", \"value\": {\"name\": \"port\",
    \"image\": \"$port_image\", \"securityContext\": $security,
    \"command\": [\"python3\", \"-c\", \"import socket, time; s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(('0.0.0.0', $port)); time.sleep(1e9)\"]}}]"
kubectl -n "$ns" rollout status "$deploy" --timeout=180s || fail "the probes did not pass with the port bound"
pod=$(pod_name)
in_pod() { kubectl -n "$ns" exec "$pod" -c opence-server -- "$@"; }
[ "$(in_pod id -u)" = 10001 ] || fail "not running as 10001"
if in_pod sh -c 'touch /opt/opence/x' 2>/dev/null; then fail "the root filesystem is writable"; fi
if in_pod sh -c 'touch /data/x' 2>/dev/null; then fail "/data is writable"; fi
if in_pod test -e /var/run/secrets/kubernetes.io/serviceaccount/token; then fail "a service account token is mounted"; fi
in_pod sh -c 'touch /state/x && rm /state/x' || fail "/state is not writable"
echo "ready on the probes; uid 10001, read-only root and /data, no token, /state writable: $pod"

step "the Service and the NetworkPolicy"
[ "$(kubectl -n "$ns" get svc "$release" -o jsonpath='{.spec.ports[0].protocol}/{.spec.ports[0].port}')" = UDP/27015 ] \
  || fail "the Service does not expose UDP 27015"
[ -n "$(endpoint)" ] || fail "the Service has no endpoint"
kubectl -n "$ns" get networkpolicy "$release" >/dev/null || fail "no NetworkPolicy"
echo "UDP 27015 with an endpoint; NetworkPolicy present"

step "restart: the old pod goes, a new one gets ready"
kubectl -n "$ns" rollout restart "$deploy"
kubectl -n "$ns" rollout status "$deploy" --timeout=180s || fail "the new pod did not get ready"
[ "$(pod_name)" != "$pod" ] || fail "no new pod"
[ "$(kubectl -n "$ns" get pods -l app.kubernetes.io/instance=$release -o name | wc -l)" = 1 ] || fail "more than one server pod"
echo "new pod $(pod_name) ready"

step "uninstall"
helm uninstall "$release" -n "$ns" --wait
echo
echo "PASS ($scenario)"
