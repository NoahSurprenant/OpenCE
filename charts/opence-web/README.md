# opence-web Helm chart

Deploys the OpenCE web build ([port/web/README.md](../../port/web/README.md)): Halo: Combat Evolved
in the browser, a static site served by nginx with the headers the game needs. Optionally, a
**private server** of your own game data: the page then downloads the maps (and Skate 3 mode's
converted data) from the site itself into the browser's storage, instead of each player importing
them.

The image holds code only: the site, nginx and the manifest script. The game data is **not** in the
image and never will be.

> **Your own copy, on a private network only.** The game data you mount must be your own copy of
> the game, served only to yourself and the people you may share it with, on a private network (a
> home network, a VPN): never on a public site. The chart does nothing to restrict who reaches it;
> that is your ingress's, your network's and your NetworkPolicy's job.

Published to `oci://ghcr.io/noahsurprenant/charts/opence-web` by the
[Helm chart (web) workflow](../../.github/workflows/helm-chart-web.yml) whenever the chart changes on
main, as `<major>.<minor>.<run number>` (major and minor from `Chart.yaml`), and signed keyless with
cosign:

```sh
cosign verify ghcr.io/noahsurprenant/charts/opence-web:<version> \
  --certificate-identity-regexp '^https://github\.com/NoahSurprenant/OpenCE/\.github/workflows/helm-chart-web\.yml@refs/heads/main$' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com
```

The image, `ghcr.io/noahsurprenant/opence-web`, is built from
[docker/web/Dockerfile](../../docker/web/Dockerfile) with each build of main's web site by the
`web-image` job of the [Build workflow](../../.github/workflows/build.yml): `sha-<commit>`, and
`latest` moved to it once it is signed (identity `build\.yml` in the regular expression above).

## Quick start

The plain site, behind an ingress with TLS:

```yaml
ingress:
  enabled: true
  className: nginx
  annotations:
    cert-manager.io/cluster-issuer: letsencrypt
  hosts:
    - host: halo.example.com
  tls:
    - secretName: halo-tls
      hosts: [halo.example.com]
```

```sh
helm install opence-web oci://ghcr.io/noahsurprenant/charts/opence-web --version <version> \
  -n opence-web --create-namespace -f values.yaml
```

Every value is described in [values.yaml](values.yaml); [values.schema.json](values.schema.json)
checks them, so a misspelled or unknown key, or a value of the wrong type, fails the install instead
of being ignored.

## HTTPS and the headers

The game runs threads, which need `SharedArrayBuffer`, which a browser gives only to a page in a
**secure context** (HTTPS, or `http://localhost`) that is **cross-origin isolated**. nginx sends, with
every response:

| Header | Value |
| --- | --- |
| `Cross-Origin-Opener-Policy` | `same-origin` |
| `Cross-Origin-Embedder-Policy` | `require-corp` |
| `Cross-Origin-Resource-Policy` | `same-origin` |
| `X-Content-Type-Options` | `nosniff` |

so put TLS in front of it (an ingress with a certificate), or try it with
`kubectl port-forward svc/opence-web 8080:80` and `http://localhost:8080/`. Over plain HTTP from
another machine the page does not start the game. An ingress or proxy in front must pass these
headers through unchanged.

The site is served at the root of its host: its addresses are relative, but nginx's
`/server-data.json` and `/game-data/` are not, so the ingress path is `/`.

