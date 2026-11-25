"""One joaat rule across tools: every lowercased copy hashes a name as tooling/hashdict.py does.

    python -I tests/test_joaat_agreement.py

Each tool carries its own copy of Jenkins one-at-a-time, so nothing stopped one of them from encoding
the name differently. Two did: audio_config hashed the lowercased UTF-8 bytes, and world_names encoded
to UTF-8 first and then lower-cased the bytes (bytes.lower() only folds ASCII). Both gave a different
32-bit hash for any non-ASCII character, which surfaces as a stream id no bank stores or a name hash
that never resolves, with no error anywhere. This test imports every copy in tooling/ and compares it
with the dictionary the rest of the pipeline is pinned to.

Synthetic names only; no game data.
"""
import os
import re
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tooling")
for path in (ROOT, TOOLS):
    if path not in sys.path:
        sys.path.insert(0, path)

import hashdict  # noqa: E402

# ASCII names, then names with a non-ASCII character: "é" is one Latin-1 byte (0xE9) and two UTF-8
# bytes (0xC3 0xA9), and "ÉLÉGIE" also tells text.lower() apart from lower-casing the encoded bytes.
NAMES = [
    "adder", "plain_name", "burn", "boardwalk",
    "café", "Café", "Señor", "Dessert Name", "ÉLÉGIE", "Radio_LosSantos_Café",
]

# The modules whose name a reviewer would expect here: a rename must not turn the loop below into a
# test of nothing.
EXPECTED = ("anim_index", "audio_config", "awc_names", "hashdict", "mission_tables", "world_names")


def string_hash_copies():
    """{module name: function} for every ``def joaat(``/``def joaat32(`` that takes a string.

    ``tooling/save_keys.py`` defines ``joaat(data, seed=0)`` over bytes with a seed for the composition
    probe; it is a different hash and is covered by tests/test_save_keys.py.
    """
    copies = {}
    for filename in sorted(os.listdir(TOOLS)):
        if not filename.endswith(".py"):
            continue
        with open(os.path.join(TOOLS, filename), encoding="utf-8") as handle:
            source = handle.read()
        for match in re.finditer(r"^def (joaat32|joaat)\(\s*([A-Za-z_]\w*)", source, re.M):
            if match.group(2) == "data":
                continue
            module = __import__(filename[:-3])
            copies[filename[:-3]] = getattr(module, match.group(1))
    return copies


def runtime_joaat(name):
    """The C++ runtime's rule (port/src/mods.h joaat): the std::string_view bytes, ASCII-lowercased."""
    h = 0
    for byte in name.encode("utf-8"):
        if 0x41 <= byte <= 0x5A:
            byte += 0x20
        h = (h + byte) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    return (h + (h << 15)) & 0xFFFFFFFF


class JoaatAgreementTest(unittest.TestCase):
    def test_every_copy_matches_the_pinned_dictionary(self):
        copies = string_hash_copies()
        for module in EXPECTED:
            self.assertIn(module, copies)
        for module, function in sorted(copies.items()):
            for name in NAMES:
                with self.subTest(copy=module, name=name):
                    if module == "palmetto_mods":
                        # Measured contract: palmetto_mods hashes a mod.json name the way the C++ runtime does
                        # (port/src/mods.h joaat: the UTF-8 bytes of the std::string_view, ASCII-lowercased), not
                        # the Latin-1 pipeline dictionary below, so a non-ASCII mod name must match that rule.
                        # For ASCII names - the game names mods reference - both rules give the same hash.
                        self.assertEqual(function(name), runtime_joaat(name),
                                         f"{module} hashes {name!r} differently from port/src/mods.h joaat")
                    else:
                        self.assertEqual(function(name), hashdict.joaat(name),
                                         f"{module} hashes {name!r} differently from hashdict.joaat")

    def test_a_stream_id_is_the_name_hash_of_the_same_name(self):
        # The consequence of the bug: audio_config's id and the id awc_names recovers from a bank were
        # computed with two different encodings, so they could never match for an accented title.
        import audio_config
        import awc_names

        for title in ("Café", "Dessert Name", "Radio Station", "plain_song"):
            with self.subTest(title=title):
                self.assertEqual(audio_config.title_stream_id(title),
                                 awc_names.joaat(audio_config.normalized_name(title)) & 0x1FFFFFFF)


if __name__ == "__main__":
    unittest.main()
