#!/usr/bin/env python3
"""Writes the manifest of game data that a private server of the web build
serves (port/web/README.md, "Serving the game data privately"):

    python tools/web_server_data_manifest.py --maps /srv/game-data/maps \\
        --skate-data /srv/game-data/skate-data --output /srv/game-data/manifest.json \\
        --config /srv/site/server-data.json --manifest-url game-data/manifest.json

The page (port/web/site/server-data.js) reads server-data.json beside
index.html, which names this manifest, and copies the files it lists into
the browser's storage, where the page's own importers put them: the maps
folder's maps (and loading.tga) into maps/, and Skate 3 mode's converted
data (skate-data/assets/... and skate-data/sounds/...) into skate-data/. It
checks each file's size and SHA-256 and, on a later visit, copies again only
the files whose hash changed.

The manifest (format "opence-server-data-manifest", version 1):

    {"format": "opence-server-data-manifest", "version": 1,
     "maps": {"base": "maps/", "files": [{"path": "ui.map", "size": 1234, "sha256": "..."}]},
     "skate": {"base": "skate-data/", "files": [{"path": "assets/board.json", ...}]}}

Each base is relative to the manifest's own address, and each file's path to
its base; "skate" is left out without --skate-data. Only what the page's
importers would take is listed: in the maps folder, the .map files and
loading.tga (no subfolders); in the skate-data folder, everything under
assets/ and sounds/ (conversion.log and the rest stay out). Hidden files and
symbolic links are skipped.

The config (--config; format "opence-server-data", version 1):

    {"format": "opence-server-data", "version": 1, "manifest": "game-data/manifest.json"}

with the manifest's address relative to the page (--manifest-url). The page
only follows addresses of its own origin.

Only for game data you own, on a private network: see the README.
"""

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Dict, List, Optional

MANIFEST_FORMAT = "opence-server-data-manifest"
CONFIG_FORMAT = "opence-server-data"
FORMAT_VERSION = 1
# as port/web/site/xiso-worker.js and server-data.js
MAXIMUM_MAP_NAME = 64
MAXIMUM_SKATE_NAME = 255
MAXIMUM_SKATE_DEPTH = 16
MAXIMUM_SKATE_FILES = 20000
SKATE_PARTS = ("assets", "sounds")
HASH_CHUNK = 4 << 20
MAP_NAME_CHARACTERS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.- ")


def map_name(name: str) -> bool:
    """a name a file of the maps folder may have (xiso-worker.js plainName)"""
    return (0 < len(name) <= MAXIMUM_MAP_NAME and not name.startswith(".")
            and all(character in MAP_NAME_CHARACTERS for character in name))


def map_wanted(name: str) -> bool:
    """what the page's maps importer copies: the maps, and loading.tga"""
    lower = name.lower()
    return map_name(name) and (lower.endswith(".map") or lower == "loading.tga")


def skate_name(name: str) -> bool:
    """a name a skate data file or folder may have (xiso-worker.js skateName)"""
    return (0 < len(name) <= MAXIMUM_SKATE_NAME and not name.startswith(".")
            and not any(character in "\\/" or ord(character) < 0x20 for character in name))


def sha256(path: Path) -> str:
    """the file's SHA-256, dropping each chunk from the page cache once hashed:
    the data is read once, and under a tight memory limit (the Helm chart's
    init container) a full page cache would otherwise be reclaimed inside the
    cgroup all the time, a path with a kernel bug in Debian's 6.12"""
    digest = hashlib.sha256()
    drop = getattr(os, "posix_fadvise", None)
    with path.open("rb", buffering=0) as source:
        offset = 0
        while True:
            chunk = source.read(HASH_CHUNK)
            if not chunk:
                break
            digest.update(chunk)
            if drop:
                drop(source.fileno(), offset, len(chunk), os.POSIX_FADV_DONTNEED)
            offset += len(chunk)
    return digest.hexdigest()


def entry(path: Path, relative: str) -> Dict:
    return {"path": relative, "size": path.stat().st_size, "sha256": sha256(path)}


def map_files(folder: Path) -> List[Dict]:
    files = []
    for path in sorted(folder.iterdir(), key=lambda item: item.name):
        if path.is_symlink() or not path.is_file() or not map_wanted(path.name):
            continue
        files.append(entry(path, path.name))
    return files


