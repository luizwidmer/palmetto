"""Generate, verify and self-test the local BCPack predictor file.

The console DirectStorage BCPack decoder keeps a 256-block initial endpoint and
index history for BC6H and BC7. Those tables are not public codec constants:
they are recovered from the player's own texture pools, which makes the
resulting predictor file game-derived. It must live **outside** this
repository; the runtime reads it from ``PALMETTO_DS_BCPACK_PREDICTOR`` and this
tool refuses to write it inside the repository.

Three subcommands, all read-only towards the package and standard library only:

    python -I tooling/bcpack_predictor.py generate <package-or-pool> --out <outside-repo.json>
    python -I tooling/bcpack_predictor.py validate <predictor.json-or-directory>
    python -I tooling/bcpack_predictor.py self-test [--out <outside-repo.json>]

``generate`` scans the package's ``.ttpf`` pools through the committed
calibration engine (``tooling/bcpack_history_calib.py``), validates what it
built against the format below, and writes one deterministic JSON file:
canonical key order and separators, a trailing newline, a ``format`` /
``formatVersion`` header and a SHA-256 of the table member it covers. The same
inputs give the same bytes on every run and platform. ``--dry-run`` runs the
whole path and reports the size and checksum without writing anything.

``validate`` reads a file, a combined file, or a ``bc6h.json`` / ``bc7.json``
directory with a bounded reader and checks every value range the decoder relies
on, without rewriting the file. A document without the header (the calibration
tool's own output) is accepted and reported as ``legacy``.

``self-test`` needs no game files: it builds a small synthetic sparse predictor
from a fixed seed, validates it, writes it outside the repository (a temp
directory unless ``--out`` is given) and validates the written file again.
The fixture exercises the index-keyed table form; it is not a usable predictor
for real blocks because the decoder treats unlisted rows as unknown.

Format, policy, determinism and limits are documented in
``docs/bcpack_predictor.md``. No path, count or byte from a user's package is
embedded in this file.
"""
import argparse
import hashlib
import json
import os
import sys
import tempfile

_TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)

import bcpack  # noqa: E402
import bcpack_history_calib as calib  # noqa: E402
import bcpack_tables_bptc as bptc  # noqa: E402
import ttpf_mips as ttpf  # noqa: E402
from report_paths import same_report_path  # noqa: E402

FORMAT_ID = "palmetto-bcpack-predictor"
FORMAT_VERSION = 1
ENVELOPE_KEYS = ("format", "formatVersion", "checksum")
CHECKSUM_ALGORITHM = "sha256"
# Domain separation: a raw table digest and a predictor-file digest are different values even for the same tables.
_CHECKSUM_DOMAIN = b"palmetto-bcpack-predictor\x00v1\x00"
# The runtime's bounded loader caps one predictor file at this size; validating a bigger file would accept what the
# loader refuses, so the same cap applies here (docs/bcpack_predictor.md).
MAX_FILE_BYTES = 8 * 1024 * 1024
REPOSITORY_ROOT = os.path.dirname(_TOOLS_DIR)
_SHA256_HEX = set("0123456789abcdef")


class PredictorError(Exception):
    """A user-facing predictor failure with a short reason name and one explanation line."""

    def __init__(self, reason, message):
        super().__init__(message)
        self.reason = reason


# ---------------------------------------------------------------------------
# Canonical encoding and checksum
# ---------------------------------------------------------------------------

def canonical_json(value):
    """One byte sequence for one value: sorted keys, no optional whitespace, ASCII only."""
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def _table_members(document):
    """Every top-level member the checksum covers: the document without its three header members."""
    return {key: value for key, value in document.items() if key not in ENVELOPE_KEYS}


def predictor_checksum(tables):
    """The SHA-256 of the canonical table object, domain-separated from a bare table digest."""
    payload = canonical_json(tables).encode("utf-8")
    return hashlib.sha256(_CHECKSUM_DOMAIN + payload).hexdigest()


def encode_predictor(tables):
    """Return `(bytes, checksum)` for the deterministic predictor file of `tables`."""
    tables = {name: tables[name] for name in sorted(tables)}
    checksum = predictor_checksum(tables)
    document = {
        "format": FORMAT_ID,
        "formatVersion": FORMAT_VERSION,
        "checksum": {"algorithm": CHECKSUM_ALGORITHM, "value": checksum},
    }
    document.update(tables)
    return canonical_json(document).encode("utf-8") + b"\n", checksum


