#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alejandro Bustos
#
# Reel's Pi check (implementation plan, 6.4), modelled on Slope's: adds an
# Overdrive and a delay to the running Ingen graph, both fed from IN, and a
# Reel crossfading them into the patch's Mist ("clouds"); a Reel LFO
# modulates the delay's feedback and Mist's density. Three keyframes are
# added through the controls, as the page adds them. Then, for SECONDS: Frame
# moves every 200 ms as an encoder would for the first half, and Reel LFO's
# Out 1 drives Frame CV for the second; the graph is copied to a bundle every
# minute (Reel's state saved while it runs). JACK xruns, DSP load and the
# engine's RSS are recorded; the saved patch is reloaded afterwards by the
# caller. Run on the Pi as the peepo user, with the patch saved:
#   pi_check.py build SECONDS      the patch with Reel and Reel LFO
#   pi_check.py baseline SECONDS   the same patch without them
import json, math, re, socket, subprocess, sys, time

SOCK = '/run/peepo/ingen.sock'
M = 'ingen:/main'
REEL = 'https://github.com/ABustos2606/reel#reel'
LFO = 'https://github.com/ABustos2606/reel#lfo'
OD = 'http://drobilla.net/plugins/mda/Overdrive'
DLY = 'http://polyeffects.com/lv2/digit_delay'
PREFIXES = ''.join('@prefix %s: <%s> .\n' % p for p in [
    ('ingen', 'http://drobilla.net/ns/ingen#'), ('lv2', 'http://lv2plug.in/ns/lv2core#'),
    ('patch', 'http://lv2plug.in/ns/ext/patch#'), ('xsd', 'http://www.w3.org/2001/XMLSchema#')])
KEYFRAMES = [(0.1, (1.0, 0.0, 0, 0)), (0.5, (0.6, 0.6, 0, 0)), (0.9, (0.0, 1.0, 0, 0))]


