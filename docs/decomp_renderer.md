# The renderer command graph, the constant-table/LUT family, the upscaler kind branch and the present path: a static map

**Status: measured (static).** This page decompiles the retail image's renderer from the user's own copy with the
project's Ghidra tooling and maps four things on one page: the command-list node decoder and its node/record layouts,
the type-5 constant-table/LUT command family and its 256-entry tables, the own-upscaler kind branch (kind 6 vs kind 7),
and the present/presenter chain including where the presented plane is written and where colour conversion happens. It
is decompilation only: no boot was run for this page, no runtime code, test, switch or default path changed, no game
data (strings, asset bytes, hashes, package paths) appears here, and no other documentation page was edited. Every
row marked **measured (static)** is read from the decompiled code at the named RVA; rows marked **measured (boot)**
are quoted from the page named; rows marked **inferred** are this page's reading and are not claimed as measured.

RVAs are relative to the image's preferred base. The image is the same retail build the rest of the tree measures; the
Ghidra project and the decompiled function dumps are outside the repository.

## 1. The reader's chain in one table

| # | stage | code (RVA) | what it is | source |
|---:|---|---|---|---|
| 1 | renderer node declares a command | `0x1836D0..0x18392F` | case table on the node's type word, builds a 0x28-byte descriptor | measured (static) |
| 2 | descriptor is hash-consed into the command graph | `0x183B70..0x183F57` | intern table on the command-list object; 0x48-byte node | measured (static) |
| 3 | node owners walk their command lists | `0x182600`, `0x183A20`, `0x183190` | per-frame flush and re-link of the node lists | measured (static) |
| 4 | pass/draw objects record into the graph | `0x1829C0`, `0x182B60`, `0x184220`, `0x184850` | 0xa0-byte pass object (vtable `0x6632810`) and 0xe0/0xd0-byte recorders | measured (static) |
| 5 | type-5 node becomes a GPU constant table | `0x1851D0` -> `0x1854D0` -> `0x184770` -> `0x185570` -> `0x186170` -> `0x1335A20` | factory, command object, job, job runner, executor | measured (static) |
| 6 | table block is sized and filled | `0x45594F0`, `0x455A530`, `0x455A610`, `0x1555B60`, `0x1555C10`, `0x550E2D0` | constant-table module, block size, format stride, copy | measured (static) |
| 7 | upscaler kind is selected | `0x7E93F0`, `0x7E93B0`, `0x75140`, `0x55DEB88`, `0x56DCF00` | kind 6/7 selector, factory, console-type read, capability word | measured (static) |
| 8 | frame submissions consume the graph and present | `0x2481E90`, `0x24846D0`, `0xCD72B0`, `0xCD7320` | submission function, per-frame present job, present-state writer | measured (static) |

The two graphs are distinct and must not be conflated. The **node graph** (rows 1-4) is hash-consed per command-list
object and stores commands as values. The **submission ring** (row 8) is a 0x60-entry ring of 0x48-byte records that
the frame function replays onto the D3D12 command list and, in one record kind, into PresentX. Row 5-6 is the
constant-table job chain that the type-5 node starts, not the node graph itself.

## 2. The command-list node decoder (game+0x1836D0)

