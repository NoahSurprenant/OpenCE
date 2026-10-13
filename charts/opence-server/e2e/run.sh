#!/usr/bin/env bash
# End-to-end test of the chart on a real cluster: ./run.sh [minimal]
#
# Needs kubectl and helm pointed at an empty cluster (k3d or k3s; they enforce NetworkPolicies), and
# runs the server image IMAGE (repository:tag; default the published :latest - the workflow builds it
# from the checkout and imports it). There are no maps in CI (they are never published), so the
# release runs with ci/minimal-values.yaml's emptyDir in their place: the server starts, binds its
# tunnel port and hosts nothing. That is enough to check everything around it:
#   1. install: the pod starts under the chart's security context, its probes (the bound tunnel port)
#      pass and it gets ready; the log says it is a dedicated server hosting the configured maps;
#   2. the pod: user 10001, a read-only root filesystem, no service account token, the state volume
#      writable (config.toml through the image's link), /data read-only;
#   3. the Service and the NetworkPolicy exist as rendered;
#   4. a restart: the old server quits on SIGTERM and says so, within the grace period, and the new
#      pod gets ready (Recreate: never two at once).
set -euo pipefail

scenario=${1:-minimal}
case "$scenario" in minimal) ;; *) echo "usage: $0 [minimal]" >&2; exit 2 ;; esac
chart=$(cd "$(dirname "$0")/.." && pwd)
ns=opence-e2e
release=opence-server
image=${IMAGE:-ghcr.io/noahsurprenant/opence-server:latest}
deploy=deploy/$release

step() { echo; echo "=== $*"; }
fail() { echo "FAIL: $*" >&2; diagnose; exit 1; }
diagnose() {
  echo "--- pods" >&2
  kubectl -n "$ns" get pods -o wide >&2 || true
  for pod in $(kubectl -n "$ns" get pods -o name 2>/dev/null); do
    echo "--- $pod" >&2
    kubectl -n "$ns" describe "$pod" | sed -n '/^Events:/,$p' >&2 || true
    kubectl -n "$ns" logs "$pod" --tail=60 >&2 || true
  done
}
pod_name() { kubectl -n "$ns" get pods -l app.kubernetes.io/instance=$release -o jsonpath='{.items[0].metadata.name}'; }

step "install ($scenario, $image)"
kubectl create namespace "$ns"
helm install "$release" "$chart" -n "$ns" -f "$chart/ci/$scenario-values.yaml" \
  --set "image.repository=${image%:*}" --set "image.tag=${image##*:}" --set image.pullPolicy=IfNotPresent \
  --set defaultServiceAccount.manage=true
kubectl -n "$ns" rollout status "$deploy" --timeout=180s || fail "the server did not get ready"
pod=$(pod_name)
kubectl -n "$ns" logs "$pod" | grep -q 'settings: a dedicated server, hosting bloodgulch:slayer' \
  || fail "the log does not say it is a dedicated server hosting bloodgulch"
echo "ready, and a dedicated server: $pod"

step "the pod's hardening"
[ "$(kubectl -n "$ns" exec "$pod" -- id -u)" = 10001 ] || fail "not running as 10001"
if kubectl -n "$ns" exec "$pod" -- sh -c 'touch /opt/opence/x' 2>/dev/null; then fail "the root filesystem is writable"; fi
if kubectl -n "$ns" exec "$pod" -- sh -c 'touch /data/x' 2>/dev/null; then fail "/data is writable"; fi
if kubectl -n "$ns" exec "$pod" -- test -e /var/run/secrets/kubernetes.io/serviceaccount/token; then fail "a service account token is mounted"; fi
kubectl -n "$ns" exec "$pod" -- grep -q '^\[server\]' /state/config.toml || fail "config.toml was not written to /state"
kubectl -n "$ns" exec "$pod" -- test -d /state/saves || fail "no save root in /state"
echo "uid 10001, read-only root and /data, no token, state written"

step "the Service and the NetworkPolicy"
[ "$(kubectl -n "$ns" get svc "$release" -o jsonpath='{.spec.ports[0].protocol}/{.spec.ports[0].port}')" = UDP/27015 ] \
  || fail "the Service does not expose UDP 27015"
[ -n "$(kubectl -n "$ns" get endpointslices -l kubernetes.io/service-name=$release -o jsonpath='{.items[0].endpoints[0].addresses[0]}')" ] \
  || fail "the Service has no endpoint"
kubectl -n "$ns" get networkpolicy "$release" >/dev/null || fail "no NetworkPolicy"
echo "UDP 27015 with an endpoint; NetworkPolicy present"

step "restart: SIGTERM, a clean exit, a new pod"
kubectl -n "$ns" logs -f "$pod" > "${TMPDIR:-/tmp}/opence-e2e-old.log" 2>&1 &
follower=$!
sleep 2
start=$(date +%s)
kubectl -n "$ns" delete pod "$pod" --wait=true --timeout=60s
took=$(( $(date +%s) - start ))
wait "$follower" || true
grep -q 'quitting on signal 15' "${TMPDIR:-/tmp}/opence-e2e-old.log" || { cat "${TMPDIR:-/tmp}/opence-e2e-old.log" | tail -20; fail "the server did not quit on SIGTERM"; }
grep -q 'server: closed' "${TMPDIR:-/tmp}/opence-e2e-old.log" || fail "the server did not close cleanly"
[ "$took" -lt 15 ] || fail "the pod took ${took}s to stop (grace period 15s)"
kubectl -n "$ns" rollout status "$deploy" --timeout=180s || fail "the new server did not get ready"
[ "$(pod_name)" != "$pod" ] || fail "no new pod"
echo "quit on SIGTERM in ${took}s, closed cleanly; new pod $(pod_name) ready"

step "uninstall"
helm uninstall "$release" -n "$ns" --wait
echo
echo "PASS ($scenario)"
