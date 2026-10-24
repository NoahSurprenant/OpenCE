"""The manifest of game data a private server of the web build serves
(tools/web_server_data_manifest.py; port/web/README.md, "Serving the game
data privately"), on folders made here with stand-in files.

    python -m pytest -q tools/test_web_server_data_manifest.py
"""

import hashlib
import json
import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import web_server_data_manifest as tool  # noqa: E402


def write(path: Path, data: bytes) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


@pytest.fixture
def data(tmp_path):
    maps = tmp_path / "maps"
    write(maps / "ui.map", b"ui" * 1000)
    write(maps / "bloodgulch.map", b"bg")
    write(maps / "loading.tga", b"tga")
    write(maps / "readme.txt", b"not a map")
    write(maps / ".hidden.map", b"hidden")
    write(maps / "sub" / "a10.map", b"in a subfolder")
    skate = tmp_path / "skate-data"
    write(skate / "assets" / "board.json", b"{}")
    write(skate / "assets" / "meshes" / "deck.bin", b"\x00\x01")
    write(skate / "assets" / ".DS_Store", b"mac")
    write(skate / "sounds" / "push.wav", b"RIFF")
    write(skate / "conversion.log", b"log")
    write(skate / "other" / "x.bin", b"x")
    return tmp_path


def test_lists_what_the_importers_take(data):
    manifest = tool.build_manifest(data / "maps", data / "skate-data")
    assert manifest["format"] == "opence-server-data-manifest"
    assert manifest["version"] == 1
    assert manifest["maps"]["base"] == "maps/"
    assert [item["path"] for item in manifest["maps"]["files"]] == ["bloodgulch.map", "loading.tga", "ui.map"]
    assert manifest["skate"]["base"] == "skate-data/"
    assert [item["path"] for item in manifest["skate"]["files"]] == [
        "assets/board.json", "assets/meshes/deck.bin", "sounds/push.wav"]


def test_sizes_and_hashes(data):
    manifest = tool.build_manifest(data / "maps")
    ui = next(item for item in manifest["maps"]["files"] if item["path"] == "ui.map")
    assert ui["size"] == 2000
    assert ui["sha256"] == hashlib.sha256(b"ui" * 1000).hexdigest()
    assert "skate" not in manifest


def test_hashed_chunks_leave_the_page_cache(tmp_path, monkeypatch):
    if not hasattr(os, "posix_fadvise"):
        pytest.skip("no posix_fadvise here")
    calls = []
    monkeypatch.setattr(tool, "HASH_CHUNK", 1000)
    monkeypatch.setattr(os, "posix_fadvise", lambda fd, offset, length, advice: calls.append((offset, length, advice)))
    path = write(tmp_path / "ui.map", b"u" * 2500)
    assert tool.sha256(path) == hashlib.sha256(b"u" * 2500).hexdigest()
    assert calls == [(0, 1000, os.POSIX_FADV_DONTNEED), (1000, 1000, os.POSIX_FADV_DONTNEED),
                     (2000, 500, os.POSIX_FADV_DONTNEED)]


def test_symbolic_links_are_skipped(data):
    outside = write(data / "outside" / "secret.map", b"secret")
    os.symlink(outside, data / "maps" / "linked.map")
    os.symlink(data / "outside", data / "skate-data" / "assets" / "linked")
    manifest = tool.build_manifest(data / "maps", data / "skate-data")
    paths = [item["path"] for part in ("maps", "skate") for item in manifest[part]["files"]]
    assert not any("linked" in path or "secret" in path for path in paths)


def test_names():
    assert tool.map_wanted("ui.map") and tool.map_wanted("LOADING.TGA") and tool.map_wanted("my map.map")
    assert not tool.map_wanted(".ui.map") and not tool.map_wanted("ui.txt") and not tool.map_wanted("a;b.map")
    assert not tool.map_wanted("x" * 61 + ".map")
    assert tool.skate_name("board.json") and tool.skate_name("a b (1).bin")
    assert not tool.skate_name(".DS_Store") and not tool.skate_name("a\\b") and not tool.skate_name("a\nb")


def test_writes_the_manifest_and_the_config(data, capsys):
    output = data / "out" / "manifest.json"
    config = data / "out" / "server-data.json"
    assert tool.main(["--maps", str(data / "maps"), "--skate-data", str(data / "skate-data"),
                      "--output", str(output), "--config", str(config), "--manifest-url", "data/manifest.json",
                      "--maps-url", "halo-maps"]) == 0
    manifest = json.loads(output.read_text())
    assert manifest["maps"]["base"] == "halo-maps/"
    assert json.loads(config.read_text()) == {"format": "opence-server-data", "version": 1,
                                              "manifest": "data/manifest.json"}
    assert not list((data / "out").glob("*.partial"))
    assert "maps: 3 files, 2005 bytes" in capsys.readouterr().out


def test_warns_without_ui_map(tmp_path, capsys):
    write(tmp_path / "maps" / "bloodgulch.map", b"bg")
    assert tool.main(["--maps", str(tmp_path / "maps"), "--output", str(tmp_path / "manifest.json")]) == 0
    assert "no ui.map" in capsys.readouterr().err


@pytest.mark.parametrize("url", ["https://example.com/manifest.json", "//example.com/m.json", "/abs/m.json", "a\\b"])
def test_refuses_addresses_that_are_not_relative(tmp_path, url):
    write(tmp_path / "maps" / "ui.map", b"ui")
    with pytest.raises(SystemExit):
        tool.main(["--maps", str(tmp_path / "maps"), "--output", str(tmp_path / "m.json"), "--manifest-url", url])


def test_a_missing_folder_fails(tmp_path, capsys):
    assert tool.main(["--maps", str(tmp_path / "nothing"), "--output", str(tmp_path / "m.json")]) == 1
    assert "is not a folder" in capsys.readouterr().err
