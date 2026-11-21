# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alejandro Bustos
"""Check parsed Turtle against the C++ port ABI. Test dependency: rdflib."""
import pathlib
import re
import sys

try:
    import rdflib
    from rdflib import Namespace, RDF, RDFS, URIRef
except ImportError:
    sys.exit("FAIL ttl_check: install python3-rdflib (or pip install rdflib)")

ROOT = pathlib.Path(__file__).resolve().parents[1]
LV2 = Namespace("http://lv2plug.in/ns/lv2core#")
DOAP = Namespace("http://usefulinc.com/ns/doap#")
UNITS = Namespace("http://lv2plug.in/ns/extensions/units#")
STATE = Namespace("http://lv2plug.in/ns/ext/state#")
URID = Namespace("http://lv2plug.in/ns/ext/urid#")
ATOM = Namespace("http://lv2plug.in/ns/ext/atom#")
RSZ = Namespace("http://lv2plug.in/ns/ext/resize-port#")
REEL = URIRef("https://github.com/ABustos2606/reel#reel")
LFO = URIRef("https://github.com/ABustos2606/reel#lfo")


def four(*names):
    return [f"{name}_{n}" for name in names for n in range(1, 5)]


# Fixed in milestone 0 (implementation plan, section 3): symbols never change
# and new ports are appended.
PLUGINS = {
    REEL: {
        "enum": "Port", "file": "reel.ttl", "name": "Reel", "count": 41, "hosts": "reel-defaults",
        "class": LV2.MixerPlugin, "state": True,
        "permanent": four("in") + four("out") + ["mix", "frame", "frame_cv_amount"] + four("ch")
        + ["add", "delete", "clear_all"] + four("easing") + four("response") + ["mode", "output", "frame_cv"]
        + four("level") + ["trigger", "enabled", "t_frame", "t_count", "t_active", "t_nearest", "t_step", "t_keyframes"],
    },
    LFO: {
        "enum": "LfoPort", "file": "reel_lfo.ttl", "name": "Reel LFO", "count": 13, "hosts": "lfo-defaults",
        "class": LV2.OscillatorPlugin, "state": False,
        "permanent": ["rate", "shape", "shape_spread", "spread", "coupling", "rate_cv"] + four("out") + ["bpm", "sync", "note"],
    },
}
# "Frame" (the position control) and "keyframes" are ordinary words; the
# module's name, "Frames", is not.
FORBIDDEN = re.compile(r"mutable|\bframes\b|gillet|emilie", re.I)
TOGGLES = {"add", "delete", "clear_all", "enabled", "sync"}
# Upstream's EasingCurve order (keyframer.h), so the glue passes values through.
EASING = {0: "Step", 1: "Linear", 2: "In quartic", 3: "Out quartic", 4: "Sine", 5: "Bounce"}
MODE = {0: "Keyframes", 1: "Sequencer"}
# Reel LFO's Note, in src/reel_lfo.cpp's kNoteBeats order.
NOTE = {0: "8 bars", 1: "4 bars", 2: "2 bars", 3: "1 bar", 4: "1/2", 5: "1/4", 6: "1/8", 7: "1/16"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def declared_ports(enum):
    header = (ROOT / "src/ports.hpp").read_text(encoding="utf-8")
    body = re.search(rf"enum class {enum} : uint32_t \{{(.*?)\}};", header, re.S)
    require(body is not None, f"missing {enum} enum")
    declared = re.findall(r"(\w+)\s*=\s*(\d+),\s*//\s*(audio_in|audio_out|control_in|control_out|cv_in|cv_out|atom_out)", body[1])
    # C++ names that are keywords carry a trailing underscore (delete_).
    return {int(index): (symbol.rstrip("_"), kind) for symbol, index, kind in declared}, body[1], len(declared)


def check_plugin(bundle, uri, spec):
    expected, body, declared = declared_ports(spec["enum"])
    count = spec["count"]
    require(declared == len(expected) == count, f"{spec['enum']}: count/duplicate index")
    require(set(expected) == set(range(count)), f"{spec['enum']}: non-contiguous indices")
    require(re.search(r"count\s*=\s*" + str(count) + r"\b", body), f"{spec['enum']}: count sentinel")
    require([expected[i][0] for i in range(len(spec["permanent"]))] == spec["permanent"],
            f"{spec['enum']}: permanent indices/symbols changed")

    graph = rdflib.Graph().parse(bundle / spec["file"], format="turtle")
    require((uri, RDF.type, LV2.Plugin) in graph and (uri, RDF.type, spec["class"]) in graph, f"{uri}: plugin class")
    # Peepo withholds an InstrumentPlugin as "played by MIDI notes"; Reel has no MIDI input.
    require((uri, RDF.type, LV2.InstrumentPlugin) not in graph, f"{uri}: not an instrument")
    require(graph.value(uri, DOAP.license) == URIRef("https://spdx.org/licenses/MIT"), "license")
    require(str(graph.value(uri, DOAP.name)) == spec["name"], "name")
    require(not FORBIDDEN.search(str(graph.value(uri, RDFS.comment))), "description names the original")
    require(not set(graph.objects(uri, LV2.requiredFeature)), "no required features")
    optional = set(graph.objects(uri, LV2.optionalFeature))
    require(LV2.hardRTCapable in optional, "hardRTCapable")
    extensions = set(graph.objects(uri, LV2.extensionData))
    if spec["state"]:
        require(STATE.interface in extensions and URID.map in optional, "state:interface, urid:map optional")
    else:
        require(not extensions, "no extension data")
    require(graph.value(uri, LV2.minorVersion) is not None and graph.value(uri, LV2.microVersion) is not None, "version")

    actual, symbols, defaults = {}, set(), {}
    for port in graph.objects(uri, LV2.port):
        index, symbol, name = int(graph.value(port, LV2["index"])), str(graph.value(port, LV2.symbol)), str(graph.value(port, LV2.name))
        require(index not in actual and symbol not in symbols, f"duplicate port {symbol}")
        symbols.add(symbol)
        require(re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", symbol), f"invalid symbol: {symbol}")
        require(name.isascii() and name.isprintable() and 0 < len(name) <= 16, f"{symbol}: name must be ASCII, <=16 characters")
        require(not FORBIDDEN.search(name), f"{symbol}: name names the original")
        types = set(graph.objects(port, RDF.type))
        directions = types & {LV2.InputPort, LV2.OutputPort}
        kinds = types & {LV2.AudioPort, LV2.ControlPort, LV2.CVPort, ATOM.AtomPort}
        require(len(directions) == len(kinds) == 1, f"{symbol}: ambiguous port kind")
        kind = {LV2.AudioPort: "audio", LV2.ControlPort: "control", LV2.CVPort: "cv", ATOM.AtomPort: "atom"}[next(iter(kinds))]
        kind += "_in" if LV2.InputPort in types else "_out"
        actual[index] = (symbol, kind)
        lo, hi = graph.value(port, LV2.minimum), graph.value(port, LV2.maximum)
        if LV2.ControlPort in types or LV2.CVPort in types:
            require(lo is not None and hi is not None and float(lo) <= float(hi), f"{symbol}: bounds")
            if LV2.InputPort in types:
                default = graph.value(port, LV2.default)
                require(default is not None and float(lo) <= float(default) <= float(hi), f"{symbol}: default")
                if LV2.ControlPort in types:
                    defaults[symbol] = float(default)
        props = set(graph.objects(port, LV2.portProperty))
        labels = {int(float(graph.value(p, RDF.value))): str(graph.value(p, RDFS.label))
                  for p in graph.objects(port, LV2.scalePoint)}
        if LV2.enumeration in props:
            require(LV2.integer in props and sorted(labels) == list(range(len(labels))), f"{symbol}: enum values")
            require(min(labels) == float(lo) and max(labels) == float(hi), f"{symbol}: enum bounds")
            require(all(label.isascii() for label in labels.values()), f"{symbol}: labels")
        if symbol.startswith("easing_"):
            require(labels == EASING and float(graph.value(port, LV2.default)) == 1,
                    f"{symbol}: labels follow EasingCurve, default Linear")
        if symbol == "mode":
            require(labels == MODE, "mode labels")
        if symbol == "note":
            require(labels == NOTE and float(graph.value(port, LV2.default)) == 3, "note: lengths, default 1 bar")
        if symbol == "bpm":
            require(graph.value(port, UNITS.unit) == UNITS.bpm and (float(lo), float(hi)) == (30, 300),
                    "bpm: Peepo's tempo range")
        if symbol in TOGGLES:
            require(LV2.toggled in props, f"{symbol}: toggled")
        if symbol == "enabled":
            require(graph.value(port, LV2.designation) == LV2.enabled, "enabled: designation")
        if symbol == "output":
            require(graph.value(port, UNITS.unit) == UNITS.db and (float(lo), float(hi)) == (-24, 12), "output: dB")
        if kind == "atom_out":
            # The keyframe list for the page (src/telemetry.hpp): strings in a
            # sequence, room for 64 keyframes.
            require(graph.value(port, ATOM.bufferType) == ATOM.Sequence and
                    set(graph.objects(port, ATOM.supports)) == {ATOM.String} and
                    int(graph.value(port, RSZ.minimumSize)) >= 4096, f"{symbol}: string sequence, 4096 bytes")
        if symbol.startswith("t_") and kind == "control_out" and symbol != "t_frame":
            require(LV2.integer in props, f"{symbol}: integer telemetry")
        if kind == "cv_in":
            require((float(lo), float(hi)) == (-1, 1), f"{symbol}: CV range is 1.0 = 5 V, +-5 V")
        if kind == "cv_out":
            require((float(lo), float(hi)) == (0, 1), f"{symbol}: CV outputs are 0 to 1")
    require(actual == expected, f"{spec['enum']}: ports differ: {actual}")
    check_host_defaults(defaults, spec["hosts"])
    return defaults


def check_host_defaults(defaults, name):
    """The test host starts every control at its Turtle default."""
    host = (ROOT / "tests/host.cpp").read_text(encoding="utf-8")
    body = re.search(rf"// {name}-begin(.*?)// {name}-end", host, re.S)
    require(body is not None, f"defaults: host.cpp {name} block")
    values = dict.fromkeys(defaults, 0.0)
    values.update({s: float(v) for s, v in re.findall(r"control\(\w+::(\w+)\) = (-?[0-9.]+)f;", body[1])})
    for symbol, value in defaults.items():
        require(abs(values[symbol] - value) < 1e-6,
                f"defaults: the test host gives {symbol} {values[symbol]}, the Turtle says {value}")


def check_fallbacks(defaults):
    """The C++ fallbacks for an unconnected or non-finite control agree with the Turtle."""
    glue = (ROOT / "src/reel.cpp").read_text(encoding="utf-8")
    # Port::delete_ is the C++ name of the symbol delete.
    fallbacks = {s.rstrip("_"): float(v) for s, v in re.findall(r"control\(Port::(\w+), (-?[0-9.]+)f,", glue)}
    # The four Level controls share one call: control(nth(Port::ch_1, c), ...).
    for s, v in re.findall(r"control\(nth\(Port::(\w+)_1, c\), (-?[0-9.]+)f,", glue):
        fallbacks.update({f"{s}_{n}": float(v) for n in range(1, 5)})
    require(len(fallbacks) >= 7, f"defaults: C++ fallbacks not found: {fallbacks}")
    for symbol, value in fallbacks.items():
        require(symbol in defaults and abs(defaults[symbol] - value) < 1e-6,
                f"defaults: {symbol} falls back to {value}, the Turtle says {defaults.get(symbol)}")


def check(bundle):
    manifest = rdflib.Graph().parse(bundle / "manifest.ttl", format="turtle")
    require(set(manifest.subjects(RDF.type, LV2.Plugin)) == set(PLUGINS), "manifest: two plugins")
    for uri, spec in PLUGINS.items():
        require(manifest.value(uri, LV2.binary) == URIRef((bundle / "reel.so").as_uri()), "manifest: binary")
        require(manifest.value(uri, RDFS.seeAlso) == URIRef((bundle / spec["file"]).as_uri()), "manifest: seeAlso")
    defaults = check_plugin(bundle, REEL, PLUGINS[REEL])
    check_fallbacks(defaults)
    check_plugin(bundle, LFO, PLUGINS[LFO])
    print(f"PASS ttl_check: {PLUGINS[REEL]['count']} + {PLUGINS[LFO]['count']} ports match ports.hpp; "
          "names, bounds, defaults, labels, license, state and bundle")


if __name__ == "__main__":
    try:
        check(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "reel.lv2").resolve())
    except Exception as error:
        sys.exit(f"FAIL ttl_check: {error}")