Site files are sent with `Cache-Control: no-cache` (the browser revalidates with `ETag` and
`Last-Modified`, as the page's service worker and `version.json` expect of a new build);
`/server-data.json` and `/game-data/manifest.json` with `no-store`. `halo.wasm` is
`application/wasm`, gzipped when the browser asks.

## Behind a proxy: the visitors' addresses

nginx logs the address of whatever connected to it: behind an ingress, the ingress controller's pod,
for every visitor. `realIp` has nginx's realip module take the visitor's address from the header the
proxy writes, in the access log and `$remote_addr`, believed only on a connection from
`realIp.trustedProxies` (off while that is empty, the default):

```yaml
realIp:
  # where the ingress controller connects from: the pod network (k3s's default here)
  trustedProxies: [10.42.0.0/16]
  # X-Forwarded-For, read from the right past every trusted address (recursive: true), is the
  # default; a proxy that writes the visitor alone in a header can be named instead
  header: X-Forwarded-For
  recursive: true
```

With X-Forwarded-For, any hop between the visitor and nginx that appends itself (a CDN in front of
the ingress, say) must be in `trustedProxies` too, or nginx logs that hop instead; when the ingress
already resolves the visitor into a header of its own (`X-Real-IP`, say), naming that header avoids
listing them. The chart writes the settings into a ConfigMap mounted at `/etc/nginx/opence/conf.d`,
which the image's nginx includes; a change rolls the pod. Only the log and `$remote_addr` change: the
site serves everyone the same.

## A private server: game data

Not set (the default), the chart serves the plain site: no init container, and `/server-data.json`
is a 404, which the page takes as "import your own data". Set exactly one source:

| Value | Volume |
| --- | --- |
| `gameData.existingClaim` | A PersistentVolumeClaim you made, for example on a static, read-only NFS PersistentVolume. |
| `gameData.volume` | Any volume source, as in a pod spec without its name (`nfs`, `hostPath`, `persistentVolumeClaim`, `csi`, ...). |

```yaml
gameData:
  volume:
    nfs:
      server: nfs.example.com
      path: /exports/games
      readOnly: true
  maps:
    subPath: halo/maps            # ui.map, bloodgulch.map, ... (loading.tga too)
  skate:
    enabled: true                 # false: maps only
    assetsSubPath: skate-data/assets
    soundsSubPath: skate-data/sounds
```

The volume is mounted **read-only**, by subPath only, at

| Path in the pod | Served at | From the volume |
| --- | --- | --- |
| `/srv/game-data/maps` | `/game-data/maps/` | `gameData.maps.subPath` (default `maps`) |
| `/srv/game-data/skate-data/assets` | `/game-data/skate-data/assets/` | `gameData.skate.assetsSubPath` (default `skate-data/assets`) |
| `/srv/game-data/skate-data/sounds` | `/game-data/skate-data/sounds/` | `gameData.skate.soundsSubPath` (default `skate-data/sounds`) |

so nothing else of the volume is visible to the pod; anything else under `/game-data/` is a 404, as
are hidden files and symbolic links. Range requests are answered (206); the game data is never
gzipped.

At each pod start an init container, `manifest` (the same image and hardening), runs
[tools/web_server_data_manifest.py](../../tools/web_server_data_manifest.py):

```sh
python3 /opt/opence-web/web_server_data_manifest.py --maps /srv/game-data/maps \
  --skate-data /srv/game-data/skate-data --output /srv/generated/manifest.json \
  --config /srv/generated/server-data.json --manifest-url game-data/manifest.json
```

(without `--skate-data` when `gameData.skate.enabled` is false). It hashes every file it lists into
an emptyDir, which nginx mounts read-only and serves as `/game-data/manifest.json` and
`/server-data.json`. For about 2 GB of maps and skate data that takes some 30-60 seconds (a core and
the volume's read speed), before nginx starts and the pod gets ready; `manifest.resources` sets its
requests and limits. A change to the data needs a new pod (`kubectl rollout restart`).

## Security

- nginx runs as uid/gid 101, non-root, with a read-only root filesystem (only `/tmp`, an emptyDir, is
  writable, and `/srv/generated` for the init container), no capabilities, no privilege escalation,
  the runtime's seccomp profile, and no service account token. No `fsGroup`: the kubelet never
  changes the game data's ownership. The pod passes Pod Security's `restricted` profile (with a
  claim, or another volume type `restricted` allows).
- The NetworkPolicy (`networkPolicy.enabled`, default on) lets anyone reach port 8080
  (`networkPolicy.ingressFrom` narrows it, e.g. to the ingress controller's namespace) and allows
  **no egress at all**: nginx and the manifest script need no network, not even DNS.
- `defaultServiceAccount.manage: true` also turns off the namespace's default ServiceAccount token.

## The image

| | |
| --- | --- |
| Repository | `ghcr.io/noahsurprenant/opence-web` |
| Port | 8080 (HTTP); `GET /healthz` is 200 for the probes |
| User | 101:101 |
| Site | `/srv/site` |
| Game data | `/srv/game-data/maps`, `/srv/game-data/skate-data/{assets,sounds}` (mounted) |
| Manifest | `/srv/generated/{manifest.json,server-data.json}` (written by the script) |
| Script | `/opt/opence-web/web_server_data_manifest.py` (Python 3, standard library) |
| Writable | `/tmp` (pid and temporary files) |
| Settings | `/etc/nginx/opence/conf.d/*.conf`, included at nginx's http level (the chart's `realIp`) |

## Tests

`helm unittest charts/opence-web` (tests/), the `ci/*-values.yaml` files (lint and kubeconform), and
[e2e/run.sh](e2e/run.sh), which installs the chart on k3d with a stub site and small generated
placeholder files in place of the game data.