# ---------------------------------------------------------------------------
# Format validation (the rules the runtime loader and the BCn consumers rely on)
# ---------------------------------------------------------------------------

def _is_integer(value):
    """JSON `true`/`false` are Python bools, which are ints: they are not table numbers."""
    return isinstance(value, int) and not isinstance(value, bool)


def _entries(table, where):
    """Yield `(index, value)` for a 256-entry list or an index-keyed object, as the consumers accept."""
    if isinstance(table, list):
        if len(table) != 256:
            raise PredictorError("bad_length", f"{where}: expected 256 entries, found {len(table)}")
        for index, value in enumerate(table):
            yield index, value
        return
    if isinstance(table, dict):
        for key, value in table.items():
            if not isinstance(key, str) or not key or not key.isdigit():
                raise PredictorError("bad_length", f"{where}: {key!r} is not a decimal table index")
            index = int(key)
            if index > 255:
                raise PredictorError("out_of_range", f"{where}: index {index} outside 0..255")
            yield index, value
        return
    raise PredictorError("malformed", f"{where}: expected a 256-entry list or an index-keyed object")


def _integers(values, where, low, high):
    for value in values:
        if not _is_integer(value):
            raise PredictorError("malformed", f"{where}: not an integer")
        if value < low or value > high:
            raise PredictorError("out_of_range", f"{where}: {value} outside {low}..{high}")


def _flatten(value, where):
    """Flatten a nested-or-flat integer row, up to the three array levels the consumers accept."""
    out = []

    def walk(node, depth):
        if depth > 3:
            raise PredictorError("bad_length", f"{where}: nested deeper than three arrays")
        if isinstance(node, list):
            for item in node:
                walk(item, depth + 1)
        else:
            out.append(node)

    walk(value, 0)
    return out


def _row_table(table, where, length, low, high):
    """`{index: tuple(row)}` for a 256-row table of fixed-length integer rows (missing rows stay absent)."""
    rows = {}
    if table is None:
        return rows
    for index, row in _entries(table, where):
        values = _flatten(row, f"{where}[{index}]")
        if len(values) != length:
            raise PredictorError("bad_length", f"{where}[{index}]: expected {length} integers, found {len(values)}")
        _integers(values, f"{where}[{index}]", low, high)
        rows[index] = tuple(values)
    return rows


def _scalar_table(table, where, low, high):
    """`{index: value}` for a 256-entry table of single integers."""
    values = {}
    if table is None:
        return values
    for index, value in _entries(table, where):
        if not _is_integer(value):
            raise PredictorError("malformed", f"{where}[{index}]: not an integer")
        if value < low or value > high:
            raise PredictorError("out_of_range", f"{where}[{index}]: {value} outside {low}..{high}")
        values[index] = value
    return values


def _bc6h_endpoint_table(table, where):
    """`{index: ((4 ints per q) per channel)}`: BC6H endpoint history is [3 channels][4 components]."""
    rows = {}
    if table is None:
        return rows
    widths = (65535, 1023, 63, 63)  # component order W (16-bit base), X (10-bit), Y and Z (6-bit)
    for index, row in _entries(table, where):
        if not isinstance(row, list) or len(row) != 3:
            raise PredictorError("bad_length", f"{where}[{index}]: expected 3 channels")
        channels = []
        for channel, values in enumerate(row):
            if not isinstance(values, list) or len(values) != 4:
                raise PredictorError("bad_length", f"{where}[{index}][{channel}]: expected 4 components")
            for component, value in enumerate(values):
                _integers((value,), f"{where}[{index}][{channel}][{component}]", 0, widths[component])
            channels.append(tuple(values))
        rows[index] = tuple(channels)
    return rows


def _bc6h_depth_table(table, where):
    """`{index: ((confirmed leading bits) per component)}`: the same 3x4 shape, bounded by 16/10/6/6."""
    rows = {}
    if table is None:
        return rows
    widths = (16, 10, 6, 6)  # component order W, X, Y, Z
    for index, row in _entries(table, where):
        if not isinstance(row, list) or len(row) != 3:
            raise PredictorError("bad_length", f"{where}[{index}]: expected 3 channels")
        channels = []
        for channel, values in enumerate(row):
            if not isinstance(values, list) or len(values) != 4:
                raise PredictorError("bad_length", f"{where}[{index}][{channel}]: expected 4 depths")
            for component, value in enumerate(values):
                _integers((value,), f"{where}[{index}][{channel}][{component}]", 0, widths[component])
            channels.append(tuple(values))
        rows[index] = tuple(channels)
    return rows


