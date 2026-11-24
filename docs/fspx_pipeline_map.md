# FSPX pipeline map: the 29 FidelityFX programs against what the boot asks for

Per program: dispatch state from `fspx/<name>.json` (kind 0); "translated" means the name has an entry in the build's
`spirv/fspx/index.json` and a `.spv` beside it; "requested" counts the `no translated compute shader for FSPX` log
lines that name the program's kind-6 key (last boot with the 42 refusals).

| program | wave32 | threads | translated | requested | times |
|---:|:---:|:---:|:---:|:---:|---:|
| 00 | no | 64×1×1 | yes | no | 0 |
| 01 | no | 256×1×1 | yes | no | 0 |
| 02 | yes | 32×1×1 | yes | no | 0 |
| 03 | yes | 128×1×1 | no | no | 0 |
| 04 | yes | 32×1×1 | no | no | 0 |
| 05 | yes | 32×1×1 | no | no | 0 |
| 06 | no | 64×1×1 | no | no | 0 |
| 07 | yes | 128×1×1 | no | no | 0 |
| 08 | no | 64×1×1 | no | no | 0 |
| 09 | no | 64×1×1 | no | no | 0 |
| 10 | yes | 32×1×1 | yes | no | 0 |
| 11 | yes | 32×1×1 | yes | no | 0 |
| 12 | yes | 64×1×1 | no | no | 0 |
| 13 | yes | 32×1×1 | yes | no | 0 |
| 14 | yes | 32×1×1 | yes | no | 0 |
| 15 | yes | 256×1×1 | no | no | 0 |
| 16 | yes | 32×1×1 | yes | no | 0 |
| 17 | yes | 32×1×1 | yes | no | 0 |
| 18 | yes | 64×1×1 | no | no | 0 |
| 19 | yes | 32×1×1 | yes | no | 0 |
| 20 | yes | 32×1×1 | yes | no | 0 |
| 21 | yes | 32×1×1 | yes | no | 0 |
| 22 | yes | 32×1×1 | yes | no | 0 |
| 23 | yes | 32×1×1 | yes | no | 0 |
| 24 | yes | 32×1×1 | yes | no | 0 |
| 25 | yes | 32×1×1 | yes | no | 0 |
| 26 | yes | 32×1×1 | yes | no | 0 |
| 27 | yes | 256×1×1 | yes | no | 0 |
| 28 | yes | 64×1×1 | no | no | 0 |

**Requested programs without a translation: 0 of the 29 above; none of them is requested at all.** All 42 refusals
(39 distinct kind-6 keys; the 11 untranslated programs above are not among them) are the title's own precompiled
compute pipelines deserialized from the pipeline library in `shaders.arc`: 31 of the 39 keys are byte-identical to
that library's first 31 compute programs, and the 11 remaining calls match its compute records 31-41 by program size
and layout-blob size
(not by bytes), so none of them is a FidelityFX program of this set, and none of the 39 has a translation in the
corpus. What that implies for the boot: translating more of the 29 programs cannot reduce the 42 refusals or move the
stop after them; the build has to unpack the pipeline library's compute programs (not a `make_game.py` step today),
translate and index them by kind-6 and kind-0 keys, and the translator's first gaps after that are the four
16x16-thread wave64 depth-copy/downsample programs it refuses today as `CS thread-group-size SGPR`.
