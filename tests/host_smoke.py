# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alejandro Bustos
"""Optional Linux host check, using a private dummy JACK server.

Needs jackd, jalv, Ingen, python3-jack-client and numpy. Never stops an
existing engine or server. Logs are kept in build/host-smoke/.
"""
import argparse
import math
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import time

import jack
import numpy as np

URI = "https://github.com/ABustos2606/reel#reel"
LFO_URI = "https://github.com/ABustos2606/reel#lfo"
PREFIXES = """@prefix patch: <http://lv2plug.in/ns/ext/patch#> .
@prefix ingen: <http://drobilla.net/ns/ingen#> .
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .
"""
TONE = 0.5


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def wait_for(check, message, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.05)
    raise RuntimeError(message)


class Protocol:
    """Small request/response client for Ingen's patch protocol (from Discipline)."""
    def __init__(self, path, transcript):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.connect(str(path))
        self.buffer = b""
        self.sequence = 0
        self.transcript = transcript.open("w")
        self.last_status = 0

    def request(self, verb, fields, allow_error=False):
        self.sequence += 1
        message = (PREFIXES + f'[] a patch:{verb} ; patch:sequenceNumber '
                   f'"{self.sequence}"^^xsd:int ; {fields} .\n')
        self.socket.sendall(message.encode())
        self.transcript.write("SEND " + message + "\n")
        deadline = time.monotonic() + 10
        while True:
            while b"\0" not in self.buffer:
                remaining = deadline - time.monotonic()
                require(remaining > 0, f"Ingen {verb} response timed out")
                self.socket.settimeout(remaining)
                data = self.socket.recv(65536)
                require(data, "Ingen closed its socket")
                self.buffer += data
            data, self.buffer = self.buffer.split(b"\0", 1)
            text = data.decode()
            self.transcript.write("RECV " + text + "\n")
            statuses = []
            for match in re.finditer(r"a patch:Response\s*;(.*?)\.\s*(?:\n|$)", text, re.S):
                sequence = re.search(r'patch:sequenceNumber "(\d+)"', match[1])
                status = re.search(r'patch:body "(-?\d+)"', match[1])
                if sequence and int(sequence[1]) == self.sequence and status:
                    statuses.append(int(status[1]))
            if statuses:
                self.last_status = max(statuses, key=abs)
                require(allow_error or self.last_status == 0, f"Ingen {verb}: {text}")
                return text

    def close(self):
        self.socket.close()
        self.transcript.close()

    def put(self, subject, body):
        return self.request("Put", f"patch:subject <{subject}> ; patch:body [ {body} ]")

    def connect(self, tail, head):
        return self.put("ingen:/main/", f"a ingen:Arc ; ingen:tail <{tail}> ; ingen:head <{head}>")

    def set(self, block, symbol, value):
        return self.request("Set", f'patch:subject <ingen:/main/{block}/{symbol}> ; '
                            f'patch:property ingen:value ; patch:value "{value}"^^xsd:float')

    def value(self, path):
        text = self.request("Get", f"patch:subject <{path}>")
        values = re.findall(r'ingen:value "?([0-9.eE+-]+)', text)
        require(values, f"no value for {path}")
        return float(values[-1])


def near(value, expected, tolerance=0.03):
    return abs(value - expected) <= tolerance * max(abs(expected), 0.01)


# Two keyframes added through the controls: Frame, then Levels 1-4.
KEYFRAMES = [(0.2, (1.0, 0.5, 0.0, 0.25)), (0.8, (0.0, 0.25, 1.0, 0.75))]
SETTLE = 0.15  # seconds: several 1024-frame periods


def code(x):
    return int(x * 65535 + 0.5)