def _validate_bc6h(section):
    """Validate one BC6H predictor section and return its coverage counters."""
    if not isinstance(section, dict):
        raise PredictorError("malformed", "bc6h: section is not an object")
    if "endpointHistory" not in section and "indexHistory" not in section:
        raise PredictorError("malformed", "bc6h: neither endpointHistory nor indexHistory is present")
    endpoints = _bc6h_endpoint_table(section.get("endpointHistory"), "bc6h.endpointHistory")
    indices = _row_table(section.get("indexHistory"), "bc6h.indexHistory", 16, 0, 15)
    endpoint_class = _scalar_table(section.get("endpointClass"), "bc6h.endpointClass", 0, 3)
    index_class = _scalar_table(section.get("indexClass"), "bc6h.indexClass", 0, 3)
    endpoint_depths = _bc6h_depth_table(section.get("endpointConfirmedBits"), "bc6h.endpointConfirmedBits")
    index_depths = _row_table(section.get("indexConfirmedBits"), "bc6h.indexConfirmedBits", 16, 0, 4)
    return {
        "endpointHistory_rows": len(endpoints),
        "indexHistory_rows": len(indices),
        "endpointClass_confirmed": sum(1 for value in endpoint_class.values() if value == 3),
        "endpointClass_provisional": sum(1 for value in endpoint_class.values() if value == 1),
        "endpointClass_last_resort": sum(1 for value in endpoint_class.values() if value == 2),
        "indexClass_confirmed": sum(1 for value in index_class.values() if value == 3),
        "indexClass_provisional": sum(1 for value in index_class.values() if value == 1),
        "indexClass_last_resort": sum(1 for value in index_class.values() if value == 2),
        "endpointConfirmedSlots": sum(1 for channels in endpoint_depths.values()
                                       for channel in channels for depth in channel if depth > 0),
        "indexConfirmedSlots": sum(1 for row in index_depths.values() for depth in row if depth > 0),
    }


def _last_resort(table):
    """Count the BC7 endpoint last-resort entries, accepting the 2-int and the 4-int forms."""
    if table is None:
        return 0
    if not isinstance(table, list):
        raise PredictorError("malformed", "bc7.historyEndpointLastResort: expected a list")
    count = 0
    for entry in table:
        if not isinstance(entry, list):
            raise PredictorError("malformed", "bc7.historyEndpointLastResort: entry is not a list")
        if len(entry) == 4:
            _integers(entry, "bc7.historyEndpointLastResort[k, sub, endpoint, channel]", 0, 255)
            index, subset, endpoint, channel = entry
            if subset > 2 or endpoint > 1 or channel > 3:
                raise PredictorError(
                    "out_of_range",
                    f"bc7.historyEndpointLastResort: [{index}, {subset}, {endpoint}, {channel}] outside "
                    f"[0..255, 0..2, 0..1, 0..3]",
                )
        elif len(entry) == 2:
            _integers(entry, "bc7.historyEndpointLastResort[k, slot]", 0, 255)
            index, slot = entry
            if slot > 23:
                raise PredictorError(
                    "out_of_range", f"bc7.historyEndpointLastResort: [{index}, {slot}] has slot outside 0..23"
                )
        else:
            raise PredictorError(
                "bad_length", "bc7.historyEndpointLastResort: entry must have 2 or 4 integers"
            )
        count += 1
    return count


