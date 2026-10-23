{{/* The objects' name: fullnameOverride, else the release name if it contains the chart's name, else
   <release>-<chart>. */}}
{{- define "opence-server.fullname" -}}
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

{{- define "opence-server.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" -}}
{{- end }}

{{/* Labels on every object. */}}
{{- define "opence-server.labels" -}}
{{ include "opence-server.selectorLabels" . }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
helm.sh/chart: {{ printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/* What the Service and the NetworkPolicy select: stable across chart versions. */}}
{{- define "opence-server.selectorLabels" -}}
app.kubernetes.io/name: {{ include "opence-server.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end }}

{{/* repository:tag, repository:tag@digest, or repository@digest. tag may itself carry a digest
   (tag@sha256:... or sha256:..., what Argo CD Image Updater writes); digest, when set, replaces it.
   An empty tag is the chart's appVersion. */}}
{{- define "opence-server.image" -}}
{{- include "opence-server.imageRef" (dict "image" .Values.image "default" .Chart.AppVersion) -}}
{{- end }}

{{/* The stream's image (stream.image), as opence-server.image. */}}
{{- define "opence-server.streamImage" -}}
{{- include "opence-server.imageRef" (dict "image" .Values.stream.image "default" .Chart.AppVersion) -}}
{{- end }}

{{/* image (repository, tag, digest) as a reference; default: the tag when tag is empty. */}}
{{- define "opence-server.imageRef" -}}
{{- $image := .image -}}
{{- $ref := $image.repository -}}
{{- $tag := toString ($image.tag | default .default) -}}
{{- if $image.digest }}{{ $tag = regexReplaceAll "@.*$" $tag "" }}{{ if hasPrefix "sha256:" $tag }}{{ $tag = "" }}{{ end }}{{ end -}}
{{- if hasPrefix "sha256:" $tag }}{{ $ref = printf "%s@%s" $ref $tag }}
{{- else if $tag }}{{ $ref = printf "%s:%s" $ref $tag }}{{ end -}}
{{- if and $image.digest (not (contains "@" $ref)) }}{{ $ref = printf "%s@%s" $ref $image.digest }}{{ end -}}
{{- $ref -}}
{{- end }}

{{/* The claim of the state volume: persistence.existingClaim, else the chart's. */}}
{{- define "opence-server.stateClaim" -}}
{{- .Values.persistence.existingClaim | default (printf "%s-state" (include "opence-server.fullname" .)) -}}
{{- end }}

{{/* The tunnel port in /proc/net/udp's form (4 hex digits, upper case), for the probe. */}}
{{- define "opence-server.portHex" -}}
{{- printf "%04X" (int .Values.network.tunnelPort) -}}
{{- end }}

{{/* "true" when a game data volume is configured. */}}
{{- define "opence-server.hasGameData" -}}
{{- with .Values.gameData -}}
{{- if or .existingClaim (and .nfs.server .nfs.path) .volume }}true{{ end -}}
{{- end -}}
{{- end }}

{{/* "true" when a custom_maps volume is configured. */}}
{{- define "opence-server.hasCustomMaps" -}}
{{- with .Values.customMaps -}}
{{- if or .existingClaim (and .nfs.server .nfs.path) .volume }}true{{ end -}}
{{- end -}}
{{- end }}

{{/* "true" when a Skate 3 data volume is configured. */}}
{{- define "opence-server.hasSkateData" -}}
{{- with .Values.skateData -}}
{{- if or .existingClaim (and .nfs.server .nfs.path) .volume }}true{{ end -}}
{{- end -}}
{{- end }}

{{/* "true" when a logs volume is configured. */}}
{{- define "opence-server.hasLogs" -}}
{{- with .Values.logs -}}
{{- if or .existingClaim .volume }}true{{ end -}}
{{- end -}}
{{- end }}

{{/* The stream's objects' name and the headless Service of the server's system link. */}}
{{- define "opence-server.streamName" -}}
{{- printf "%s-stream" (include "opence-server.fullname" .) | trunc 63 | trimSuffix "-" -}}
{{- end }}

{{- define "opence-server.systemLinkName" -}}
{{- printf "%s-system-link" (include "opence-server.fullname" .) | trunc 63 | trimSuffix "-" -}}
{{- end }}

{{/* What the stream's Service and NetworkPolicy select (never the server's pods). */}}
{{- define "opence-server.streamSelectorLabels" -}}
app.kubernetes.io/name: {{ include "opence-server.name" . }}-stream
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end }}

{{- define "opence-server.streamLabels" -}}
{{ include "opence-server.streamSelectorLabels" . }}
app.kubernetes.io/component: stream
app.kubernetes.io/managed-by: {{ .Release.Service }}
helm.sh/chart: {{ printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" }}
{{- end }}