def vca_gain(level, response):
    """src/vca.hpp: the gain for a Level (0-1) at a Response (0-1)."""
    x = code(level) / 65535
    if x < 1.5 / 65535:
        return 0.0
    r = int(response * 255 + 0.5)
    balance = round(32767 * (r / 255) ** 1.5) / 32768
    floored = max(x, 1 / 4096)
    if balance == 0:
        return floored
    v_linear = -(2 / 3) * math.log10(floored)
    v_exponential = 2.5 * (1 - x)
    return 10 ** (-1.5 * (v_linear + balance * (v_exponential - v_linear)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--ingen", default="ingen")
    parser.add_argument("--period", type=int, default=1024,
                        help="dummy JACK period (default: 1024, tolerant of WSL scheduling)")
    args = parser.parse_args()
    logdir = Path("build/host-smoke").resolve()
    logdir.mkdir(parents=True, exist_ok=True)
    server_name = f"reel-smoke-{os.getpid()}"
    env = dict(os.environ, JACK_DEFAULT_SERVER=server_name, JACK_NO_AUDIO_RESERVATION="1")
    # A private copy on native storage (a bundle on WSL's /mnt/c is slow to page in).
    private_lv2 = Path(f"/tmp/reel-smoke-lv2-{os.getpid()}")
    shutil.copytree(args.bundle.resolve(), private_lv2 / "reel.lv2")
    env["LV2_PATH"] = str(private_lv2) + ":" + env.get("LV2_PATH", "/usr/local/lib/lv2:/usr/lib/lv2")
    processes, logs = [], []
    probe = protocol = None
    saved_patches = []

    def launch(name, command):
        log = (logdir / f"{name}.log").open("w")
        logs.append(log)
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, env=env)
        processes.append(process)
        return process

    def stop(process):
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)

    try:
        launch("jack", ["jackd", "--no-realtime", "--timeout", "5000", "--name", server_name,
                        "-d", "dummy", "-r", "48000", "-p", str(args.period)])
        wait_for(lambda: subprocess.run(["jack_lsp"], env=env, stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL).returncode == 0, "private JACK server did not start")
        probe = jack.Client("reel_probe", servername=server_name, no_start_server=True)
        source = probe.outports.register("tone")
        captures = [probe.inports.register(f"cap_{n}") for n in range(3)]
        state = {"level": 0.0, "phase": 0.0, "peak": [0.0] * 3, "finite": True}

        @probe.set_process_callback
        def process(frames):
            n = np.arange(frames)
            source.get_array()[:] = state["level"] * np.sin(2 * np.pi * 220.0 / 48000 * (n + state["phase"]))
            state["phase"] += frames
            for i, port in enumerate(captures):
                data = port.get_array()
                state["finite"] &= bool(np.isfinite(data).all())
                state["peak"][i] = max(state["peak"][i], float(np.abs(data).max()))

        probe.activate()

        def listen(seconds):
            state["peak"] = [0.0] * 3
            time.sleep(seconds)
            require(state["finite"], "finite output")
            return list(state["peak"])

        def capture(*ports):
            for port, cap in zip(ports, captures):
                probe.connect(port, cap)

        # jalv: Level 2 at 0.5, Levels 3 and 4 at 0: Out 2 is half the tone,
        # the Mix the tone times 1.5, Level out 2 a steady 0.5.
        host = launch("jalv", ["jalv", "-i", "-n", "reel_jalv", "-c", "ch_2=0.5", "-c", "ch_3=0", "-c", "ch_4=0", URI])
        wait_for(lambda: bool(probe.get_ports("reel_jalv:mix")), "jalv did not load Reel")
        time.sleep(0.5)
        require(host.poll() is None, "jalv failed during activation")
        for n in range(1, 5):
            probe.connect(source, f"reel_jalv:in_{n}")
        capture("reel_jalv:out_2", "reel_jalv:mix", "reel_jalv:level_2")
        state["level"] = 0.0
        require(max(listen(0.5)[:2]) == 0.0, "jalv: silent with silent input")
        state["level"] = TONE
        out_2, mix, level_2 = listen(1.5)
        require(near(out_2, 0.5 * TONE) and near(mix, 1.5 * TONE) and near(level_2, 0.5, 0.001),
                f"jalv: Out 2 {out_2}, Mix {mix}, Level out 2 {level_2}")
        state["level"] = 0.0
        stop(host)
        print(f"PASS jalv: Reel loads; Out 2 {out_2:.3f}, Mix {mix:.3f} (tone {TONE}), Level out 2 {level_2:.3f}",
              flush=True)

        lfo = launch("jalv-lfo", ["jalv", "-i", "-n", "reel_lfo_jalv", LFO_URI])
        wait_for(lambda: bool(probe.get_ports("reel_lfo_jalv:out_4")), "jalv did not load Reel LFO")
        time.sleep(0.5)
        require(lfo.poll() is None, "jalv failed during Reel LFO activation")
        stop(lfo)
        print("PASS jalv: Reel LFO loads", flush=True)

        for threads in (1, 4):
            path = Path(f"/tmp/{server_name}-{threads}.sock")
            client = f"reel_ingen_{threads}"
            engine = launch(f"ingen-{threads}", [args.ingen, "-e", "-f", "-s", server_name, "-n", client,
                                                 "-S", str(path), "-E", "0", "-p", str(threads)])
            wait_for(path.exists, "Ingen socket did not appear")
            time.sleep(1)
            require(engine.poll() is None, "Ingen failed during JACK activation")
            protocol = Protocol(path, logdir / f"protocol-{threads}.log")
            protocol.put("ingen:/main/audio_in", "a lv2:InputPort, lv2:AudioPort")
            for name in ("out_a", "out_b"):
                protocol.put(f"ingen:/main/{name}", "a lv2:OutputPort, lv2:AudioPort")
            protocol.put("ingen:/main/cv_out", "a lv2:OutputPort, lv2:CVPort")
            # Two Reels in one graph (with several threads they can run at
            # once) and a Reel LFO.
            for name in ("reel", "reel2"):
                protocol.put(f"ingen:/main/{name}", f"a ingen:Block ; lv2:prototype <{URI}>")
                for n in range(1, 5):
                    protocol.connect("ingen:/main/audio_in", f"ingen:/main/{name}/in_{n}")
            protocol.put("ingen:/main/lfo", f"a ingen:Block ; lv2:prototype <{LFO_URI}>")
            protocol.connect("ingen:/main/reel/out_2", "ingen:/main/out_a")
            protocol.connect("ingen:/main/reel2/mix", "ingen:/main/out_b")
            protocol.connect("ingen:/main/reel/level_2", "ingen:/main/cv_out")
            protocol.set("reel", "ch_2", 0.5)
            wait_for(lambda: bool(probe.get_ports(f"{client}:out_a")), "Ingen JACK outputs")
            probe.connect(source, f"{client}:audio_in")
            capture(f"{client}:out_a", f"{client}:out_b", f"{client}:cv_out")
            state["level"] = 0.0
            time.sleep(0.5)
            require(max(listen(0.5)[:2]) == 0.0, "Ingen: silent with silent input")
            state["level"] = TONE
            time.sleep(0.5)
            out_a, out_b, cv = listen(1.5)
            require(near(out_a, 0.5 * TONE) and near(out_b, 4 * TONE) and near(cv, 0.5, 0.001),
                    f"Ingen: Out 2 {out_a}, second Reel's Mix {out_b}, Level out 2 {cv}")
            state["level"] = 0.0
            print(f"PASS Ingen: two Reels and a Reel LFO, {threads} threads; Out 2 at Level 0.5 {out_a:.3f}, "
                  f"the other's Mix {out_b:.3f} (tone {TONE}), Level out 2 {cv:.3f}", flush=True)

            # Keyframes through the controls, as a stomp or the page adds them:
            # move Frame, set the four Levels, pulse Add.
            for position, levels in KEYFRAMES:
                protocol.set("reel", "frame", position)
                time.sleep(SETTLE)
                for n, level in enumerate(levels, 1):
                    protocol.set("reel", f"ch_{n}", level)
                time.sleep(SETTLE)
                protocol.set("reel", "add", 1)
                time.sleep(SETTLE)
                protocol.set("reel", "add", 0)
                time.sleep(SETTLE)
            heard = []
            for position, levels in KEYFRAMES:
                protocol.set("reel", "frame", position)
                time.sleep(SETTLE)
                heard.append(listen(0.3)[2])
                require(near(heard[-1], levels[1], 0.001),
                        f"Ingen: Level out 2 {heard[-1]} at Frame {position}, keyframe level {levels[1]}")
            print(f"PASS Ingen: two keyframes added through Frame, Level and Add; Level out 2 "
                  f"{heard[0]:.3f} and {heard[1]:.3f} at them; {threads} threads", flush=True)

            # Level 2 at 0.6 and 3 at 0 with Frame between the keyframes edit
            # none; then Frame moves onto the second keyframe (Level 2 0.25)
            # and the patch is saved so. A host that loaded the Level values
            # as moves would edit that keyframe; the reload checks it didn't.
            protocol.set("reel", "frame", 0.5)
            time.sleep(SETTLE)
            settings = {"ch_2": 0.6, "ch_3": 0, "frame_cv_amount": -0.5, "easing_1": 3,
                        "response_2": 0.4, "mode": 1, "output": -6, "enabled": 0, "frame": 0.8}
            for symbol, value in settings.items():
                protocol.set("reel", symbol, value)
                time.sleep(0.03)
            time.sleep(SETTLE)
            save_path = logdir / f"{server_name}-{threads}.ingen"
            protocol.request("Copy", f"patch:subject <ingen:/main> ; patch:destination <{save_path.as_uri()}>")
            protocol.request("Delete", "patch:subject <ingen:/main/reel>")
            protocol.request("Copy", f"patch:subject <{save_path.as_uri()}> ; patch:destination <ingen:/main/restored>")
            for symbol, value in settings.items():
                got = protocol.value(f"ingen:/main/restored/reel/{symbol}")
                require(abs(got - value) < 1e-5, f"saved {symbol} restores: {got}")
            # Ingen writes each block's state to <block>/state.ttl in the bundle.
            saved = (save_path / "reel" / "state.ttl").read_text()
            require(re.search(r'<https://github.com/ABustos2606/reel#version> "1"\^\^xsd:int', saved),
                    "Reel's LV2 state (reel#version 1) is saved with the patch")
            require("reel#keyframes" in saved and all(str(code(p)) in saved for p, _ in KEYFRAMES),
                    "the keyframes are saved with the patch")
            require("reel#keyframes" not in (save_path / "reel2" / "state.ttl").read_text(),
                    "the other Reel saves none")
            require(engine.poll() is None, "Ingen exited")
            print(f"PASS Ingen: controls and keyframes saved with the patch, which reloads; {threads} threads",
                  flush=True)
            protocol.close()
            protocol = None
            stop(engine)
            path.unlink(missing_ok=True)

            # A fresh engine loads the saved patch: the keyframes come back,
            # with the saved Response 2 (0.4) applied to them.
            engine = launch(f"ingen-{threads}-reload", [args.ingen, "-e", "-f", "-s", server_name, "-n", client,
                                                        "-S", str(path), "-E", "0", "-p", str(threads)])
            wait_for(path.exists, "Ingen socket did not appear (reload)")
            time.sleep(1)
            require(engine.poll() is None, "Ingen failed during JACK activation (reload)")
            protocol = Protocol(path, logdir / f"protocol-{threads}-reload.log")
            protocol.put("ingen:/main/cv_out", "a lv2:OutputPort, lv2:CVPort")
            protocol.request("Copy", f"patch:subject <{save_path.as_uri()}> ; patch:destination <ingen:/main/restored>")
            protocol.connect("ingen:/main/restored/cv_out", "ingen:/main/cv_out")
            wait_for(lambda: bool(probe.get_ports(f"{client}:cv_out")), "Ingen JACK outputs (reload)")
            capture(f"{client}:cv_out", f"{client}:cv_out", f"{client}:cv_out")
            protocol.set("restored/reel", "enabled", 1)
            heard = []
            for position, levels in KEYFRAMES:
                protocol.set("restored/reel", "frame", position)
                time.sleep(SETTLE)
                heard.append(listen(0.3)[0])
                expected = vca_gain(levels[1], 0.4)
                require(near(heard[-1], expected, 0.002),
                        f"reloaded: Level out 2 {heard[-1]} at Frame {position}, expected {expected}")
            require(engine.poll() is None, "Ingen exited (reload)")
            print(f"PASS Ingen: a fresh engine reloads the keyframes; Level out 2 {heard[0]:.4f} and "
                  f"{heard[1]:.4f} at them (Response 0.4); {threads} threads", flush=True)
            protocol.close()
            protocol = None
            stop(engine)
            path.unlink(missing_ok=True)
            saved_patches.append(save_path)

        # jalv loads the block state Ingen saved (lilv state, as Ingen writes
        # it). That holds Reel's LV2 state only (Ingen keeps the port values
        # in main.ttl), so the controls are at their defaults, Response 0:
        # Level out 2 is each keyframe's level 2 exactly. With no keyframes it
        # would be the Level 2 control, 1.
        jalv_state = saved_patches[0] / "reel"
        host = launch("jalv-state", ["jalv", "-n", "reel_jalv_state", "-l", str(jalv_state), "-c", "frame=0.8", URI])
        wait_for(lambda: bool(probe.get_ports("reel_jalv_state:level_2")), "jalv did not load the saved state")
        time.sleep(0.5)
        require(host.poll() is None, "jalv failed loading the saved state")
        capture("reel_jalv_state:level_2", "reel_jalv_state:level_2", "reel_jalv_state:level_2")
        heard = []
        for position, levels in reversed(KEYFRAMES):
            host.stdin.write(f"frame = {position}\n".encode())
            host.stdin.flush()
            time.sleep(SETTLE)
            heard.append(listen(0.3)[0])
            require(near(heard[-1], levels[1], 0.001),
                    f"jalv: Level out 2 {heard[-1]} at Frame {position}, keyframe level {levels[1]}")
        stop(host)
        print(f"PASS jalv: loads the state Ingen saved; Level out 2 {heard[1]:.3f} and {heard[0]:.3f} at the two "
              f"keyframes", flush=True)
    finally:
        if protocol:
            protocol.close()
        if probe:
            probe.close()
        for process in reversed(processes):
            if process.poll() is None:
                stop(process)
        for log in logs:
            log.close()
        shutil.rmtree(private_lv2, ignore_errors=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        sys.exit(f"FAIL host_smoke: {error} (see build/host-smoke/*.log)")