`0x1836D0..0x18392F` switches on `*param_2` (the declaration's first dword) and builds a 0x28-byte descriptor on its
stack. The switch has six cases; case 3 and case 6 share the block at `0x1836E5`; every other value falls to the
default path, which returns 0 without touching the caller's command slot. The case number is the **decode form**; the
node's own type word is the form's output, and for the forms that carry an object (3, 4, 5 and 6) it is the
declaration's own +0x04 dword.

| form | node type (+0x00) | +0x04 | +0x08 (u16 class) | +0x0A | +0x10 handle | +0x20 record | +0x28 flag |
|---:|---|---|---|---|---|---|---|
| 1 | 1 | 0 | 0 | 0 | 0 | declaration+0x10 | 0 |
| 2 | 2 | declaration+0x08 | 0 | 0 | 0 | declaration+0x10 | 0 |
| 3 | declaration+0x04 | 0 | 0 | 0 | 0 | declaration+0x10 | 0 |
| 4 | declaration+0x04 | 0 | declaration+0x18 low u16 | 0 | object+0x08's `vtable[0xA8]` | declaration+0x10 | declaration+0x1A bit 0 |
| 5 | declaration+0x04 | 0 | declaration+0x18 low u16 | 0 | command object's `vtable[0xA8]` (job) | declaration+0x10 | 1 |
| 6 | declaration+0x04 | declaration+0x08 | 0 | 0 | 0 | declaration+0x10 | 0 |

What each form does, measured (static):

| form | behaviour |
|---:|---|
| 1 | plain node, type 1: no object and no job; the record is the declaration's +0x10 pointer |
| 2 | plain node, type 2, sub-word declaration+0x08: no object and no job |
| 3 | node whose type is the declaration's +0x04: no object, no job |
| 4 | takes an object at declaration+0x08 and asks its slot +0xA8 for a GPU handle; node type = declaration+0x04, class = declaration+0x18, flag = declaration+0x1A bit 0; if the object is null or slot +0xA8 answers null it falls back to a type-2 node with no handle |
| 5 | calls the type-5 factory `0x1851D0(owner, declaration+0x08, float)`; stores the returned command object in the caller's slot (`*param_3`); node type = declaration+0x04, class = declaration+0x18, handle = the command's `vtable[0xA8]` result (the job from `0x184770`), flag = 1; if the factory returns null the decoder returns 0 |
| 6 | as 3, plus declaration+0x08 becomes the +0x04 sub-word |

For type 5 the declaration's record (the constant-table record the factory hashes) is at **declaration+0x08**, and the
descriptor's +0x20 record field is a **second pointer at declaration+0x10**. The interner compares that second pointer
byte for byte when two declarations differ, so a type-5 node is keyed by the constant-table record and by the
second descriptor together. That second descriptor's meaning is **inferred**: its bytes take part in node equality and
it is not read by the factory, the job chain or the copy.

### 2.1 The interned node object (game+0x183B70)

`0x183B70..0x183F57` looks the descriptor up in a hash table at owner+0x18 (open addressing, grow via `0x187110`) and
allocates a 0x48-byte node on a miss. The node is the renderer's command-graph value.

| offset | type | content |
|---:|---|---|
| +0x00 | qword | vtable `0x6631EF0` (slot +0 = `0x12DC9D0` refcount release, slot +8 = 0) |
| +0x08 | u32 | 1, the node's initial reference count |
| +0x0C | u32 | 0 |
| +0x10 | u32 | descriptor type word (1-6) |
| +0x14 | u32 | descriptor +0x04 |
| +0x18 | u16 | descriptor +0x08 |
| +0x1A | u16 | descriptor +0x0A |
| +0x20 | qword | descriptor +0x10 (GPU handle: 0 for types 1-3/6, object handle for 4, job for 5) |
| +0x28 | qword | descriptor +0x18 (written 0 by every path seen) |
| +0x30 | qword | descriptor +0x20 (record/descriptor pointer) |
| +0x38 | u8 | descriptor +0x28 flag |
| +0x40 | qword | owner command-list object |

The key hashes `handle >> 2`, the record's first dword, type, sub-word, class word, flag and the two flag-ish bytes; on
a bucket hit it compares every field and, when the record pointers differ, calls `0x4559470` to compare the records'
contents. A returned node is addref'd; the caller owns one reference.

All node types share this one class and vtable; the type word selects the payload interpretation (which handle, record
and flag the descriptor carried), not a different object class. The only separate command class in this slice is the
type-5 command object of section 4.2, which the type-5 node's handle points at.

Direct producers of the same node kind (whole `.text` E8 scan to `0x183B70`): three sites inside the decoder,
`0x4557D60` (two sites: type 0x0B through the `0x56C0BD0` descriptor builder, and it also appends a kind-8 record to
its own 0x38-byte pass array), `0x455AF70` (two sites: type 0x0B, or 0x0D when an element flag bit is set, through
`0x56C0BD0`), `0x45602B0` (two sites: the two-descriptor merge), `0x56C00F0` (two sites) and `0x56C09D0` (two
sites: type 0x0C table element with a constant-table block from `0x45594F0(9, 1, ...)`). `0x45629A0` is not in this
list because it reaches the factory `0x1851D0`, not the interner: type-5 nodes are produced through the
decoder/factory path and the merge, never by direct interning.

| emitter | RVA | type produced | source of the descriptor |
|---|---|---:|---|
| node decoder | `0x1836D0` | form output (1, 2, or declaration+0x04) | renderer node's declaration |
| table-set element emitter | `0x45629A0` | type-5 command through `0x1851D0`, node handle only | element+0x40 record |
| two-descriptor merge | `0x45602B0` | 0, 1, 3, 5-10 (merge table) | both sides' +0x08 records after 5->4 and 6->3 substitution |
| state/RT record builder | `0x4557D60` | 0x0B | node/context fields, class pointer `0x9E30340` |
| table-element builder (0x455AF70) | `0x455AF70` | 0x0B or 0x0D | element descriptor, class pointer `0x9E2FFF0`/`0x9E30340` |
| table-element builder (0x56C09D0) | `0x56C09D0` | 0x0C | constant-table block from `0x45594F0` |

The merge table is `0x6631F00` (4-byte groups, the returned type is byte +3 of the selected group): `0x4560810`
substitutes 5->4 and 6->3 first and then maps `(typeA, typeB, flags)`; the reachable outputs are 0, 1, 3, 5, 6, 7, 8,
9 and 10. The type vocabulary therefore extends past the decoder's six: 5/6 are the draw/draw merge outputs, 7-10 the
draw/state merge outputs, and 0x0B-0x0D only appear from the table-element builders.

### 2.2 The recorder and its pass object

`0x183190..0x183335` re-links a node under a new owner: it releases the old owner's reference, splices the node into
the target list at `owner+0x18` (`0x182870` releases one entry), and calls the node's `vtable[0x10]` (`0x183930`, the
decrement-and-destroy). The pass object that owns these lists is 0xa0 bytes with vtable `0x6632810`:

| slot | RVA | role |
|---|---|---|
| +0x00 | `0x1825D0` | destructor |
| +0x08 | `0x182600` | per-frame flush: walks the list at `[FUN_14503E0D0(obj+0x28)+0x90]`, filters on flag bits 0x20/0x300, releases entries and re-records the tail |
| +0x10 | `0x183A20` | walks the same list calling each entry's slot +0x28 |
| +0x18 | `0x182970` | list/state helper |
| +0x20 | `0x45182060` | dispatch helper |
| +0x28 | `0x185920` | pass setup |