def skate_files(folder: Path) -> List[Dict]:
    found = []
    for part in SKATE_PARTS:
        top = folder / part
        if not top.is_dir() or top.is_symlink():
            continue
        for directory, subdirectories, names in os.walk(top):
            relative_dir = Path(directory).relative_to(folder)
            # (depth counts the file's own name, as the page does)
            subdirectories[:] = sorted(name for name in subdirectories
                                       if skate_name(name) and not (Path(directory) / name).is_symlink()
                                       and len(relative_dir.parts) + 2 <= MAXIMUM_SKATE_DEPTH)
            for name in sorted(names):
                path = Path(directory) / name
                if not skate_name(name) or path.is_symlink() or not path.is_file():
                    continue
                found.append((relative_dir / name).as_posix())
    found.sort()
    if len(found) > MAXIMUM_SKATE_FILES:
        raise ValueError(f"{folder} has {len(found)} files under assets/ and sounds/: is it skate-data?")
    return [entry(folder / relative, relative) for relative in found]


def build_manifest(maps: Path, skate_data: Optional[Path] = None, maps_url: str = "maps/",
                   skate_url: str = "skate-data/") -> Dict:
    if not maps.is_dir():
        raise ValueError(f"{maps} is not a folder")
    manifest = {"format": MANIFEST_FORMAT, "version": FORMAT_VERSION,
                "maps": {"base": maps_url, "files": map_files(maps)}}
    if skate_data is not None:
        if not skate_data.is_dir():
            raise ValueError(f"{skate_data} is not a folder")
        manifest["skate"] = {"base": skate_url, "files": skate_files(skate_data)}
    return manifest


def build_config(manifest_url: str) -> Dict:
    return {"format": CONFIG_FORMAT, "version": FORMAT_VERSION, "manifest": manifest_url}


def relative_url(value: str) -> str:
    """a relative address: the page refuses others' origins anyway, and a
    relative one keeps the data on the page's"""
    if not value or "://" in value or value.startswith("//") or value.startswith("/") or "\\" in value:
        raise argparse.ArgumentTypeError(f"{value!r}: give an address relative to the file that names it")
    return value


def folder_url(value: str) -> str:
    value = relative_url(value)
    return value if value.endswith("/") else value + "/"


def write_json(path: Path, value: Dict) -> None:
    """written whole, then renamed into place: a server never serves half of one"""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".partial")
    temporary.write_text(json.dumps(value, indent=1) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--maps", type=Path, required=True, help="the maps folder (ui.map and the others)")
    parser.add_argument("--skate-data", type=Path, help="Skate 3 mode's converted skate-data folder (optional)")
    parser.add_argument("--output", type=Path, required=True, help="the manifest to write")
    parser.add_argument("--maps-url", type=folder_url, default="maps/",
                        help="the maps folder's address, relative to the manifest's (default: maps/)")
    parser.add_argument("--skate-url", type=folder_url, default="skate-data/",
                        help="the skate-data folder's address, relative to the manifest's (default: skate-data/)")
    parser.add_argument("--config", type=Path, help="also write server-data.json, which names the manifest")
    parser.add_argument("--manifest-url", type=relative_url, default="game-data/manifest.json",
                        help="the manifest's address, relative to the page (default: game-data/manifest.json)")
    args = parser.parse_args(argv)
    try:
        manifest = build_manifest(args.maps, args.skate_data, args.maps_url, args.skate_url)
    except (OSError, ValueError) as error:
        print(f"web_server_data_manifest: {error}", file=sys.stderr)
        return 1
    names = {item["path"].lower() for item in manifest["maps"]["files"]}
    if "ui.map" not in names:
        print(f"web_server_data_manifest: warning: no ui.map in {args.maps}: the game needs it", file=sys.stderr)
    write_json(args.output, manifest)
    if args.config:
        write_json(args.config, build_config(args.manifest_url))
    for part in ("maps", "skate"):
        if part in manifest:
            files = manifest[part]["files"]
            size = sum(item["size"] for item in files)
            print(f"{part}: {len(files)} files, {size} bytes", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