def _validate_bc7(section):
    """Validate one BC7 predictor section and return its coverage counters."""
    if not isinstance(section, dict):
        raise PredictorError("malformed", "bc7: section is not an object")
    if "historyEndpoints" not in section and "historyIndices" not in section:
        raise PredictorError("malformed", "bc7: neither historyEndpoints nor historyIndices is present")
    endpoints = _row_table(section.get("historyEndpoints"), "bc7.historyEndpoints", 24, 0, 255)
    endpoint_bits = _row_table(section.get("historyEndpointBits"), "bc7.historyEndpointBits", 24, 0, 8)
    endpoint_confirmed = _row_table(
        section.get("historyEndpointConfirmedBits"), "bc7.historyEndpointConfirmedBits", 24, 0, 8
    )
    indices = _row_table(section.get("historyIndices"), "bc7.historyIndices", 32, 0, 15)
    index_bits = _row_table(section.get("historyIndexBits"), "bc7.historyIndexBits", 32, 0, 4)
    index_confirmed = _row_table(
        section.get("historyIndexConfirmedBits"), "bc7.historyIndexConfirmedBits", 32, 0, 4
    )
    for index in sorted(set(endpoint_bits) & set(endpoint_confirmed)):
        for slot, (depth, confirmed) in enumerate(zip(endpoint_bits[index], endpoint_confirmed[index])):
            if confirmed > depth:
                raise PredictorError(
                    "out_of_range",
                    f"bc7.historyEndpointConfirmedBits[{index}][{slot}]={confirmed} exceeds the recorded "
                    f"historyEndpointBits depth {depth}",
                )
    for index in sorted(set(index_bits) & set(index_confirmed)):
        for slot, (depth, confirmed) in enumerate(zip(index_bits[index], index_confirmed[index])):
            if confirmed > depth:
                raise PredictorError(
                    "out_of_range",
                    f"bc7.historyIndexConfirmedBits[{index}][{slot}]={confirmed} exceeds the recorded "
                    f"historyIndexBits depth {depth}",
                )
    return {
        "historyEndpoints_rows": len(endpoints),
        "historyIndices_rows": len(indices),
        "endpointConfirmedSlots": sum(1 for row in endpoint_confirmed.values() for depth in row if depth > 0),
        "endpointKnownSlots": sum(1 for row in endpoint_bits.values() for depth in row if depth > 0),
        "indexConfirmedSlots": sum(1 for row in index_confirmed.values() for depth in row if depth > 0),
        "indexKnownSlots": sum(1 for row in index_bits.values() for depth in row if depth > 0),
        "historyEndpointLastResort": _last_resort(section.get("historyEndpointLastResort")),
    }


def _validate_envelope(document):
    """Validate the header members; return the claimed checksum, or None for a header-less legacy document."""
    has_format = "format" in document
    has_version = "formatVersion" in document
    if has_format != has_version:
        raise PredictorError("malformed", "format and formatVersion must appear together")
    if not has_format:
        if "checksum" in document:
            raise PredictorError("malformed", "a checksum without format/formatVersion is not a predictor header")
        return None
    if document["format"] != FORMAT_ID:
        raise PredictorError(
            "unsupported_format", f"{document['format']!r} is not {FORMAT_ID!r}"
        )
    version = document["formatVersion"]
    if not _is_integer(version) or version != FORMAT_VERSION:
        raise PredictorError(
            "unsupported_version",
            f"formatVersion {version!r} is not the supported version {FORMAT_VERSION}",
        )
    checksum = document.get("checksum")
    if not isinstance(checksum, dict):
        raise PredictorError("malformed", "a headed predictor needs a checksum object")
    if checksum.get("algorithm") != CHECKSUM_ALGORITHM:
        raise PredictorError(
            "malformed", f"checksum.algorithm must be {CHECKSUM_ALGORITHM!r}"
        )
    value = checksum.get("value")
    if (not isinstance(value, str) or len(value) != 64
            or any(character not in _SHA256_HEX for character in value)):
        raise PredictorError("malformed", "checksum.value must be 64 lowercase hexadecimal digits")
    return value


def validate_document(document):
    """Validate a parsed predictor document; return its header, coverage and whether it is header-less."""
    if not isinstance(document, dict):
        raise PredictorError("malformed", "the document root is not an object")
    checksum = _validate_envelope(document)
    tables = {}
    if "bc6h" in document or "bc7" in document:
        if "bc6h" in document:
            tables["bc6h"] = _validate_bc6h(document["bc6h"])
        if "bc7" in document:
            tables["bc7"] = _validate_bc7(document["bc7"])
    elif "endpointHistory" in document:
        tables["bc6h"] = _validate_bc6h(document)
    elif "historyEndpoints" in document:
        tables["bc7"] = _validate_bc7(document)
    else:
        raise PredictorError(
            "no_codec_section",
            "no bc6h/bc7 section (neither endpointHistory nor historyEndpoints at the top level)",
        )
    if checksum is not None:
        actual = predictor_checksum(_table_members(document))
        if actual != checksum:
            raise PredictorError(
                "checksum_mismatch", f"checksum {checksum} does not match the table members ({actual})"
            )
    header = {
        "format": document.get("format"),
        "formatVersion": document.get("formatVersion"),
        "legacy": checksum is None,
    }
    return header, tables


