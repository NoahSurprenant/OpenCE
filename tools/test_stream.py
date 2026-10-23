"""The headless stream mode (stream.enabled; port/linux/src/stream_output.c;
port/linux/README.md, "Streaming") in a real game on this machine: a
dedicated server with bots, and a stream of it read from its pipes as ffmpeg
would read them. The stream draws its picture with OpenGL (SDL's offscreen
driver, Mesa's llvmpipe will do), so it needs a build whose SDL has it and a
32-bit OpenGL: HALO_TEST_STREAM=1 says the machine has them (else the test is
skipped), with HALO_TEST_GAME and the Halo maps as tools/test_bots.py's
tests. On loopback addresses of its own (127.0.8.*)."""

import os
import re
import signal
import struct
import threading
import time

import pytest

from tools.test_bots import _log, _needs, _start, _stop, _wait

WIDTH, HEIGHT, FPS = 640, 480, 30
FRAME_BYTES = WIDTH * HEIGHT * 3


def _address(last):
    return f"{os.environ.get('HALO_TEST_STREAM_NET', '127.0.8')}.{last}"


class _Reader(threading.Thread):
    """a pipe read as ffmpeg reads it, whole chunks of chunk bytes, the
    newest keep of them kept (from when keeping is set)"""

    def __init__(self, path, chunk, keep):
        super().__init__(daemon=True)
        self.path, self.chunk, self.keep = path, chunk, keep
        self.chunks, self.count, self.keeping, self.stop = [], 0, False, False

    def run(self):
        with open(self.path, "rb", buffering=0) as pipe:
            data = b""
            while not self.stop:
                block = pipe.read(1 << 20)
                if not block:
                    return
                data += block
                while len(data) >= self.chunk:
                    piece, data = data[:self.chunk], data[self.chunk:]
                    self.count += 1
                    if self.keeping:
                        self.chunks.append(piece)
                        del self.chunks[:-self.keep]


def _server(tmp_path, game, maps, address):
    # (no network.broadcast: the server knows nothing of the stream, as a
    # server in a cluster does not; it answers the stream's search directly)
    return _start(tmp_path, "server", game, maps, {
        "HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "5", "HALO_SERVER_BOTS": "4", "HALO_BOT_SEED": "6",
        "HALO_NET_ADDRESS": address})


def test_stream_mode(tmp_path):
    """A stream (stream.enabled, 640x480 at 30 frames a second) of a dedicated
    server with four bots fighting: its picture's and sound's pipes are read
    (as ffmpeg reads them); it says it is streaming and writes its ready file
    at its first frame; once it watches the game, its frames are not all the
    same (the director's camera moves with the bots) and its sound is not
    silent (their fight). The server is given no address of the stream: it
    answers the stream's search directly. The server restarts: the stream joins
    its new game by itself. SIGTERM ends it in seconds, its ready file gone. About two
    minutes."""
    if os.environ.get("HALO_TEST_STREAM") != "1":
        pytest.skip("needs HALO_TEST_STREAM=1: a build whose SDL draws offscreen with OpenGL, and a 32-bit OpenGL")
    maps, paths = _needs("HALO_TEST_GAME", "HALO_TEST_SERVER")
    game, server = paths["HALO_TEST_GAME"], paths["HALO_TEST_SERVER"]
    server_address, stream_address = _address(11), _address(12)
    video, audio, ready = tmp_path / "video", tmp_path / "audio", tmp_path / "ready"
    os.mkfifo(video)
    os.mkfifo(audio)
    games = {"server": _server(tmp_path, server, maps, server_address)}
    executables = {"server": server, "stream": game}
    readers = []
    try:
        _wait(lambda: re.search(r"bots: \[BOT\] \w+ at tick \d+", _log(tmp_path, "server")), 180, games, tmp_path,
              "the server's bots did not play")
        games["stream"] = _start(tmp_path, "stream", game, maps, {
            "HALO_STREAM": "1", "HALO_STREAM_VIDEO": str(video), "HALO_STREAM_AUDIO": str(audio),
            "HALO_STREAM_READY_FILE": str(ready), "HALO_STREAM_WIDTH": str(WIDTH), "HALO_STREAM_HEIGHT": str(HEIGHT),
            "HALO_STREAM_FPS": str(FPS), "LP_NUM_THREADS": "2",
            "HALO_NET_ADDRESS": stream_address, "HALO_NET_BROADCAST": server_address})
        pictures = _Reader(video, FRAME_BYTES, 4 * FPS)
        sound = _Reader(audio, 48000 * 4 // 10, 100)  # (tenths of a second)
        readers = [pictures, sound]
        for reader in readers:
            reader.start()
        _wait(lambda: ready.is_file() and "stream: streaming" in _log(tmp_path, "stream"), 60, games, tmp_path,
              "the stream did not start")
        _wait(lambda: "stream: watching a game" in _log(tmp_path, "stream"), 90, games, tmp_path,
              "the stream did not watch the game")
        for reader in readers:
            reader.keeping = True
        _wait(lambda: len(pictures.chunks) >= 4 * FPS and len(sound.chunks) >= 100, 60, games, tmp_path,
              "the stream sent too little")
        # (llvmpipe compiles each shader the first time it draws with it: with
        # a cold shader cache - a new machine, a CI runner - the first seconds
        # in a game draw few new frames and the rest repeat. So the last 4
        # seconds are looked at until they change, for up to a minute)
        started = time.monotonic()
        while True:
            frames = list(pictures.chunks)
            distinct = len(set(frames))
            if distinct >= 10 or time.monotonic() - started > 60:
                break
            time.sleep(2)
        print(f"{pictures.count} frames read; of the last {len(frames)}, {distinct} different "
              f"({time.monotonic() - started:.0f} s after 4 s of them)")
        assert distinct >= 10, f"the stream's frames hardly change: {distinct} different of {len(frames)}"
        # (the frames are pictures: not all one colour)
        assert len(set(frames[-1][::3 * 97])) > 8, "the stream's last frame is all one colour"
        loudness = []
        for tenth in sound.chunks:
            samples = struct.unpack(f"<{len(tenth) // 2}h", tenth)
            loudness.append((sum(sample * sample for sample in samples) / len(samples)) ** 0.5)
        print(f"{sound.count} tenths of a second of sound read; loudest {max(loudness):.0f}, "
              f"median {sorted(loudness)[len(loudness) // 2]:.0f} (RMS of 32767)")
        assert max(loudness) > 300, f"the stream's sound is silent: {max(loudness):.0f}"
        for reader in readers:
            reader.keeping = False

        # the server goes and comes back: the stream finds its new game
        games["server"].kill()
        games["server"].wait()
        watched = _log(tmp_path, "stream").count("stream: watching a game")
        (tmp_path / "server").rename(tmp_path / "server-1")
        games["server"] = _server(tmp_path, server, maps, server_address)
        _wait(lambda: _log(tmp_path, "stream").count("stream: watching a game") > watched, 240, games, tmp_path,
              "the stream did not watch the server's new game")
        assert "network test: the game is gone; joining again" in _log(tmp_path, "stream")

        # SIGTERM: out in seconds, the ready file gone
        process = games.pop("stream")
        started = time.monotonic()
        process.send_signal(signal.SIGTERM)
        status = process.wait(timeout=15)
        print(f"the stream exited ({status}) {time.monotonic() - started:.1f} s after SIGTERM")
        assert status == 0 and time.monotonic() - started < 10
        assert not ready.exists(), "the stream left its ready file"
    finally:
        for reader in readers:
            reader.stop = True
        _stop(games, tmp_path, executables)