class Ingen:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(SOCK)
        self.s.settimeout(0.2)
        self.seq = 1000
        self.buf = ''
        self.s.sendall(PREFIXES.encode())

    def _send(self, body):
        self.seq += 1
        text = '[]\n\ta patch:%s ;\n\tpatch:sequenceNumber "%d"^^xsd:int ;\n%s' % (body[0], self.seq, body[1])
        self.s.sendall(text.encode())
        return self._wait(self.seq)

    def _read(self):
        try:
            chunk = self.s.recv(65536)
            if chunk: self.buf += chunk.decode(errors='replace')
        except socket.timeout:
            pass
        if len(self.buf) > 4_000_000: self.buf = self.buf[-100000:]

    def _wait(self, seq):
        pat = re.compile(r'patch:Response\s*;\s*patch:sequenceNumber\s*"%d"\^\^xsd:int\s*;(?:\s*patch:subject\s*<[^>]*>\s*;)?\s*patch:body\s*"?(-?\d+)' % seq)
        until = time.time() + 10
        while time.time() < until:
            m = pat.search(self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return int(m.group(1))
            self._read()
        raise RuntimeError('no response to %d' % seq)

    def check(self, what, status):
        print(('PASS ' if status == 0 else 'FAIL ') + what + ('' if status == 0 else ' (status %d)' % status), flush=True)
        if status != 0: raise SystemExit(1)

    def block(self, sym, uri, x, y):
        self.check('add %s' % sym, self._send(('Put', '\tpatch:subject <%s/%s> ;\n\tpatch:body [\n\t\ta ingen:Block ;\n\t\tlv2:prototype <%s> ;\n\t\tingen:canvasX %.1f ;\n\t\tingen:canvasY %.1f\n\t] .\n' % (M, sym, uri, x, y))))

    def arc(self, tail, head):
        self.check('cable %s -> %s' % (tail, head), self._send(('Put', '\tpatch:subject <%s> ;\n\tpatch:body [\n\t\ta ingen:Arc ;\n\t\tingen:tail <%s/%s> ;\n\t\tingen:head <%s/%s>\n\t] .\n' % (M, M, tail, M, head))))

    def unarc(self, tail, head):
        self.check('uncable %s -> %s' % (tail, head), self._send(('Delete', '\tpatch:body [\n\t\ta ingen:Arc ;\n\t\tingen:tail <%s/%s> ;\n\t\tingen:head <%s/%s>\n\t] .\n' % (M, tail, M, head))))

    def set(self, port, value, quiet=False):
        st = self._send(('Set', '\tpatch:subject <%s/%s> ;\n\tpatch:property ingen:value ;\n\tpatch:value "%s"^^xsd:float .\n' % (M, port, repr(float(value)))))
        if not quiet or st: self.check('set %s = %s' % (port, value), st)

    def broadcast(self, port, on):
        return self._send(('Set', '\tpatch:subject <%s/%s> ;\n\tpatch:property ingen:broadcast ;\n\tpatch:value %s .\n' % (M, port, 'true' if on else 'false')))

    def copy(self, path):
        return self._send(('Copy', '\tpatch:subject <%s> ;\n\tpatch:destination <file://%s> .\n' % (M, path)))

    def keyframe_list(self, timeout=3):
        # Reel's t_keyframes, as the page watches it (ingen:activity, a string).
        self.broadcast('reel/t_keyframes', True)
        until = time.time() + timeout
        pat = re.compile(r'reel/t_keyframes>.*?ingen:activity.*?"(1:[0-9a-f]*)"', re.S)
        while time.time() < until:
            m = pat.search(self.buf)
            if m:
                self.buf = self.buf[m.end():]
                self.broadcast('reel/t_keyframes', False)
                return m.group(1)
            self._read()
        raise RuntimeError('no keyframe list from Reel')


def build(e, reel=True):
    e.block('od', OD, 312, 40)
    e.block('dly', DLY, 312, 220)
    for t, h in [('in_l', 'clouds/l_in'), ('in_r', 'clouds/r_in')]: e.unarc(t, h)
    for p, v in [('od/drive', 0.3), ('od/muffle', 0.2), ('od/output', 0.5), ('dly/BPM_0', 120), ('dly/Delay_1', 0.375),
                 ('dly/Feedback_4', 0.4), ('dly/Amp_5', 0.5), ('dly/EnableEcho_7', 1)]: e.set(p, v)
    for t, h in [('in_l', 'od/left_in'), ('in_r', 'od/right_in'), ('in_l', 'dly/in0')]: e.arc(t, h)
    if not reel:
        for t, h in [('od/left_out', 'clouds/l_in'), ('dly/out0', 'clouds/r_in')]: e.arc(t, h)
        return
    e.block('reel', REEL, 500, 120)
    e.block('reel_lfo', LFO, 500, 320)
    for t, h in [('od/left_out', 'reel/in_1'), ('dly/out0', 'reel/in_2'), ('reel/mix', 'clouds/l_in'), ('reel/mix', 'clouds/r_in'),
                 ('reel_lfo/out_2', 'dly/Feedback_4'), ('reel_lfo/out_3', 'clouds/density')]: e.arc(t, h)
    e.set('reel_lfo/rate', 0.45); e.set('reel_lfo/spread', 0.75)
    # Three keyframes, as the page adds them: Frame, the four Levels, Add pulsed.
    for position, levels in KEYFRAMES:
        e.set('reel/frame', position); time.sleep(0.1)
        for n, level in enumerate(levels, 1): e.set('reel/ch_%d' % n, level, quiet=True)
        time.sleep(0.1); e.set('reel/add', 1); time.sleep(0.1); e.set('reel/add', 0); time.sleep(0.1)
    listed = e.keyframe_list()
    count = (len(listed) - 2) // 24
    print(('PASS' if count == 3 else 'FAIL') + ' Reel lists %d keyframes on t_keyframes: %s' % (count, listed), flush=True)
    if count != 3: raise SystemExit(1)


def engine_pid():
    return subprocess.run(['pgrep', '-f', '-o', '^/opt/peepo/bin/ingen '], capture_output=True, text=True).stdout.split()[0]


def watch(seconds, e, reel=True):
    import jack
    client = jack.Client('reel_pi_check', no_start_server=True)
    xruns = []
    client.set_xrun_callback(lambda delay: xruns.append(delay))
    client.activate()
    pid = engine_pid()
    loads, rss, saves = [], [], 0
    start = time.time(); next_sample = start + 5; next_save = start + 60; lfo_on = False
    print('WATCH %d s, %d Hz, %d frames, engine pid %s' % (seconds, client.samplerate, client.blocksize, pid), flush=True)
    while time.time() - start < seconds:
        now = time.time()
        if reel:
            if now - start < seconds / 2:
                # An encoder: Frame sweeps back and forth across the keyframes.
                e.set('reel/frame', 0.5 + 0.45 * math.sin((now - start) * 0.5), quiet=True)
            elif not lfo_on:
                e.set('reel/frame', 0); e.arc('reel_lfo/out_1', 'reel/frame_cv'); lfo_on = True
                print('PASS Frame now driven by Reel LFO Out 1', flush=True)
            if now >= next_save:
                status = e.copy('/tmp/reel-pi-check-save.ingen'); saves += 1; next_save += 60
                if status: e.check('save %d' % saves, status)
        if now >= next_sample:
            next_sample += 5
            loads.append(client.cpu_load())
            with open('/proc/%s/status' % pid) as f:
                rss.append(int(next(l for l in f if l.startswith('VmRSS')).split()[1]))
            if len(loads) % 60 == 0:
                print('  %4d s: load mean %.1f%% max %.1f%%, xruns %d, RSS %d KiB, saves %d' % (now - start, sum(loads) / len(loads), max(loads), len(xruns), rss[-1], saves), flush=True)
        time.sleep(0.2)
    client.deactivate(); client.close()
    if reel:
        listed = e.keyframe_list()
        print(('PASS' if (len(listed) - 2) // 24 == 3 else 'FAIL') + ' the keyframes are still there: %s' % listed, flush=True)
    out = {'seconds': seconds, 'xruns': len(xruns), 'load_mean': sum(loads) / len(loads), 'load_max': max(loads),
           'rss_kib_first': rss[0], 'rss_kib_last': rss[-1], 'rss_kib_max': max(rss), 'saves': saves}
    print('RESULT ' + json.dumps(out), flush=True)


if __name__ == '__main__':
    mode = sys.argv[1]
    e = Ingen()
    build(e, reel=mode == 'build')
    watch(int(sys.argv[2]), e, reel=mode == 'build')