# ---------------------------------------------------------------------------
# File reading and the validate subcommand
# ---------------------------------------------------------------------------

def _predictor_files(path):
    """A directory is read as its bc6h.json/bc7.json files; anything else is read as one predictor file."""
    if not path:
        raise PredictorError("missing", "no predictor path was given")
    if os.path.isdir(path):
        candidates = [os.path.join(path, f"{codec}.json") for codec in ("bc6h", "bc7")]
        files = [candidate for candidate in candidates if os.path.isfile(candidate)]
        if not files:
            raise PredictorError("missing", f"{path}: no bc6h.json or bc7.json predictor file")
        return files
    if not os.path.exists(path):
        raise PredictorError("missing", f"{path}: no such predictor file or directory")
    return [path]


def _read_text(path):
    try:
        size = os.path.getsize(path)
    except OSError as error:
        raise PredictorError("unreadable", f"{path}: {error}") from error
    if size > MAX_FILE_BYTES:
        raise PredictorError(
            "too_large", f"{path}: {size} bytes is over the {MAX_FILE_BYTES}-byte predictor cap"
        )
    try:
        with open(path, "rb") as handle:
            raw = handle.read()
    except OSError as error:
        raise PredictorError("unreadable", f"{path}: {error}") from error
    try:
        return raw.decode("utf-8"), size
    except UnicodeDecodeError as error:
        raise PredictorError("malformed", f"{path}: not UTF-8 text ({error.reason})") from error


def _parse_document(text, path):
    def reject_constant(name):
        raise ValueError(f"{name} is not a number")

    try:
        return json.loads(text, parse_constant=reject_constant)
    except (json.JSONDecodeError, ValueError, RecursionError) as error:
        raise PredictorError("malformed", f"{path}: not JSON ({error})") from error


def validate_predictor(path):
    """Validate a predictor file or directory without writing anything; return a report dictionary."""
    report = {
        "path": os.path.abspath(path),
        "files": 0,
        "bytes": 0,
        "format": None,
        "formatVersion": None,
        "legacy": True,
        "checksum": None,
        "tables": {},
    }
    for file in _predictor_files(path):
        text, size = _read_text(file)
        document = _parse_document(text, file)
        header, tables = validate_document(document)
        report["files"] += 1
        report["bytes"] += size
        if header["legacy"]:
            report["legacy"] = report["legacy"] and True
        else:
            report["legacy"] = False
            report["format"] = header["format"]
            report["formatVersion"] = header["formatVersion"]
            report["checksum"] = document["checksum"]["value"]
        report["tables"].update(tables)
    return report


# ---------------------------------------------------------------------------
# Generation from the player's package
# ---------------------------------------------------------------------------

def resolve_pools(inputs):
    """Resolve package roots, ttdb folders and `.ttpf` files to a deterministic, deduplicated pool list."""
    pools = []
    for raw in inputs:
        path = os.path.abspath(os.path.expanduser(raw))
        found = ttpf.find_pools(path)
        if not found:
            raise PredictorError("input", f"{raw}: no .ttpf texture pool in it")
        pools.extend(found)
    unique = []
    seen = set()
    for pool in sorted(pools, key=lambda item: os.path.normcase(os.path.abspath(item))):
        key = os.path.normcase(os.path.abspath(pool))
        if key not in seen:
            seen.add(key)
            unique.append(pool)
    return unique


def check_offline_reader(tables):
    """Cross-check the tables with the committed BCPack decoder's own JSON reader (a second, independent shape check)."""
    if "bc6h" in tables:
        endpoints, indices, endpoint_class, index_class, endpoint_depths, index_depths = bptc.load_bc6h(
            {"bc6h": tables["bc6h"]}, with_confirmed_bits=True
        )
        if not (len(endpoints) == len(indices) == len(endpoint_class) == len(index_class) == 256):
            raise PredictorError("bad_length", "the BC6H decoder reader did not fill the 256-row tables")
        if len(endpoint_depths) != 256 or len(index_depths) != 256:
            raise PredictorError("bad_length", "the BC6H decoder reader did not fill the 256 depth rows")
    if "bc7" in tables:
        loaded = bptc.load_bc7({"bc7": tables["bc7"]})
        for key in ("history_endpoints", "history_endpoint_bits", "history_endpoint_confirmed_bits",
                    "history_indices", "history_index_bits", "history_index_confirmed_bits"):
            if len(loaded[key]) != 256:
                raise PredictorError("bad_length", f"the BC7 decoder reader did not fill {key}'s 256 rows")


