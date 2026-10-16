{{/* The objects' name: fullnameOverride, else the release name if it contains the chart's name, else
   <release>-<chart>. */}}
{{- define "opence-web.fullname" -}}
{{- if .Values.fullnameOverride -}}
{{- .Values.fullnameOverride | trunc 63 | trimSuffix "-" -}}
{{- else -}}
{{- $name := default .Chart.Name .Values.nameOverride -}}
{{- if contains $name .Release.Name -}}
{{- .Release.Name | trunc 63 | trimSuffix "-" -}}
{{- else -}}
{{- printf "%s-%s" .Release.Name $name | trunc 63 | trimSuffix "-" -}}
{{- end -}}
{{- end -}}
{{- end }}

{{- define "opence-web.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" -}}
{{- end }}

{{/* Labels on every object. */}}
{{- define "opence-web.labels" -}}
{{ include "opence-web.selectorLabels" . }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
helm.sh/chart: {{ printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/* What the Service and the NetworkPolicy select: stable across chart versions. */}}
{{- define "opence-web.selectorLabels" -}}
app.kubernetes.io/name: {{ include "opence-web.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end }}

{{/* repository:tag, repository:tag@digest, or repository@digest. tag may itself carry a digest
   (tag@sha256:... or sha256:..., what Argo CD Image Updater writes); digest, when set, replaces it.
   An empty tag is the chart's appVersion. */}}
{{- define "opence-web.image" -}}
{{- $image := .Values.image -}}
{{- $ref := $image.repository -}}
{{- $tag := toString ($image.tag | default .Chart.AppVersion) -}}
{{- if $image.digest }}{{ $tag = regexReplaceAll "@.*$" $tag "" }}{{ if hasPrefix "sha256:" $tag }}{{ $tag = "" }}{{ end }}{{ end -}}
{{- if hasPrefix "sha256:" $tag }}{{ $ref = printf "%s@%s" $ref $tag }}
{{- else if $tag }}{{ $ref = printf "%s:%s" $ref $tag }}{{ end -}}
{{- if and $image.digest (not (contains "@" $ref)) }}{{ $ref = printf "%s@%s" $ref $image.digest }}{{ end -}}
{{- $ref -}}
{{- end }}

{{/* "true" when game data is configured (a private server); fails when both sources are. */}}
{{- define "opence-web.hasGameData" -}}
{{- with .Values.gameData -}}
{{- if and .existingClaim .volume -}}
{{- fail "gameData: set existingClaim or volume, not both" -}}
{{- end -}}
{{- if or .existingClaim .volume }}true{{ end -}}
{{- end -}}
{{- end }}

{{/* The game data's read-only mounts, by subPath: the maps, and skate-data's assets and sounds. */}}
{{- define "opence-web.gameDataMounts" -}}
- name: game-data
  mountPath: /srv/game-data/maps
  subPath: {{ .Values.gameData.maps.subPath }}
  readOnly: true
{{- if .Values.gameData.skate.enabled }}
- name: game-data
  mountPath: /srv/game-data/skate-data/assets
  subPath: {{ .Values.gameData.skate.assetsSubPath }}
  readOnly: true
- name: game-data
  mountPath: /srv/game-data/skate-data/sounds
  subPath: {{ .Values.gameData.skate.soundsSubPath }}
  readOnly: true
{{- end }}
{{- end }}

{{/* nginx's realip settings (realIp): what the ConfigMap holds, and the pod's checksum of it. */}}
{{- define "opence-web.realIpConf" -}}
{{- range .Values.realIp.trustedProxies -}}
set_real_ip_from {{ . }};
{{ end -}}
real_ip_header {{ .Values.realIp.header }};
real_ip_recursive {{ ternary "on" "off" .Values.realIp.recursive }};
{{- end }}
