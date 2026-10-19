# opence-server Helm chart

Deploys the OpenCE dedicated server: a headless Halo: Combat Evolved host for internet play, with no
window, sound or player of its own ([port/linux/README.md](../../port/linux/README.md#dedicated-server),
"Dedicated server"). One pod runs the server; a Service exposes its UDP tunnel port; a NetworkPolicy
limits it to what internet play needs.

The game data is **not** in the image and never will be: you mount your own copy of the maps, from
an Xbox disc image of the game, read-only.

Published to `oci://ghcr.io/noahsurprenant/charts/opence-server` by the
[Helm chart workflow](../../.github/workflows/helm-chart.yml) whenever the chart changes on main, as
`<major>.<minor>.<run number>` (major and minor from `Chart.yaml`), and signed keyless with cosign:

```sh
cosign verify ghcr.io/noahsurprenant/charts/opence-server:<version> \
  --certificate-identity-regexp '^https://github\.com/NoahSurprenant/OpenCE/\.github/workflows/helm-chart\.yml@refs/heads/main$' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com
```

The image, `ghcr.io/noahsurprenant/opence-server`, is built from
[docker/server/Dockerfile](../../docker/server/Dockerfile) by the
[Server image workflow](../../.github/workflows/server-image.yml): `sha-<commit>` for each build of
main, and `latest` moved to it once it is signed (the same identity, with `server-image\.yml`).

## Quick start

Put the folder that contains `maps/` where the cluster can mount it - an NFS export, or a
PersistentVolumeClaim you create - and give it to the chart:

```yaml
gameData:
  nfs:
    server: nfs.example.com
    path: /exports/halo        # contains maps/

server:
  maps: [bloodgulch:slayer, bloodgulch:ctf, hangemhigh:king]
  timeLimit: 15

network:
  # The router forwards UDP 27015 to the Service; players reach the server at this name.
  publicAddress: halo.example.com:27015

service:
  type: LoadBalancer
  externalTrafficPolicy: Local
```

```sh
helm install opence-server oci://ghcr.io/noahsurprenant/charts/opence-server --version <version> \
  -n opence-server --create-namespace -f values.yaml
kubectl -n opence-server logs deploy/opence-server | grep 'Invite players'
```

The server is invite-only by default (`network.hostPublic: false`): players join with the invite link
that the log shows. It changes at each start unless the state is persistent (`persistence.enabled`).
Set `network.hostPublic: true` and `network.serverName` to list it in the in-game server browser.

Every value is described in [values.yaml](values.yaml); [values.schema.json](values.schema.json)
checks them, so a misspelled or unknown key, or a value of the wrong type, fails the install instead
of being ignored.

## Game data

Exactly one of these, mounted read-only at `/data` (`HALO_DATA_ROOT`); the chart refuses to render
without one, and sets no default path:

| Value | Volume |
| --- | --- |
| `gameData.existingClaim` | A PersistentVolumeClaim you made, for example on a static, read-only NFS PersistentVolume. |
| `gameData.nfs.server`, `gameData.nfs.path` | An NFS export, mounted by the kubelet. |
| `gameData.volume` | Any other volume source, as in a pod spec without its name (`hostPath`, `csi`, ...). |

`gameData.subPath` picks a folder within the volume. The server only reads the data and writes nothing
to `/data`: its log (`debug.txt`), `gamestate.txt` and an empty `custom_maps` folder go to the save
root under `/state`, and every line of the log goes to the standard error too, where `kubectl logs`
shows it.

The server needs `maps/ui.map` and a map file for each map of `server.maps`, and checks them before it
starts. If one is missing or is not a map (a wrong volume, path or `subPath`, or an empty folder), it
exits with status 2 and one line that names the maps and the folder, before it binds its port:

```
halo-linux: server: no usable maps in /data/maps (looked for ui.map, bloodgulch.map); exiting
```

The pod goes into `CrashLoopBackOff` and never gets ready. `kubectl logs --previous` shows the line.

## Custom maps, Skate 3 data and logs

Three optional volumes, each with at most one source:

| Value | Mounted at | Variable | Without it |
| --- | --- | --- | --- |
| `customMaps.existingClaim`, `customMaps.nfs`, `customMaps.volume` (`subPath`, `readOnly`, true by default) | `/custom_maps` | `HALO_CUSTOM_MAPS_ROOT` | `custom_maps` in the game data if it has one, else an empty folder in the save root |
| `skateData.existingClaim`, `skateData.nfs`, `skateData.volume` (`subPath`), always read-only | `/skate-data` | `HALO_SKATE_ASSETS=/skate-data/assets` | the bots stay on foot |
| `logs.existingClaim`, `logs.volume` (`subPath`) | `/logs` | `HALO_LOG_ROOT` | `debug.txt` in the save root, `/state/saves/debug.txt` |

The log goes to the standard error either way.

`skateData` is your own converted Skate 3 data (`port/skate/README.md`, "What you need": the
`skate-data` folder, the one with `assets/` in it), never in the image. With it the server's bots
ride boards to travel and step off to fight (`port/skate/README.md`, "Bots on boards"), every bot
that wants to (`server.botSkaters` can limit them): the first skater about 58 MB of memory and 12
seconds of a processor, each further one about 10 MB and a few milliseconds, and each about 4.6 ms
of a processor every tick while it rides (a server short of processors lets its bots skate slower,
and says so; "Several skaters" in that README has the figures for 32, 64 and 127); raise
`resources.limits.memory` to match (about 600Mi for 32 riding bots, 900Mi for 64, 1800Mi for 127; the
512Mi default fits a handful). The server logs `skate: a dedicated server with Skate 3 data` at start.
Without it the server logs that it has none, and its bots walk. Human skaters joining the server
skate either way: the server relays them and decides their hits without the data.

## Networking

Internet play uses one UDP port, `network.tunnelPort` (27015). The machines of an invite find each
other through public MQTT brokers (TCP 1883) and learn their public addresses from STUN servers
(UDP).

Most clusters sit behind a NAT that gives each destination its own port, which players cannot reach
through the server's STUN address. Then:

1. Expose the port: `service.type: LoadBalancer` (or `NodePort`). A load balancer's fixed address
   is usually an annotation of the implementation (`service.annotations`).
2. Forward that UDP port on the router to the Service's address.
3. Set `network.publicAddress` to the router's public address, a name or an IPv4 address, with
   `:<port>` if the router forwards another one. A name is looked up again every five minutes, so a
   dynamic DNS name follows the router. The name must resolve to the router itself: a proxy that
   only carries HTTP (a CDN's) cannot carry the game's UDP.

`service.externalTrafficPolicy: Local` keeps the players' own addresses (no SNAT to a node).

UPnP is off (`network.allowUpnp`): a pod seldom reaches the router's UPnP, and the forward is made
by hand.

### Network policy

On by default (`networkPolicy.enabled`):

| Direction | Allowed |
| --- | --- |
| In | UDP on the tunnel port, from anyone (`networkPolicy.from` narrows it) |
| Out | DNS to the cluster's DNS pods (`networkPolicy.dns.to`); TCP 1883 (the MQTT brokers) and every UDP port (STUN, and the players' own tunnel ports, which can be any) to `networkPolicy.internet.cidr`, except the private ranges in `networkPolicy.internet.except` |

So the server reaches nothing in the cluster or on the local network. `networkPolicy.extraIngress`
and `extraEgress` add rules.

## The pod

- One replica with `strategy: Recreate`: one server owns the port and the address, never two at once.
- Non-root (uid 10001), read-only root filesystem, every capability dropped, no privilege
  escalation, seccomp `RuntimeDefault`, no service account token.
- `/state` is the only writable folder (with a `logs` volume, or a writable `customMaps` one, those
  too): `config.toml` (the server writes the defaults at its first
  start; the `HALO_*` variables the chart sets take priority over it), the save root with its map
  cache (about 115 MB for the multiplayer maps) and `debug.txt`, and the key of the invite links. An emptyDir
  unless `persistence.enabled`.
- Probes: the server holds a UDP socket on the tunnel port (the container's `/proc/net/udp`) from
  the start of internet play until it quits. The startup probe allows two minutes. A server without
  its maps never binds the port: it exits with status 2 (Game data, above).
- A fatal error (a map that cannot be read, say) ends the server with status 1 and a `server: a fatal
  error (...)` line, and the pod restarts. If the server does not exit within 5 seconds of SIGTERM, it
  is ended with status 143, before the kubelet kills it.
- SIGTERM ends the game in progress, tells the players and exits 0 in about 2 seconds
  (`terminationGracePeriodSeconds: 15`).
- Measured with a client playing: about 120-140 MB resident and 1-3% of a core. The defaults
  request 50m and 192Mi and limit memory to 512Mi.
- The image is 32-bit x86 Linux code: amd64 nodes only (`nodeSelector`).

For admin, `telnetConsole.enabled` opens the game's script console on 127.0.0.1 inside the pod (no
password), which `kubectl exec` reaches; NOTES.txt prints the command.

## Settings

The chart maps its values to the server's environment variables (port/linux/README.md, "Settings"):

| Value | Variable |
| --- | --- |
| `server.maps` (list) | `HALO_SERVER_MAPS` |
| `server.gameTypes` (list) | `HALO_SERVER_GAME_TYPES` |
| `server.timeLimit`, `server.scoreLimit` | `HALO_SERVER_TIME_LIMIT`, `HALO_SERVER_SCORE_LIMIT` |
| `server.bots`, `server.botDifficulty`, `server.botSkaters` | `HALO_SERVER_BOTS`, `HALO_SERVER_BOT_DIFFICULTY`, `HALO_SERVER_BOT_SKATERS` |
| `network.tunnelPort` | `HALO_NET_TUNNEL_PORT` |
| `network.publicAddress` | `HALO_NET_PUBLIC_ADDRESS` |
| `network.hostPublic`, `network.serverName` | `HALO_NET_HOST_PUBLIC`, `HALO_NET_SERVER_NAME` |
| `network.allowUpnp`, `network.online`, `network.stunServers` | `HALO_NET_ALLOW_UPNP`, `HALO_NET_ONLINE`, `HALO_NET_STUN` |
| `telnetConsole.enabled`, `telnetConsole.port` | `HALO_TELNET_CONSOLE`, `HALO_TELNET_CONSOLE_PORT` |

Always set: `HALO_DEDICATED=1`, `HALO_DATA_ROOT=/data`, `HALO_SAVE_ROOT=/state/saves`; with their
volumes, `HALO_CUSTOM_MAPS_ROOT=/custom_maps` and `HALO_LOG_ROOT=/logs`. Any other
setting goes in `extraEnv`, which comes last and so overrides the chart's.

## Tests

- `tests/`: [helm-unittest](https://github.com/helm-unittest/helm-unittest) suites (`helm unittest charts/opence-server`).
- `ci/*-values.yaml`: linted and rendered against the Kubernetes schemas of the oldest and newest
  supported versions.
- `e2e/run.sh`: installs the chart on a k3s cluster with the image built from the checkout, in a
  namespace that enforces Pod Security's `restricted` profile. There are no maps in CI, so
  `ci/minimal-values.yaml` mounts an emptyDir in their place. The test checks that the server exits
  with status 2 and the line about the maps, that the pod goes into `CrashLoopBackOff` and never gets
  ready, and that the Service has no ready endpoint. Then it patches the Deployment: the container runs
  `sleep` in place of the server (with the same image, user, security context and volumes), and a
  sidecar binds the tunnel port as the server does. With this, the test checks that the probes pass
  and the pod gets ready, the pod's hardening, the Service and its endpoint, the NetworkPolicy and a
  restart. A running server and its clean stop on SIGTERM need maps, so OpenCE's own
  `tools/test_linux_port.py` (`test_dedicated_server`, on a runner that has the maps) checks them.

Kubernetes 1.34 to 1.37 are tested (`kubeVersion` in `Chart.yaml` refuses older).