def generate_tables(pools, *, limit=0, max_blocks=256):
    """Fit the BC6H and BC7 histories from the pools and return (tables, calibration statistics)."""
    calibrators = {6: calib.Bc6hCalibrator(), 7: calib.Bc7Calibrator()}
    calib.calibrate_from_pools(pools, calibrators, limit=limit, max_blocks=max_blocks)
    tables = {}
    stats = {}
    for codec, name in ((6, "bc6h"), (7, "bc7")):
        document, calibration = calibrators[codec].build_document()
        tables[name] = document
        stats[name] = calibration
    # The generated document must pass the same format rules a third party's file does before anything is written.
    _, coverage = validate_document(dict(tables))
    check_offline_reader(tables)
    return tables, stats, coverage


# ---------------------------------------------------------------------------
# Output policy and writing
# ---------------------------------------------------------------------------

def _inside_repository(path):
    root = os.path.normcase(os.path.realpath(REPOSITORY_ROOT))
    target = os.path.normcase(os.path.realpath(path))
    try:
        return os.path.commonpath([root, target]) == root
    except ValueError:  # two drives have no common path
        return False


def check_output_path(out, pools):
    """Policy guard: the predictor is game-derived, so it may not live in the repository or overwrite its inputs."""
    if not out:
        raise PredictorError("usage", "--out is required")
    out = os.path.abspath(os.path.expanduser(out))
    if os.path.isdir(out):
        raise PredictorError("usage", f"--out {out} is a directory; name the predictor file")
    if _inside_repository(out):
        raise PredictorError(
            "policy",
            f"refusing to write {out}: the predictor is game-derived and must stay outside the repository "
            f"(docs/bcpack_predictor.md)",
        )
    for pool in pools:
        if same_report_path(out, pool):
            raise PredictorError("usage", f"--out {out} would overwrite the input pool {pool}")
    return out


def write_predictor(out, data, pools):
    """Write `data` atomically to `out` after the policy guard; return the resolved output path."""
    out = check_output_path(out, pools)
    parent = os.path.dirname(out)
    os.makedirs(parent, exist_ok=True)
    handle, temporary = tempfile.mkstemp(prefix=".bcpack-predictor-", suffix=".tmp", dir=parent)
    try:
        with os.fdopen(handle, "wb") as stream:
            stream.write(data)
        os.replace(temporary, out)
    except OSError as error:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise PredictorError("write", f"{out}: {error}") from error
    return out


# ---------------------------------------------------------------------------
# Self-test fixture
# ---------------------------------------------------------------------------

_MASK64 = (1 << 64) - 1


def _synthetic_tables(seed=0x243F6A8885A308D3):
    """A small, deterministic, sparse predictor: six rows per table, every value inside the format's ranges."""
    state = [seed & _MASK64]

    def draw(bound):
        value = state[0]
        value ^= (value << 13) & _MASK64
        value ^= value >> 7
        value ^= (value << 17) & _MASK64
        state[0] = value & _MASK64
        return (value >> 33) % bound

    keys = ("0", "1", "7", "42", "127", "255")
    endpoint_widths = (65536, 1024, 64, 64)
    depth_widths = (17, 11, 7, 7)
    bc6h = {
        "endpointHistory": {
            key: [[draw(endpoint_widths[q]) for q in range(4)] for _ in range(3)] for key in keys
        },
        "indexHistory": {key: [draw(16) for _ in range(16)] for key in keys},
        "endpointClass": {key: draw(4) for key in keys},
        "indexClass": {key: draw(4) for key in keys},
        "endpointConfirmedBits": {
            key: [[draw(depth_widths[q]) for q in range(4)] for _ in range(3)] for key in keys
        },
        "indexConfirmedBits": {key: [draw(5) for _ in range(16)] for key in keys},
    }
    endpoint_bits = {key: [draw(9) for _ in range(24)] for key in keys}
    index_bits = {key: [draw(5) for _ in range(32)] for key in keys}
    bc7 = {
        "historyEndpoints": {key: [draw(256) for _ in range(24)] for key in keys},
        "historyEndpointBits": endpoint_bits,
        "historyEndpointConfirmedBits": {
            key: [draw(depth + 1) for depth in endpoint_bits[key]] for key in keys
        },
        "historyIndices": {key: [draw(16) for _ in range(32)] for key in keys},
        "historyIndexBits": index_bits,
        "historyIndexConfirmedBits": {
            key: [draw(depth + 1) for depth in index_bits[key]] for key in keys
        },
        "historyEndpointLastResort": [[0, 0], [7, 23], [42, 1, 1, 3]],
    }
    return {"bc6h": bc6h, "bc7": bc7}