`0x182D90..0x182F34` is the per-node emission helper that reaches the decoder: it asks the node object's slot
+0xD8 for the owner and slot +0x128 for the handle, builds the declaration through the owner's `[owner+0x90]->slot
+0x58`, then calls `0x1836D0(owner, declaration, command-slot, handle, flags)`. `0x182B60` wraps that into a recorder
and a 0xe0-byte object; `0x182CC0` builds a 0xd0-byte object with vtable `0x6632880`. `0x4589CA0` is the same shape
from the 0x458xxxx module (type-3 declaration, 0xd0-byte object with vtable `0x6634390`).

## 3. The submission ring (game+0x2481E90)

`0x2481E90..0x2482653` is the frame function that turns the renderer's recorded submissions into D3D12 calls. Its
input is a 0x60-entry ring of 0x48-byte records at `*(owner+8)+0x10`; the read index is owner+0x1B2C (wraps at 0x60),
the count owner+0x1B30 and the lock words at owner+0x1B38/0x1B10.

| record offset | type | content |
|---:|---|---|
| +0x00 | qword | owner object (render context) |
| +0x08 | u8 | record kind |
| +0x10 | qword | kind payload A |
| +0x18 | u32 | payload B |
| +0x1C | u32 | payload C |
| +0x20 | u32 | payload D |
| +0x28 | qword | payload E |
| +0x30 | qword | payload F (execute/end argument) |
| +0x38 | u32 | payload G (count) |
| +0x40 | qword | payload H (execute/end argument) |

| kind | measured (static) action |
|---:|---|
| 0 | batches three qwords per entry (+8, +0x18, +0x10) and calls `owner->vtable[0x118](owner, count, batch, image+0x75E0E88)` |
| 1 | releases the object at +0x10 through its vtable |
| 3 | binds the display target: `image+0x75E0E88 = *(qword*)(record+8)`; if null, creates it through a device slot 0x280 and registers it at `image+0x75DE4B8[(payload&0xf)*0x90]`; then `0x24826A0` |
| other (>=2) | pass/present record: builds a render-target/plane array from the 0x18-byte entries at record+8 (count at record+0x38), uses `image+0x75E0E88` as the first target, runs transitions 5 and 6 through `0xA1C290`, then calls `owner->vtable[0x100]` |

The `vtable[0x100]` call is at `0x24824C1` (return address `0x24824C7`) and is console queue slot 32, PresentX; the
earlier display slice already measured that call site
([display_path.md](display_path.md)). The present plane block is built on the stack at `0x2482302` (clear) and
`0x2482313` (fill) from the record's entries and the display target. The function ends with
`owner->vtable[0x80](owner, record+0x30, record+0x40)`.

## 4. The type-5 constant-table/LUT family

Type 5 is one of the renderer's node types (declaration form 5 is the one that emits it) and the only type whose node
carries a GPU constant table. The whole chain is measured statically end to end; the boot-measured half (the
zero-byte block, the 0x1000-byte copy and the 0x538D0E6 stop) is the prior slice's and is only cited here.

### 4.1 The chain

| step | RVA | measured (static) behaviour |
|---|---|---|
| decode | `0x1836D0` case 5 | factory call, command object stored in the caller's slot, job handle requested |
| factory | `0x1851D0..0x1853FE` | cache at owner+0x20; hash `0x185400`, compare `0x185450`; allocate 0x48 and construct on a miss |
| hash | `0x185400` | mixes kind (+0x10), every entry's dword (+0x20, 8 bytes each), count (+0x1C) and, when +0x30 is non-zero, the float +0x28 |
| compare | `0x185450` | count, kind, float (bitwise), selector==1 at +0x14, and every entry's key byte and dword |
| constructor | `0x1854D0..0x185567` | 0x48 bytes, vtable `0x66314B0`; owner +0x28, record +0x30, width +0x38, height +0x3C, float +0x40; kind 0 -> width 0x100, height 1; kinds 1/2 -> square size from `0x185FC0` |
| square size | `0x185FC0` | max per-channel key delta, sqrt, index into the 17-dword preset at `0x6631460` (16, 32, 32, 64 x4, 128 x6, 256 x4) |
| class getter | `0x23E550` | returns the literal 10 (0xa) |
| job create | `0x184770` | the command's `vtable[0xA8]`: validates record against the owner's class and returns a job |
| job record | `0x184F60` | record/owner class match, flag tests 0x4F0/0x100/0xC0 |
| job constructor | `0x185570..0x1856C5` | 0xa0 bytes, vtable `0x658DA80`: device +0x28, context +0x30, record +0x58, flag +0x44, count +0x45, +0x46 u16, item array +0x60 (0x30 bytes per item) |
| enqueue | `0x1856D0..0x1858D1` | adds the job to the pending list and waits on the record's completion word |
| runner | `0x186170..0x186456` | job `vtable[+8]`: per job item builds the item description through `0x501F4A0`, creates the per-item GPU objects through the `0x5204E60`/`0x15608E0` factory paths, then calls the executor |
| executor | `0x1335A20..0x13361ED` | takes the record from job+0x58, allocates the constant table through `0x45594F0`, converts and commits |
| table allocator | `0x45594F0..0x45596D1` | 0x68-byte table object, vtable `0x6631B10`, from `0x455A530`; per item `0x455A5E0` dims, `0x455A610` size, allocator slot +0x50 |
| stride | `0x1555C10` (leaf, no unwind entry) | format-word switch; **no case 0xa** -> 0 bytes per element |
| copy | `0x550E2D0..0x550E6F8` | converters `0x1336750`/`0x1336800`/`0x13368F0`, then `height * (width<<4)` bytes |
| ramp converter | `0x13368F0..0x1336E65` | 8-byte source entries to 256 float4 with interpolation and padding |

### 4.2 The record (what the factory hashes)

| offset | type | role | source |
|---:|---|---|---|
| +0x00 | dword | not read by the factory, the ctor or the copy | inferred |
| +0x10 | int | record kind: 0 = plain 256x1 ramp; 1 = radial square; 2 = radial square scaled by +0x28 | measured (static) |
| +0x14 | int | selector; `== 1` selects the 2.17f scale in the copy (else 1.0f), and its `== 1` result also enters the cache key | measured (static) |
| +0x1C | u16 | source entry count | measured (static) |
| +0x20 | pointer | source entry array, 8 bytes per entry: key byte at +0, four channel bytes at +4..+7 | measured (static) |
| +0x28 | float | parameter compared bitwise by the cache; scales the radial table (kind 2) | measured (static) |
| +0x30 | qword | row/stride-sized value used by the copy's row write (destination row pitch) | measured (static) |

The command object the factory returns is 0x48 bytes with vtable `0x66314B0`:

| slot | RVA | role |
|---|---|---|
| +0x00 | `0x183F60` | destructor |
| +0x08 | `0x7D50F0` | base-class release path |
| +0x10 | `0x183930` | reference decrement and destroy |
| +0x18 | `0x552F00` | record/owner accessor |
| +0x20 | `0x23E550` | class getter, literal 10 |
| +0x28 | `0x5439F0` | record kind accessor |
| +0x30 | `0x185D70` | returns a two-word handle that reads the class through `0x23E550` |
| +0x38 | `0x426320` | owner/context accessor |
| +0x40 | `0x550E2D0` | the copy (measured: writes width<<4 bytes per row, height rows) |
| +0x48 | `0x6151A0` | record hook |
| +0x50, +0x60 | `0x4810` | shared stub |
| +0x58, +0x68 | `0x20FDE40`, `0x20FDEC0` | command hooks |
| +0x70 | `0x15172C0` | record hook |
| +0x78 | `0x455A120` | constant-table module: stores 8 dwords into a 0x20-byte block allocated through the table's allocator, at table object +0x20 |
| +0x80 | `0x185E00` | copies 8 dwords of the command's record area at +0x20 into the caller |
| +0x98, +0xA0 | `0x22BA9B0`, `0x22BA9E0` | record hooks |
| +0xA8 | `0x184770` | create the job |

### 4.3 Producers of a type-5 record

A whole-`.text` direct-call scan to `0x1851D0` finds exactly three producers: the decoder's case 5, the table-set
emitter and the two-descriptor merge. There is no other E8/E9 call site; indirect calls would have to go through a
function pointer the decompile does not show, which is **inferred** to be absent.

| producer | call site | record source | when |
|---|---|---|---|
| decoder case 5 | `0x183767` | declaration+0x08 (the node graph's type-5 record) | a renderer node declares type 5 |
| table-set emitter | `0x45629B7` (inside `0x45629A0`) | element+0x40 | the draw's table-set builder walks an element whose type `(element+0x10 & ~1) == 4` |
| two-descriptor merge | `0x4560309` and `0x4560345` (inside `0x45602B0`) | each side's descriptor +0x08, after 5->4 / 6->3 substitution | a paired draw/state descriptor merge has type 5 on either side |

The table-set path, measured (static): `0x4562600` builds a table set from the draw's record enumeration and calls
`0x4562780`, which constructs a table-set object (vtable `0x6630EA0`, two slots created through `0x45628A0(8, ...)`
and `0x45628A0(9, ...)`); the base constructor `0x4561C30` reads the draw's pipeline-state record, takes its class
nibble at +0xF, indexes the 5-byte-stride byte table at `0x6631D34` (`(nibble&0xf)*5`), multiplies that byte by 0x10
and copies 8 dwords at record+0x10+offset into the table set. `0x45628F0` walks the element list and calls the
emitter for type-4/5 elements. `0x4562320` in the same module is a projection/min-max helper, not a producer.

### 4.4 What the 256-entry tables are

| record kind | table generated | size | measured (static) generation |
|---:|---|---:|---|
| 0 | 256 x 1 RGBA32F | 0x1000 bytes | `0x13368F0` turns the 8-byte keyed entries into 256 float4: replicate the first key's colour before the first key, interpolate between keys, truncate/pad to exactly 0x100 entries |
| 1 | N x N radial | N^2 * 16 | the copy (`0x550E2D0`) indexes the 256-entry ramp by a radius: `sqrt(dx*dx + dy*dy)` scaled by the record float, rounds and clamps to 0xFF |
| 2 | N x N radial, scaled | N^2 * 16 | as kind 1, with `+0x28` applied to one axis before the same radius computation |

N comes from `0x185FC0`: the largest per-channel key delta is scaled and square-rooted into the 17-entry preset list
(16, 32, 32, 64 x4, 128 x6, 256 x4). The 256-entry kind-0 ramp is therefore a keyed four-channel colour/curve ramp,
and kinds 1/2 are radial (gradient) samplings of the same ramp.

Whether the consuming shader uses it as a colour transfer curve or as a spatial filter kernel is **inferred**, not
measured: the data shape (4 channels, key interpolation, radial variants) and the binding class (a type-4 table
element in a draw's table set) are measured, but no producer of the record's key bytes and no shader-side read of the
bound table were decompiled here. The measured boot ordering placed the upload between the submission's content draws
A/B and the compose/upscale draw C, which makes C's table set the candidate consumer (prior slice; cited, not
re-derived here).

### 4.5 The block sizing, including the class-0xa zero stride

The constant-table object allocates one block per item and sizes it with the format stride switch:

| function | measured (static) behaviour |
|---|---|
| `0x45594F0` | allocates a 0x68-byte table object (vtable `0x6631B10`) whose +0x28 data subobject `0x455A530(class, count, flags)` initialises: item count = count * `0x6FEDB0(class)`, item array 32 bytes per item |
| `0x455A530` | allocates `item_count*0x20` bytes through the allocator slot +0x50 and zeroes it; small counts use the inline array |
| `0x455A5E0` | per-item dimensions from the record (halves them for class word 200) |
| `0x455A610` | sums `0x1555B60(class, w, h)` over the item's elements |
| `0x1555B60` | block-compressed formats use a block extent, everything else `stride(class, w) * h` |
| `0x1555C10` | stride switch on format word `& 0xfff`; cases for 1/2, 3/4, 9, 0x15, 0x32/0x3f, 0x33-0x35/0x40/0x41, 0x36-0x42, 0x38/0x39, 100/101, 200/201; **class 0xa is not in the switch and returns 0** |

For the type-5 class 0xa record kind 0 the ctor fixes 256 x 1, the stride switch returns 0, the allocator is asked
for 0 bytes, and the copy then writes `1 * (256<<4) = 0x1000` bytes into it: the measured overrun of the prior slice,
now with its static cause named.

## 5. The own-upscaler kind branch

The one branch that reads a console-profile answer before the upscale pass is the upscaler factory and its selector.
All rows below are measured (static) from the decompiled functions and the listed `.rdata` tables. The prior slices cite
these objects by VA (`0x147B99040`, `0x147B98E08`, the registry read at `0x1455DEB88`); the RVAs here are those VAs minus
the image base (0x140000000), so they name the same objects.

| piece | RVA | measured (static) behaviour |
|---|---|---|
| factory | `0x7E93B0..0x7E93E1` | kind 6 -> lazy init `0x7E9490`, returns object `0x7B99040`; kind 7 -> lazy init `0x56D9BC0`, returns object `0x7B98E08`; any other kind -> null |
| kind-6 field init | `0x56D9D80` | object `0x7B99040`: vtable `0x6300BF0`, kind field +8 = 6, resolution defaults at +0x90 (1.0f), +0x98 (0x100), +0xA0 constant, plus the policy fields |
| kind-6 lazy init | `0x7E9490` | calls `0x56D9D80` then `0x55DCC0C(&0x28F1B90)`, the static initialiser that stores the vtable |
| kind-7 field init | `0x56D9C20` | object `0x7B98E08`: vtable `0x6300B50`, kind field +8 = 7, its own resolution/policy defaults |
| kind-7 lazy init | `0x56D9BC0` | one-shot init and vtable publication for the kind-7 object |
| selector | `0x7E93F0..0x7E943B` | if the cached capability word at `0x7B9846C` and the byte at `0x7B98468` are both zero, builds the word with `0x56DCF00`; then `0x75140` returns the console type; returns **7 iff console type == 6 and capability bit 0x40 is set**, otherwise 6 |
| capability word | `0x56DCF00` -> `0x876110` | walks factory kinds 0..7; each existing object's slot +8 ORs a presence bit into the word: kind 7 (`0x5EBE2D0`) sets 0x40, kind 6 (`0x5EBE2E0`) sets 0x20 |
| console type | `0x75140` -> `0x55DEB88..0x55DEC00` | direct host registry read (`RegOpenKeyW`/`RegQueryValueExW`), result cached at `0x7B63B0`; value 0x60 -> 6, 0x64 -> 7, 0x68 -> 8, 0x58 -> 5, 0x50 -> 4, 0x40 -> 3, 0x10/0x20/0x21/0x30/0x31 -> 2, a present but unknown value -> 0, and an absent key or unreadable value -> 1 |

The capability word is a **presence** bitmap of the two constructed objects, not a hardware capability: both objects set
their bit unconditionally when they exist. On a PC the console-type key is absent, the read returns 1, and the selector
returns kind 6 (the title's own upscaler) whatever the capability word says. The two object vtables are:

| vtable | slots (RVA) |
|---|---|
| kind 6 `0x6300BF0` | `0x28F1BB0`, `0x5EBE2E0`, `0x13F3F60`, `0x28F1BF0`, `0x28EF530`, `0x28F1DE0`, `0x4380`, `0x28F1DF0` |
| kind 7 `0x6300B50` | `0x28EF1A0`, `0x5EBE2D0`, `0x28EF1D0`, `0x28EF470`, `0x28EF530`, `0x28EF550`, `0x4380`, `0x4380` |

What the branch does **not** do is measured: none of the kind-6 object's methods calls the type-5 factory `0x1851D0`,
the table-set emitter `0x45629A0`, the copy `0x550E2D0` or the ramp converter `0x13368F0`; the object is a
resolution/capability policy object. The type-5 table upload is created by the draw's table set, so the kind branch
can only select the LUT indirectly, through whichever draw is recorded on the kind-6 path.

## 6. The compose/upscale passes and where the presented plane is written

The presented plane is one title resource. Which pass writes it is measured by the earlier boot slices and readable
statically in the submission stream:

| frame | passes into the display target | measured evidence |
|---|---|---|
| normal (loading/stationary) frame | constant pass A -> sibling 4K target; compose pass C samples it and writes the presenter's plane-0 target | [upscale_standin.md](upscale_standin.md) section 2.1 |
| first stage-3 content frames | the title clears the display target to black (command 3), then draw A (constant) and draw B (content) write the intermediate target, then draw C (compose, six image samples) writes the display target over the clear | [vertex_buffer_va.md](vertex_buffer_va.md) section 17.3 |
| present | the frame function replays the submission and calls PresentX with the plane block built from the display target | this page section 3; [display_path.md](display_path.md) |

The static side of that mapping is the display-target bind: record kind 3 stores its resource into
`image+0x75E0E88`, and the pass/present record (kind >= 2) uses that word as its first render target before calling
PresentX. A pass writes the presented plane exactly when its record runs with that target bound; in the measured
stage-3 submission that is the compose draw C.

The compose pass's own state comes from the draw record through the table set: the pipeline-state record's class
nibble at +0xF selects a descriptor block through `0x6631D34`, and the table set is built from it (section 4.3). The
draw's target, viewport/scissor and sampled table are the earlier slices' measurements; this page adds only the
static binding path.

## 7. The present/presenter path and colour conversion

| step | RVA | measured (static) behaviour |
|---|---|---|
| per-frame entry | `0xC9E5E0..0xC9E86C` | runs `0x24846D0`, then a barrier with state 4, then flushes the display list at `0x75D5AF0` through `0x55DF70`/`0x4FC99C0` |
| present-to-display | `0x24846D0..0x2484A6A` | `PeekMessageA`/`TranslateMessage`/`DispatchMessageA` pump; barrier with state 8; two display surfaces from `0x50BB280(0x75E0E98)`; decides the plane colour space (0 or 0xC) from `0x4FD22C0`; calls `0xCD72B0` |
| present wrapper | `0xCD72B0..0xCD731C` | takes a plane token from `0x75E0EC8`, calls the state writer, advances the token, returns the token with flags<<56 |
| present-state writer | `0xCD7320..0xCD7569` | fills the static present state (below); ends in a stats record `0x1F6330` into a 0x60-entry ring at `0x75DF2A8` (0x48 bytes per record) |
| console PresentX | `0x24824C1` | queue slot 32 (`owner->vtable[0x100]`) with the stack plane block built at `0x2482302`/`0x2482313` |

The static present state the writer fills, measured (static):

| image address | content |
|---|---|
| `0x75E0DD8` dword | 2, a present flag |
| `0x75E0E10` | plane count (1 or 2) |
| `0x75E0E14`, `0x75E0E16`, `0x75E0E17`, `0x75E0E18`, `0x75E0E1C`, `0x75E0E20` | display/refresh-mode fields taken from the display-mode entry at `0x75DE4B8[(mode&0xf)*0x90]` |
| `0x75E0DE0` + i*0x18 | per-plane source: +0 resource = surface+0x20; +8 colour space; +0xC u16 source width = surface+0x38; +0xE u16 source height = surface+0x3A |
| `0x75E0E78` | the frame-pipeline token counter |

The per-plane resource store is the instruction at `0xCD74DB`, the same writer the display slice's page-write watch
named for the plane cell ([display_path.md](display_path.md)); the earlier watch measured that cell holding 0, which
is the null-resource refusal the surface-population fix addresses
([surface_population.md](surface_population.md)).

Colour and format conversion, measured and inferred:

| where | what happens | source |
|---|---|---|
| title display resource | created through `0x193DE10`: description built by `0xC4B030`, create through the device's `vtable[0x428]`, the resource stored at surface+0x20 by the surface wrapper `0x325090`; extent at surface+0x38/+0x3A; the describe record carries the format byte | measured (static) |
| title plane colour space | `0x24846D0` selects 0xC when `0x4FD22C0` answers HDR-on and 0 otherwise, per plane; the value is stored into the plane's +8 word | measured (static) |
| title output format | the compose pass writes the display target in the title's own format (the measured content target is format 37, 3840x2160); no colour-space conversion call was found in this chain | measured (boot, cited) |
| presenter conversion | the conversion is the layer presenter's: the plane colour space drives its compose/blit (HDR10 compose vs SDR blit), and `port/src`'s presenter applies the SDR EOTF and format conversion | inferred from the layer contract; the title side only supplies the field |

The two colour-space values 0 and 0xC are the title's own field values; naming them RGB_FULL_G22_NONE_P709 (0) and
RGB_FULL_G2084_NONE_P2020 (12) is **inferred** from the DXGI colour-space enumeration and is not read from the image.

## 8. Ranked conversion hypotheses with our files and falsifiers

The hypotheses below are about conversion gaps that can stand between the recorded renderer commands and a presented
frame. Rank is by measured support, not by expected severity.

| rank | hypothesis | evidence and our files | falsifier | existing probe? |
|---:|---|---|---|---|
| 1 | **root arguments**: the PC root-argument packing puts the descriptor table at s0/s1 where the console driver block belongs, and the console table at s8 is not forwarded | [vertex_buffer_va.md](vertex_buffer_va.md) section 18: with `PALMETTO_GRAPHICS_CONSOLE_ROOT=1` a content draw's control writes 4,147,200/8,294,400 pixels; [root_arguments.md](root_arguments.md); `port/src/vulkan_replay.h` | the control draw writes 32 160 240 over the geometry's rectangle; the compose draw's matrix A still reads empty (open) | yes: `PALMETTO_GRAPHICS_CONSOLE_ROOT`, `PALMETTO_FRAME_DUMP_CONTROL_PS`, `PALMETTO_READBACK_FULL`, `PALMETTO_CONTENT_PROBE`, `PALMETTO_CONTENT_PROBE_ARM` |
| 2 | **vertex feed**: the driver-block V# has stride 0 / wrong record count / non-heap-relative base, so geometry collapses | [vertex_buffer_va.md](vertex_buffer_va.md) section 18.2; [vertex_path.md](vertex_path.md); [vertex_constant_feed.md](vertex_constant_feed.md) | the vertex-path dump decodes the emitted V# to the bound view's stride and record count and the control covers pixels | yes, partly: `PALMETTO_VERTEX_PATH`, `PALMETTO_GRAPHICS_CONSOLE_ROOT`, `PALMETTO_CONTENT_PROBE_ARM` (pins the armed ordinal, e.g. the compose draw); `PALMETTO_VERTEX_STREAM_DESCRIPTOR` alone is a measured-negative arm (corrupts the title's state before stage 3) |
| 3 | **formats/stride**: the type-5 class 0xa table's block is sized by a stride switch with no 0xa case, so the block is 0 bytes and the copy overruns | this page section 4.5 and the prior slice's measured zero-byte request and 0x1000 copy ([vertex_buffer_va.md](vertex_buffer_va.md) sections 15-16) | the executor's block request is 0 and the copy writes 0x1000; the stop moves if the stride switch treats 0xa as a known format | yes: `PALMETTO_EXECUTOR_WATCH`, `PALMETTO_POKE_LOG` at `0x45629A0`/`0x550E2D0`, `PALMETTO_XS_ALLOCATOR_CALLBACK_TRACE` |
| 4 | **colour-space**: the colour data is zero or the SDR/HDR choice hides the frame | [colour_path.md](colour_path.md): the constant pass's four colour dwords are zero and the module reads nothing else; this page section 7: plane colour space 0 vs 0xC | the four colour dwords become non-zero in a content frame, or a captured presented frame is non-black with HDR off | yes: `PALMETTO_COLOUR_PATH`, `PALMETTO_READBACK_AT_DRAW`, `PALMETTO_READBACK_FULL`, `PALMETTO_CAPTURE`, `PALMETTO_HDR`, `PALMETTO_SDR_EOTF` |
| 5 | **present/plane resource**: the present plane's resource word is null, so PresentX refuses the plane | [display_path.md](display_path.md): the `0x75E0DE0` cell was written 23,819 times, all 0, by `0xCD74DB`; [surface_population.md](surface_population.md) | the plane cell holds a non-null placed resource and PresentX succeeds on it | yes: `PALMETTO_WATCH_RVA_WRITE=75E0DE0`, `PALMETTO_PRESENT_STUB`, `PALMETTO_XS_CALLERS`, presenter captures |
| 6 | **upscaler kind**: the console FSR branch (kind 7) is not taken on PC | this page section 5: selector returns 6 unless console type 6 and caps bit 0x40; [upscale_standin.md](upscale_standin.md) section 2.2: 0 of 29 FidelityFX programs requested | make the title's console-type read answer 6 and see kind 7, FSR programs requested and this table-set draw change | yes, measured: `PALMETTO_CONSOLE_TYPE` (section 11) answers the read as Series S; the selector returns 7 and the console FSR path runs, but the stop moves earlier and the class-0xa command is not created in the measured window |
| 7 | **type-5 table consumer**: the 256-entry table is a compose-pass filter kernel (own-upscaler kernel) rather than a colour curve | this page section 4.4: four channels, key interpolation, radial variants; the boot ordering put the upload before the compose draw | the element's record kind and the compose draw's table set read back together; a kind-7 arm that still records the draw's table set removes the indirect selection | no existing single probe; `PALMETTO_RAMP_PROBE` would be a new bounded arm (not implemented) |

Hypotheses 1-5 can be exercised with switches the tree already has. Hypothesis 6 was taken up after this page: the
console-type arm exists as `PALMETTO_CONSOLE_TYPE` and is measured in section 11 (the answer is consumed and the
kind-7 FSR path runs, but it does not remove the class-0xa command - it stops the boot earlier). Hypothesis 7 needs
either a new bounded emitter probe or a kind-7 arm that survives to the table-set draw; neither exists.

## 9. Limits

- **Static only.** Every code row is decompiled from the user's image with this project's Ghidra tooling; no boot was
  run for this page, and the boot numbers quoted are other pages' measurements. The semantic names for the colour
  spaces, the record's second descriptor (type 5), the dropped table columns and the upscaler policy fields are
  **inferred** from structure, not read from symbol data.
- **The image has no symbols.** Function boundaries are `.pdata` boundaries; `0x1555C10` is a leaf with no unwind
  entry. The class/format table at `0x658DC80` is walked by a deduplicating constructor (`0x1B373F0` -> `0x1B37440`)
  and the class-0xa record is the one whose first dword is 0xa (table offset +0xA8, first qword `0x00001002_0000000a`);
  its ordinal among the constructed format objects is not asserted here.
- **The submission record's plane-field mapping is partly inferred.** The call at `0x24824C1` is PresentX (queue slot
  32, measured call site); the exact per-field assignment inside the stack plane block is read from the store order
  and is not byte-for-byte matched against the console struct in this page.
- **The 256-entry tables' semantic consumer is narrowed, not proven.** Their data shape, generation, binding class and
  upload order are measured; whether the compose shader uses them as a colour curve or a filter kernel is the open
  item, and no shader-side read is decompiled here.
- **No game data.** RVAs, offsets, type words, counts, formats and call sites only; no strings, asset bytes, package
  paths or hashes appear in this page.

## 10. Files

One file for this slice, new and untracked: `docs/decomp_renderer.md`. The work notes, decompiled function dumps and
Ghidra logs are outside the repository (`D:\leonida-work\decomp\...`). No other page, no `port/` file, no test and no
build file was edited, and no git writes were made.

## 11. The console-type experiment: answering the read as Series S, measured

**Status: measured, with a negative verdict on the picture question.** One opt-in switch, two boot sets and the static
cross-check below. The switch is off by default and the default path is unchanged (no environment variable means no
IAT patch at all); no game string, asset byte or hash is carried in the tree or in any log. The answer is consumed and
the selector takes the console FSR branch - but the console FSR path stops the boot earlier than the class-0xa window,
so it does not avoid the crash that matters.

### 11.1 The override

The switch is `PALMETTO_CONSOLE_TYPE`. The only accepted value is `6` (Series S; `seriess`, `series-s` and `series s`
are also accepted); any other value prints one line and leaves the default path. With it set, `port/src/pe_loader.cpp`:

- arms on the mapped image that holds the console-type read's two wide strings at the RVAs section 5 names: the
  HKLM subkey at `image+0x678F340` and the value name at `image+0x678EB18`. The strings are read from the title's own
  `.rdata`; a plausibility check (printable, terminated, the key containing a backslash) keeps a different build from
  arming. No title string is written into this repository;
- replaces the image's three `ADVAPI32.dll` IAT slots (`RegOpenKeyW`, `RegQueryValueExW`, `RegCloseKey`) with hooks that
  answer exactly that read: an open of HKLM with the key string gets a token handle, a query of the value name on that
  token gets `REG_DWORD 0x60` (the value section 5 maps to console type 6), and a close of the token succeeds. Every
  other registry call - and every other key or value name on the same imports - is forwarded to the host advapi32 entry
  point the loader had bound;
- reports each answered read with its caller (`RegOpenKeyW at game+0x55DEBB2`, `RegQueryValueExW at game+0x55DEBF2`, in
  the measured binary) and emits a `console_type_override` trace event, so a boot says both that the title asked and
  that the answer was served. The host registry is never written.

The default path is untouched: the loader only reads the switch inside `bind_imports`, and when it is unset the IAT is
not patched (a `--dry-run` of the same image with the switch unset prints no override line and binds every import as
before). The switch is refused safely: `7` prints one line and stays off.

### 11.2 The presence bit is not a second condition

Section 5's selector tests the console type and capability bit `0x40`. The static reading already answers the second
half: the capability word is built by the function that walks the upscaler factory for kinds 0..7 (`0x56DCF00` ->
`0x876110`); the factory constructs both the kind-6 object (`0x7E9490`, `0x7B99040`) and the kind-7 object (`0x56D9BC0`,
`0x7B98E08`) on that walk, and each object's vtable slot +8 ORs its presence bit (`0x20` / `0x40`) into the word. The
`0x40` bit is therefore set by the build itself on every boot, independent of the console type, and answering 6 is
sufficient: no additional write to the word is needed. The measured selector return in section 11.4 confirms it.

### 11.3 The two boot sets

Both sets are the section-12 content environment with the class-0xa slice's probe set (`PALMETTO_ALLOC_WATCH` plus the
`PALMETTO_DUMP_REGS`/`PALMETTO_FAULT_MEM` diagnostics, presenter captures and readbacks), one boot under the boot lock,
180 s timeout, the same binary (`palmetto.exe` SHA-256 `D7B4D8975D449BA5...` for every row). The control set is the
current default answers; the Series-S set adds `PALMETTO_CONSOLE_TYPE=6`. The extra fast-path probes that section 11.6
names are not in these rows.

| set | boot | stage 3 | first stop | stop t | device lost | class-0xa signals | FSR path | captures |
|---|---|---|---:|---:|---|---|---|---|
| control | `def1` | no | `game+0x86F981` read `0x954F` | 63.1 s | yes | none | none | 4, black |
| control | `def2` | yes | `host:VCRUNTIME140.dll+0x1CCAB` read `0xC837E003E8` | 67.3 s | yes | none | none | 10, black |
| control | `def3` | yes | `game+0x13360A3` read -1 | 72.8 s | no | FORMAT KEY key 0xa / BLOCK ALLOC 0 bytes 256x1 / ENTRY WRITTEN stride 0 size 0 | none | 10, black |
| Series S | `s61` | yes | `game+0x4EEE4EC` read `0x100000028` | 53.2 s | no | none | DLL mapped, 1920x1080 format 67 refused, 1280x720 formats 57/62 placed | 10, black |
| Series S | `s62` | yes | `0x0` execute | 52.7 s | no | none | DLL mapped | 10, black |

Stage-3 times in the probe's own clock: control `def3` 68.5 s, `def2` 61.0 s; Series S `s61` 51.8 s, `s62` 51.6 s. The
class-0xa sequence in the control boot is the one sections 4.5 and 7 describe: the emitter's FORMAT KEY at 71.80 s
(key 0xa, command vtable `game+0x66314B0`, creator vtable `game+0x658DB60`, caller `game+0x1837A2`), the executor's
BLOCK ALLOC of `0x0` bytes for a 256x1 entry at 71.84 s with `overlap YES`, the ENTRY WRITTEN with stride 0 and size 0
at 71.87 s, then the stop at `game+0x13360A3` 0.89 s later.

### 11.4 What the answer changed

- **It was consumed.** Every Series-S boot logs the title's own two calls answered at t = 0.888 s after the run's start:
  `RegOpenKeyW at game+0x55DEBB2` and `RegQueryValueExW at game+0x55DEBF2`, each answered `0x60`, twice per boot (four
  trace events). The read is the static initialiser's, so the whole boot runs on console type 6.
- **The selector takes kind 7.** A one-shot `PALMETTO_POKE_LOG` at the selector's return (`image+0x7E9429`) prints RAX
  at the exit of `0x7E93F0`. Control (60 s poke boot): 20 hits, **RAX=6 on all 20**. Series S: 17 hits, **RAX=7 on all
  17**. This is the direct measurement of the branch, and it agrees with section 11.2's reading of the capability bit.
- **The console FSR path runs.** The selector result commits the upscale dispatch to the kind-7 object. In both Series-S
  boots the title then creates the Series-S upscale shape - a 1920x1080 target in format 67 (the layer's
  `GetResourceAllocationInfo`/slot-25 path refuses the format, so `CreateCommittedResource` returns `0x80070057`), and
  1280x720 color/depth targets in formats 57 and 62 (placed, accepted) - and maps
  `amd_fidelityfx_upscaler_xbox.dll` from the game directory (a run-time load). The control boots do neither: no
  FidelityFX module is mapped anywhere in their measured window. Of the 29 FidelityFX `FSPX` programs, 0 are requested
  in either set at the measure time, so the failure is before their pipeline creation, not a translation refusal.
- **The class-0xa command is not created in the Series-S window - and the honest reason is where the boot stops.**
  In both Series-S boots the alloc-watch probe (armed at stage 3) never sees a FORMAT KEY, BLOCK ALLOC, ENTRY WRITTEN or
  an executor-entry hit before the stop, which arrives 1.1-1.4 s after the probe reads stage 3. In the control's
  stage-3 boot those events are 3.3 s after the probe's own stage-3 read, so a control boot that stops before that
  point (as `def2` did) also shows no class-0xa event. The measured conclusion is therefore not "the branch skips the
  command": it is that the console FSR dispatch fails first, in a different family, before the content submission that
  would create the command.
- **The stop family moved.** The two Series-S stops are `game+0x4EEE4EC`, a table lookup that loads `[rax+0x28]` with
  `rax = 0x100000000` - a read through the fabricated console GPU address range, the mechanism
  [boot_frontier.md](boot_frontier.md) measures - and `0x0` execute, a call through a null function pointer. Neither
  occurs in any control boot; both are new for this arm.
- **No pixels changed.** Every presenter capture in both sets is mean RGB 0.0, black; the Series-S set has 10 captures
  (12 with the poke boot) and none is non-black. The trade the peers' pages describe is confirmed: the console FSR path
  cannot run on this layer, and taking it leaves the picture where it was.

### 11.5 The precondition: stage-3 reach with the current defaults

The experiment needs machine stage 3. After the platform slice changed the first-run service defaults
([decomp_first_ui.md](decomp_first_ui.md) section 12), a first probe attempt produced 0/6 stage-3 boots with the
experiment's own fast-path probes on. The matched comparison below separates the tree from the probes: same binary,
section-12 content environment, seed and probe set (`PALMETTO_ALLOC_WATCH` plus the diagnostics; no selector poke, no
watch thread), 180 s arms, retried until two stage-3 boots.

| arm | boots | stage 3 | first-stop families |
|---|---:|---:|---|
| current defaults | 3 | 2 | `0x86F981` (no stage 3), `host:VCRUNTIME140.dll+0x1CCAB` (after stage 3), `0x13360A3` (class-0xa family) |
| `PALMETTO_SILENT_SPEECH=0`, `PALMETTO_USER_CHANGE_EVENT=0`, `PALMETTO_PAD_PRESENT=0` | 5 | 1 | `0x4D9F636`, `0x86F97E`, `0x559468F`, `0x86F981` (all no stage 3) |
| earlier arm, only `PALMETTO_SILENT_SPEECH=0` reached the process | 5 | 0 | `0x86F97E` x3, `0x86F981`, `0x4D4CA97659` |

Read together: the current defaults do not reduce stage-3 reach; this sample reaches it more often than the pre-default
answers (2/3 against 1/5 and 0/5). The 0/6 that started this measurement came from the experiment's own hot-path probes
(the selector poke and the 2 ms watch thread), not from the platform defaults; the Series-S boots themselves reach
stage 3 in 2 of 2 attempts. No change to the platform defaults is implied by this measurement; they are the better
trajectory of the two measured here.

### 11.6 Limits

- **The image version.** The two RVAs are image-relative and move with a different build. The arming plausibility check
  fails closed (no patch, one line), but the strings themselves are not verified against the decompilation beyond that
  check.
- **The two-read shape is measured, not explained.** The title calls the answered read pair twice per boot. The
  decompilation shows one caller in the static runtime block; the second call is not attributed here. Both reads are
  answered identically.
- **The class-0xa skip is not proven.** Section 11.4 gives the measured reason: the Series-S boot stops before the
  content submission whose executor pass creates the command. Whether the kind-7 upscale dispatch would still record the
  compose draw's table set - and therefore the class-0xa command - if the console FSR path survived is not measured.
  That is the next falsifier: make the FSR create/dispatch succeed far enough to reach the same executor pass (for
  example by backing the console address range, `PALMETTO_BACK_IMAGES`, or by standing in the FSR dispatch's resource
  creations) and watch the alloc-watch sites with `PALMETTO_CONSOLE_TYPE=6`.
- **The FSR path's own failure is named, not fixed.** The two stop sites are new occurrences of existing families
  (section 11.4); neither was decoded past the fault. The FSR program request count (0 of 29) is taken from the shader
  translation log, so it does not rule out a request path that the boot never reaches.
- **The A/B is one machine and one tree state.** Both sets share one binary and one boot slot; the boot slot serialises
  boots but not CPU load from builds, and stage-3 reach is a race.
- **No game data.** RVAs, registry value mappings, the console type, module names, formats, dimensions, counts and
  outcomes only; the two registry strings are read from the user's own image at run time and never written to the tree.

### 11.7 Tests and gates

- `port/test/pe_loader_test.cpp` gains 27 checks (106 total): the switch's spelling (`nullptr`, empty, `0`, `6`,
  `SeriesS`, `series-s`, surrounding spaces, a refused `7`, a refused word), the environment read and its unset/refused
  fallback, the exact registry-name match (pointer, content, prefix, case, null on either side) and the answers' armed
  and unarmed shapes. They run in the existing `pe_loader` CTest target, no new target or source file.
- The default path is covered by the switch-off checks and by the `--dry-run` comparison above.
- Gates on the changed tree: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints
  `TESTS_BUILT`, the `pe_loader` and the other `pe_loader.cpp`-linking tests pass, and the twelve `tests/test_docs_*.py`
  checks pass. No test or build file other than the two named source/test files was edited.

### 11.8 Attribution

This section was implemented from this project's own measurements: the two boot sets above (under
`tooling/boot_report.py`, boot lock held), the static reading of the selector/factory/capability functions already
recorded in sections 5-6 and in the classwhy work notes, and the layer behaviour the runtime itself prints. The peer
projects' published observations of the Series-S path are cross-checked where section 5 cites them and stay credited in
[NOTICE.md](../NOTICE.md); no code or data was copied from any of them and no NOTICE row changes for this section. No
git writes were made; the uncommitted files at the end of the slice are `port/src/pe_loader.h`,
`port/src/pe_loader.cpp`, `port/test/pe_loader_test.cpp` and this page.