def self_test_tables():
    """Build and validate the synthetic fixture tables (no game files)."""
    tables = _synthetic_tables()
    validate_document(dict(tables))
    check_offline_reader(tables)
    return tables


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _coverage_lines(tables):
    lines = []
    bc6h = tables.get("bc6h")
    if bc6h:
        lines.append(
            "BC6H rows: endpoints {endpointHistory_rows}/256, indices {indexHistory_rows}/256; "
            "endpoint classes confirmed {endpointClass_confirmed}, provisional {endpointClass_provisional}, "
            "last-resort {endpointClass_last_resort}; index classes confirmed {indexClass_confirmed}, "
            "provisional {indexClass_provisional}, last-resort {indexClass_last_resort}; "
            "confirmed endpoint slots {endpointConfirmedSlots}, index slots {indexConfirmedSlots}".format(**bc6h)
        )
    bc7 = tables.get("bc7")
    if bc7:
        lines.append(
            "BC7 rows: endpoints {historyEndpoints_rows}/256, indices {historyIndices_rows}/256; "
            "known endpoint slots {endpointKnownSlots}, confirmed {endpointConfirmedSlots}; "
            "known index slots {indexKnownSlots}, confirmed {indexConfirmedSlots}; "
            "last-resort entries {historyEndpointLastResort}".format(**bc7)
        )
    return lines


def _calibration_lines(stats):
    lines = []
    for name in sorted(stats):
        calibration = stats[name]
        if name == "bc6h":
            lines.append(
                "BC6H fit: reference blocks {reference_blocks} ({reference_conflicts} conflicts), "
                "pool chunks {pool_chunks} from {pool_textures} textures".format(**calibration)
            )
        else:
            lines.append(
                "BC7 fit: reference blocks {reference_blocks} ({reference_conflicts} conflicts), "
                "pool chunks {pool_chunks} from {pool_textures} textures; "
                "endpoint slots confirmed {endpoint_confirmed_slots}, provisional {endpoint_provisional_slots}, "
                "last-resort {endpoint_last_resort_slots}; index slots confirmed {index_confirmed_slots}, "
                "provisional {index_provisional_slots}".format(**calibration)
            )
    return lines


def _emit(args, report, progress, summary):
    """Print the JSON report, the progress lines plus the summary, or (quiet) only the summary."""
    if args.json:
        print(json.dumps(report, sort_keys=True, indent=2))
        return
    if not args.quiet:
        for line in progress:
            print(line, flush=True)
    print(summary, flush=True)


def _run_generate(args):
    if args.limit < 0:
        raise PredictorError("usage", "--limit must be non-negative")
    if args.max_blocks <= 0:
        raise PredictorError("usage", "--max-blocks must be positive")
    pools = resolve_pools(args.inputs)
    out = check_output_path(args.out, pools)
    if not args.json and not args.quiet:
        print(
            f"Found {len(pools)} texture pool file(s); scanning them read-only with the calibration engine...",
            flush=True,
        )
    tables, stats, coverage = generate_tables(pools, limit=args.limit, max_blocks=args.max_blocks)
    data, checksum = encode_predictor(tables)
    report = {
        "mode": "generate",
        "pools": len(pools),
        "out": out,
        "wrote": not args.dry_run,
        "bytes": len(data),
        "format": FORMAT_ID,
        "formatVersion": FORMAT_VERSION,
        "checksum": checksum,
        "tables": coverage,
        "calibration": stats,
    }
    lines = _calibration_lines(stats) + _coverage_lines(coverage)
    if args.dry_run:
        summary = f"Dry run: would write {len(data)} bytes (sha256 {checksum}) to {out}; nothing was written"
    else:
        write_predictor(out, data, pools)
        summary = f"Wrote predictor: {out} ({len(data)} bytes, sha256 {checksum})"
    _emit(args, report, lines, summary)
    return 0


def _run_validate(args):
    report = validate_predictor(args.path)
    report["mode"] = "validate"
    report["valid"] = True
    if report["legacy"]:
        summary = (
            f"Predictor is valid (legacy, no header): {report['path']} "
            f"({report['files']} file(s), {report['bytes']} bytes)"
        )
    else:
        summary = (
            f"Predictor is valid: {report['path']} ({report['files']} file(s), {report['bytes']} bytes, "
            f"format {report['format']} v{report['formatVersion']}, sha256 {report['checksum']})"
        )
    _emit(args, report, _coverage_lines(report["tables"]), summary)
    return 0


def _run_self_test(args):
    tables = self_test_tables()
    data, checksum = encode_predictor(tables)
    if args.out:
        out = check_output_path(args.out, [])
    else:
        out = check_output_path(
            os.path.join(tempfile.gettempdir(), "palmetto-bcpack-predictor-selftest", "predictor.json"), []
        )
    write_predictor(out, data, [])
    written = validate_predictor(out)
    report = {
        "mode": "self-test",
        "out": out,
        "bytes": len(data),
        "format": FORMAT_ID,
        "formatVersion": FORMAT_VERSION,
        "checksum": checksum,
        "tables": written["tables"],
        "synthetic": True,
    }
    progress = [
        "The fixture is synthetic and sparse (unlisted rows are unknown); it is not a usable real predictor.",
    ]
    progress.extend(_coverage_lines(written["tables"]))
    summary = f"Self-test predictor written and re-read: {out} ({len(data)} bytes, sha256 {checksum})"
    _emit(args, report, progress, summary)
    return 0


def build_arg_parser():
    parser = argparse.ArgumentParser(
        prog="bcpack_predictor.py",
        description=(
            "Generate, validate and self-test the local BCPack predictor file. The predictor holds the BC6H/BC7 "
            "initial endpoint and index history recovered from the player's own package; it is game-derived and "
            "must stay outside the repository (docs/bcpack_predictor.md)."
        ),
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    generate = subparsers.add_parser(
        "generate", help="scan the player's package and write the deterministic predictor file"
    )
    generate.add_argument(
        "inputs", nargs="+", metavar="PATH",
        help="package root, data/ttdb folder, or .ttpf pool file (read-only)",
    )
    generate.add_argument(
        "--out", required=True, metavar="PATH",
        help="predictor file to write, outside the repository (the parent folder is created if needed)",
    )
    generate.add_argument(
        "--limit", type=int, default=0,
        help="max mixed textures per codec to scan per run (0 = the whole package; use a small value for a smoke run)",
    )
    generate.add_argument(
        "--max-blocks", type=int, default=256,
        help="prefix blocks decoded per BCPack chunk while fitting (default: 256)",
    )
    generate.add_argument(
        "--dry-run", action="store_true",
        help="run the scan and report the size and checksum, but write nothing",
    )
    generate.add_argument("--json", action="store_true", help="print one machine-readable JSON report")
    generate.add_argument("--quiet", action="store_true", help="print only the final summary line")

    validate = subparsers.add_parser(
        "validate", help="validate an existing predictor file or directory without rewriting it"
    )
    validate.add_argument("path", metavar="PATH", help="predictor file, combined file, or bc6h.json/bc7.json folder")
    validate.add_argument("--json", action="store_true", help="print one machine-readable JSON report")
    validate.add_argument("--quiet", action="store_true", help="print only the final summary line")

    self_test = subparsers.add_parser(
        "self-test", help="build, validate and write a tiny synthetic fixture without game files"
    )
    self_test.add_argument(
        "--out", metavar="PATH", help="fixture path outside the repository (default: a temp directory)"
    )
    self_test.add_argument("--json", action="store_true", help="print one machine-readable JSON report")
    self_test.add_argument("--quiet", action="store_true", help="print only the final summary line")
    return parser


def run(argv=None):
    args = build_arg_parser().parse_args(argv)
    if args.command == "generate":
        return _run_generate(args)
    if args.command == "validate":
        return _run_validate(args)
    if args.command == "self-test":
        return _run_self_test(args)
    raise PredictorError("usage", f"unknown command {args.command!r}")


def main(argv=None):
    try:
        return run(argv)
    except PredictorError as error:
        print(f"ERROR: {error.reason}: {error}", file=sys.stderr)
        return 2
    except (calib.CliError, ValueError, OSError, json.JSONDecodeError, bcpack.BcpackError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    raise SystemExit(main())
