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


## 12. The Series-S upscale shape survives its creation and the FSR path still stops before the stage-3 executor pass

**Status: measured, implemented behind the console-type switch, with a negative on the class-0xa question.** Section 11
left the console FSR path refused before its first FidelityFX program: the kind-7 branch creates its 1920x1080
`R9G9B9E5_SHAREDEXP` render target through the console placed path, the layer's allocation query refuses the format's
render-target flag pair, and the creation answers `0x80070057` (`E_INVALIDARG`). This section decodes that description
and the exact refusal, implements the measured support for it on the Series-S arm, and re-measures the three questions
of the picture goal: whether the FSR path survives the creation, whether the class-0xa command appears, and whether
stage 3 continues. It survives the creation; the boot then stops before stage 3 in both measured arms, so the executor
pass and its class-0xa command are still not reached and **0 of 29** FidelityFX programs are requested. The default
path is unchanged: with `PALMETTO_CONSOLE_TYPE` unset the same description keeps its previous named refusal.

### 12.1 The measured shape and the exact refusal

With `PALMETTO_CONSOLE_TYPE=6` the kind-7 branch creates its upscale output on the title's own worker thread, at a
title VA it has mapped, through console device slot 133 (`CreatePlacedResourceX`). One boot's log for that sequence
(`fsr1`; the shape lines are the same in the committed Series-S arms of section 11, before this section's change):

```text
[rt] d3d12: GetResourceAllocationInfo format not supported yet: 67
[rt] d3d12_xs: slot 25 refused description 0/1: dimension 3 format 67 flags 0x300005 layout 0x0 1920x1080x1 mips 1 samples 1
[rt] d3d12_xs: slot 108 could not size the description, reporting 64 KiB / 64 KiB alignment
[rt] d3d12: placed texture: address <title VA> 1920x1080x1 format 67 dimension 3 mips 1 layers 1 flags 0x5 samples 1 layout 0x0
[rt] d3d12: CreateCommittedResource refused (texture size, mips, samples or alignment), dimension 3
[rt] d3d12: placed texture refused: 1920x1080x1 format 67 ... hresult 0x80070057
```

| item | measured value | reading |
|---|---|---|
| dimension | 3 | `D3D12_RESOURCE_DIMENSION_TEXTURE2D` (not a 3D texture) |
| extent / mips / array | 1920x1080, 1 mip, 1 layer, 1 sample | the console's Series-S upscale output size (1080p) |
| `desc.Format` | 67 | the PC DXGI numbering: `DXGI_FORMAT_R9G9B9E5_SHAREDEXP`, 4 bytes per texel. The title's resource descriptions use the PC enum; the console-only values sit in the PC gap 116..120 ([3d_textures.md](3d_textures.md) section 2.2), so 67 is not a console-only number |
| console flags | `0x300005`, masked to `0x5` | `ALLOW_RENDER_TARGET \| ALLOW_UNORDERED_ACCESS`; the `0x300000` bits are Xbox-only and the proxy's `0x5ff` mask drops them (device slot 133's handler) |
| layer table entry | `R9G9B9E5_SHAREDEXP` -> `VK_FORMAT_E5B9G9R9_UFLOAT_PACK32` | the format is mapped; the refusal is not a missing format |
| refusal site | allocation query -> placed creation | the query answers the invalid allocation (`UINT64_MAX`), the console slot-108 size query falls back to 64 KiB, and the created description is then refused with `0x80070057` |

The refusal reason is the flag pair, not the format: this host GPU (RTX 4090) has no `COLOR_ATTACHMENT` and no
`STORAGE_IMAGE` support for `VK_FORMAT_E5B9G9R9_UFLOAT_PACK32` (the same boot logs the UAV-only committed
`R9G9B9E5` textures taking the mutable `R32_UINT` emulation: `R9G9B9E5_SHAREDEXP UAV unavailable natively, using
mutable R32_UINT; format 67`). The layer's existing emulation refuses `ALLOW_RENDER_TARGET` by design
([d3d12_vulkan.md](d3d12_vulkan.md) section 3.7), so the allocation query has no format for the RT+UAV pair and returns
the invalid answer. The title's console branch asks for that pair (the same format's UAV creation is measured in the
FidelityFX route test, [d3d12_vulkan.md](d3d12_vulkan.md) Appendix A and [fsr_route.md](fsr_route.md) section 4.2); this section measures the host's refusal, not
the console's answer to the render-target half. The layer is the side that cannot attach the format.

### 12.2 What we implemented

| file | change |
|---|---|
| `port/src/console_texture.h` | `console_r9g9b9e5_render_target(desc)`: the measured shape (2D, `R9G9B9E5_SHAREDEXP`, 1 mip / 1 layer / 1 sample, `ALLOW_RENDER_TARGET \| ALLOW_UNORDERED_ACCESS`). `console_type_is_series_s(text)`: the Series-S spellings of `PALMETTO_CONSOLE_TYPE` the loader accepts (`6`, `seriess`, `series-s`, `series s`, any case, surrounding spaces). Both pure, so the GPU-free test covers them |
| `port/shims/d3d12_vulkan_device.cpp` | `texture_format`'s `R9G9B9E5_SHAREDEXP` block: when the host has no native attachment/storage for the format and the description is the measured console shape, and `PALMETTO_CONSOLE_TYPE` answers Series S, and the existing mutable `R32_UINT` UAV backing is available, the resource is created as that backing (the RT flag's `COLOR_ATTACHMENT` usage included). `CreateRenderTargetView` refuses the attachment view of a `uint_backed` `R9G9B9E5` resource by name: the host cannot attach `E5B9G9R9`, and an `R32_UINT` attachment would not be the format the title's pipeline names. Off the console arm the description keeps the previous refusal, and a console shape without the backing keeps a named refusal too |
| `port/shims/d3d12_xs_device.cpp` | `PALMETTO_SLOT67_PROGRAM_LOG=1`: one bounded line per slot-67 call with the kind-6 program's size and SHA-256 (and the kind-0 state's), so the FidelityFX program request count is measured rather than inferred. Off by default; the call's answer is unchanged |
| `port/test/xs_proxy_mock_test.cpp` | the shape predicate and the console-type spelling checks (see 12.6) |
| `docs/decomp_renderer.md` | this section |

The measured support is deliberately narrow: the resource is served as the UAV-only emulation the title already uses
for the same format, and the render-target half is named as not hostable. Whether the FSR path needs the attachment
(the console shape allows one) is not measured here; a draw into it would be dropped by the replay and is the next
falsifier if the path ever names it.

### 12.3 The boots and the A/B

The A/B is the same console answer with the support in place against the section-11 Series-S evidence (the previous
binary, which refused the creation), plus default-path control arms. Every arm is the section-12 content environment of
section 11.3 (the class-0xa probe set with `PALMETTO_ALLOC_WATCH`), one boot under the boot lock, presenter captures and
centre-window readbacks; the slot-67 program probe is on in every arm of this section (it reads only). Two binaries
carry the arms: `fsr1`/`fsr2`/`ctl1` ran the first build, SHA-256
`56C937CFFAF183BE353AFB75999B0DDBBC2D8CEDC40AD083A1E05622E92ECF6E`, and `fsr7`/`ctl4` ran the final build of this
section, SHA-256 `83CEC1BE2DA41ECD3EDE61D5D05D0302879A73A9452092B36EA72DA8CA86B8E6`. The two builds differ only in the
narrowing of the render-target-view refusal (a log and a zeroed view for a UAV-only resource; no creation answer) and
in comments, so the Series-S rows are the same measurement twice. One boot per arm, no retries; the first two Series-S
arms ran 180 s windows and the continuation ran a lighter 90 s window without the heavy crash dumps (see 12.5).

| arm | console type | binary SHA-256 (first 16) | wall | stage 3 | stop | device lost | shape creation | FSR programs | class-0xa | captures |
|---|---|---:|---:|---|---|---|---|---:|---|---|
| `s61` (section 11) | 6 | `D7B4D8975D449BA5` | 61.8 s | yes, 51.8 s | `game+0x4EEE4EC` read `0x100000028` | no | refused `0x80070057` | 0 of 29 | none | 10, black |
| `s62` (section 11) | 6 | `D7B4D8975D449BA5` | 61.3 s | yes, 51.6 s | `0x0` execute | no | refused `0x80070057` | 0 of 29 | none | 10, black |
| `fsr1` | 6 | `56C937CFFAF183BE` | 72.3 s | **no** (machine 2 at 68.0 s) | `game+0x55AA29B` read `0x6B9B1EB48` | **yes** | **created** | 0 of 29 | none | 12, black |
| `fsr2` | 6 | `56C937CFFAF183BE` | 65.2 s | **no** (machine 2 at 62.0 s) | `game+0xF229C` read `0x537B384DFA` | **yes** | **created** | 0 of 29 | none | 11, black |
| `ctl1` | unset | `56C937CFFAF183BE` | 82.6 s | yes, 76.7 s | `game+0x13360A3` read `-1` | no | none (title's own upscaler) | 0 of 29 | FORMAT KEY key 0xa / BLOCK ALLOC 0 bytes 256x1 / ENTRY WRITTEN stride 0 | 12, black |
| `ctl4` | unset | `83CEC1BE2DA41ECD` | 62.4 s | **no** (pre-stage-3) | `0x0` execute | **yes** | none (title's own upscaler) | 0 of 29 | none | 11, black |
| `fsr7` | 6 | `83CEC1BE2DA41ECD` | 57.1 s | **no** (machine 2 at 54.0 s) | `0x401F06648` execute | **yes** | **created** | 0 of 29 | none | 10, black |

- **The creation is survived.** All three measured Series-S arms log the shape's `placed texture` line for the 1920x1080 format-67
  description, the backing line, and one named refusal of the render-target view. The
  `CreateCommittedResource refused ... 0x80070057` line is gone. This is the first of the two failures section 11 named;
  the layer now answers the creation with a resource.
- **The second failure is not reached the same way.** The section-11 stop `game+0x4EEE4EC` (the `0x100000000` read) does
  not occur in any of the three Series-S arms. The boot instead stops earlier, in title-side garbage-pointer uses of its
  own (`fsr1` `game+0x55AA29B` reading `0x6B9B1EB48`, `fsr2` `game+0xF229C` reading `0x537B384DFA`, `fsr7` executing
  `0x401F06648`), and every one of them loses the Vulkan device in a long `ExecuteCommandLists` (200-207 ms) just
  before the stop.
- **Stage 3 does not arrive.** All three boots are still at machine stage 2 when they stop (`fsr1` init 2 at 20.0 s,
  loading 2 and session 1 at 64.0 s; `fsr2` init 2 at 24.0 s, loading 2 and session 1 at 60.0 s; `fsr7` init 2 at 16.0 s,
  loading 2 and session 1 at 48.0 s, machine 2 at 54.0 s). The stage-3 window is what section 11 reached 2 of 2 times on
  the previous binary; with the shape now created the boot spends its remaining time in the loading window and stops
  there.
- **The class-0xa command is still absent, for the measured reason.** The `PALMETTO_ALLOC_WATCH` sites saw no stage-3
  executor entry in any of the three Series-S arms, so no FORMAT KEY, BLOCK ALLOC or ENTRY WRITTEN was recorded: the
  executor pass itself is not reached. This is the same honest limitation as section 11: absence is not proof that the
  kind-7 branch skips the command.
- **The default path is unchanged, measured on two binaries.** The first build's control arm `ctl1` (no
  `PALMETTO_CONSOLE_TYPE`) reaches stage 3 at 76.7 s, records the whole class-0xa sequence (FORMAT KEY key 0xa at t=36194.118 with command vtable
  `game+0x66314B0` and creator vtable `game+0x658DB60`, STAGE 3 ENTRY, HELPER ENTRY class 0xa, BLOCK ALLOC of 0 bytes for
  the 256x1 entry with overlap YES, ENTRY WRITTEN with stride 0 and size 0), and stops at `game+0x13360A3` reading `-1`
  0.9 s later, with 12 black captures and no device loss. No Series-S shape line and no FidelityFX DLL mapping appears
  in it. So the translation is off unless the console arm is on, and the class-0xa reference is this binary's own.
- **The FSR programs are still 0 of 29.** The slot-67 program probe records 2,689 slot-67 calls (2,680 distinct
  kind-6 program hashes) in both `fsr1` and `fsr7` - the title's own compute pipeline-library programs - and none of
  them is one of the
  29 translated `amd_fidelityfx_upscaler_xbox` programs (the corpus index's `fspx_code_sha256`/`fspx_state_sha256`
  values). The FidelityFX DLL is mapped, so the kind-7 path still stops before its first upscaler dispatch, which is
  where those programs are created.
- **No pixels changed.** Every presenter capture in every arm of this section reads mean RGB 0.00 (10-12 per boot);
  the FSR output shape is created but nothing presents through it.
- **The continuation series is bounded, and part of it is lost to the host.** After an unrelated host crash and a
  period of GPU instability, the continuation arms dropped the heavy crash dumps (`PALMETTO_DUMP_REGS`,
  `PALMETTO_FAULT_MEM`): `fsr7` is the light 90 s Series-S arm of the final build and `ctl4` its light 120 s control
  (`ctl4` stops before stage 3 at `0x0` execute with a device loss, like `fsr7`, so that pre-stage-3 loss is not
  specific to the console arm; the class-0xa reference stays `ctl1`'s stage-3 boot). Two further arms are not counted:
  `ctl3`/`fsr6` produced no logs before the host crashed, and `fsr6`'s runtime was later found spinning in its crash
  handler for minutes and was killed. A validation-layer arm (`fsr3v`, 180 s, `PALMETTO_VK_VALIDATION=1`) stayed in
  the loading window for its whole window (76,128 D3D12 calls, no exception, no FSR shape) and printed no validation
  message, so it neither names the device loss nor reaches the window.

### 12.4 The stop chain, decompiled

The section-11 stop was decompiled from the image to name what the `0x100000000` read is:

- The record processor is the function at `game+0x4EEE490`, called from the recorder's iterator at `game+0x43A6B40`
  (itself called from the frame update at `game+0x6D8DA0`). Its entry switches on `(*(u32 *)record) >> 3`: a
  ten-way dispatch over the title's recorded command types.
- Case 4 (the measured record type; the record's handler index is the byte at record+0xC) reads the per-type object at
  `context+0x508 + index*8`, takes the object's first qword as a pointer, reads slot `+0x28` of it, and compares that
  value against the two functions `game+0x4EEEE90` and `game+0x4EEECF0` to pick the handler variant.
- In `s61` the object's first qword is `0x100000000`, so slot `+0x28` faults at `0x100000028`. A neighbouring title
  function's disassembly shows the same constant used as the **console GPU-address base**: `MOV R10,0x100000000` and
  `MOV R9,-0x100000000`, with the low 32 bits of a GPU address split off to index a static table. The value is therefore
  the title's own GPU-address-space base in a slot that the record processor treats as a pointer, not a value this
  layer handed out for the refused creation (that creation answered `E_INVALIDARG` and no resource, and the shape's
  placement VA was a title VA, not `0x100000000`). This is the same unified-memory family [boot_frontier.md](boot_frontier.md)
  measures: the title dereferences a console GPU address as CPU memory.

The new stops are not on that chain: `fsr1`'s `game+0x55AA29B` is a table lookup indexed by the low 32 bits of a
title global that reads `0x171360A23D` (an out-of-range index into a static table), `fsr2`'s `game+0xF229C` reads
through `0x537B384D86`, and `fsr7` executes `0x401F06648` (a call through a bad function pointer). Every one of those
boots lost the Vulkan device in the submission just before the stop, so whether the stops are the device loss's
downstream effect or the FSR path's own state is not separated by this slice.

### 12.5 The next wall and its falsifier

The FSR path now survives its shape creation and proceeds through the DLL load and its context's texture creations,
then stops in the loading window before the frame loop's stage-3 upscale dispatch. The next falsifier is in that
window, in this order:

1. **Name the device loss.** A validation-layer Series-S arm (`PALMETTO_VK_VALIDATION=1` with the host layer's
   standard suppression variables, [testing.md](testing.md)) catches an invalid usage at the submit that loses the
   device; a clean run moves the question to the title-side garbage stops. `fsr1`/`fsr2` are the control pair for
   that arm. The one attempt made (`fsr3v`, 180 s) stayed in loading and printed no validation message, so it does
   not decide this: the next arm needs the FSR window reached with the layer actually enabled.
2. **Name what the title does with the served shape.** The render-target view of the shape is refused by name; whether
   the FSR path draws into it (and the replay then drops the draw) is not measured. A per-resource record of the
   R9G9B9E5 views the title creates (the descriptor probes already exist for T#s) is the smallest measurement that
   decides whether the attachment half must be emulated.
3. **Re-watch the class-0xa sites.** Once an arm reaches stage 3 with `PALMETTO_CONSOLE_TYPE=6` and the shape served,
   the `PALMETTO_ALLOC_WATCH` sites answer the picture question directly. No arm of this slice reaches them.

### 12.6 Tests and gates

- `port/test/xs_proxy_mock_test.cpp` (`check_console_texture_translation`) gains the shape predicate's cases: the
  measured 1920x1080 RT+UAV description is the one shape accepted; render-target without unordered access, unordered
  access alone, a colour format with the same flags, the depth-stencil flag, a 3D dimension, 2 samples and 2 mips are
  refused. The console-type spelling checks accept `6`, `seriess`, `series-s`, `series s` in mixed case and with
  spaces, and refuse unset/empty, `0`, `off`, `1`, `pc`, `7`, `seriesx`, `series-x`, `8`, `devkit`, `06`, `6x` and
  `series`.
- Gates on the changed tree: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK` and `_setup\build_tests.cmd` prints
  `TESTS_BUILT`; the `xs_proxy_mock` target and the other gate targets of section 11.7 pass, and every
  `tests/test_docs_*.py` check passes (the new `PALMETTO_SLOT67_PROGRAM_LOG` and the reused
  `PALMETTO_CONSOLE_TYPE` are named here).
- The binaries of the arms: `fsr1`/`fsr2`/`ctl1` ran
  `56C937CFFAF183BE353AFB75999B0DDBBC2D8CEDC40AD083A1E05622E92ECF6E` and `fsr7`/`ctl4` ran the final tree's
  `83CEC1BE2DA41ECD3EDE61D5D05D0302879A73A9452092B36EA72DA8CA86B8E6`; the arm reports, captures and traces stay
  outside the repository (D:\leonida-work\fsrsurvive). The two builds differ only in the render-target-view refusal's
  narrowing and in comments (12.3).
- The uncommitted files at the end of the slice are the five of this section (`port/src/console_texture.h`,
  `port/shims/d3d12_vulkan_device.cpp`, `port/shims/d3d12_xs_device.cpp`, `port/test/xs_proxy_mock_test.cpp` and this
  page); the loader files and the parallel console-type slice's own new page in the same tree are that slice's, not
  this one's.

### 12.7 Attribution

This section was implemented from this project's own measurements: the boot log's shape/refusal lines and the
FidelityFX program probe of the arms above; the decompiled record dispatcher (`game+0x4EEE490`, `game+0x43A6B40`,
`game+0x6D8DA0`) and the GPU-address-base constant of `game+0x55AA250`; the host GPU's format-property answers the
runtime itself prints; and the existing emulation the device documents in [d3d12_vulkan.md](d3d12_vulkan.md) section
3.7. The console-type switch and its Series-S evidence are section 11's. No peer code was used and no game data is
added: RVAs, VAs, format numbers, flag words, dimensions, counts, program hashes and outcomes only.


## 13. The Series-S arms still stop before stage 3: the loss is not the upscale shape, and this slice cannot name it

**Status: measured, negative.** Section 12 left three questions: what loses the Vulkan device in the Series-S window,
whether the shape's render-target-usage backing is what destabilises it, and whether the kind-7 branch and stage 3 can
be made to coincide so the class-0xa question is answered. This section takes the first two to measured answers and the
third to a bounded negative. The loss is not the shape: a control arm that never creates the Series-S shape loses the
device the same way, and the shape's backing image is byte-identical to the UAV-only R9G9B9E5 emulation the title
already creates. Nothing in the tree removes the fault: the validation layer cannot reach the faulting window on this
host, and the driver's own `VK_EXT_device_fault` report crashes inside the driver when asked. Every Series-S arm still
stops in the loading window at machine stage 2, so the class-0xa command still has no coincidence to be measured
against, and its absence stays an absence of the executor pass rather than a skip.

### 13.1 The arms

Every arm is the section-12 content environment, one boot under the shared boot lock, light probe set (no
`PALMETTO_DUMP_REGS`/`PALMETTO_FAULT_MEM`), presenter captures and centre readbacks; the Series-S arms add
`PALMETTO_CONSOLE_TYPE=6`. All three ran the section-12 tree plus a later-reverted fault-report diagnostic: `df1` and
`fsr9` never switch it on, `df2` does (13.4). The binary's SHA-256 starts `0896475FEEE5D0E4`. Reports, logs and
captures stay outside the repository at D:\leonida-work\fsrstable.

| arm | console type | window | wall | machine stage 3 | stop | device lost | shape creation | FSR programs | class-0xa | captures |
|---|---|---:|---:|---|---|---|---|---:|---|---|
| `df1` | 6 | 90 s | 90.2 s, killed | no (machine 2 at 86.4 s) | timeout | yes, ~82 s | created at 78.0 s | 0 of 29 | none | 13, black |
| `df2` | 6 | 120 s | 55.9 s | no | `host:nvoglv64.dll+0xE86D7C` write `0x0` (the fault-report call, 13.4) | yes | created | 0 of 29 | none | - |
| `fsr9` | 6 | 180 s | 66.9 s | no (machine 2, loading 2 since 58.0 s) | `game+0x86F981` read `0x20787463C1` | yes, ~64 s | created at ~58 s | 0 of 29 | none | 13, black |

The three Series-S arms of section 12 (`fsr1`, `fsr2`, `fsr7`) stopped in the same loading window for the same reason;
`game+0x86F981` is the pre-stage-3 family section 11.5 already measures. No arm of either section reaches machine
stage 3 with the console answer active.

### 13.2 What the loss is, and what it is not

- **It is a GPU-side error in executed work.** Each detected loss is a Vulkan `VK_ERROR_DEVICE_LOST` (the layer's own
  `[rt] replay: the Vulkan device was lost` line), and the Windows system log carries the NVIDIA driver's GPU-context
  error (nvlddmkm event ID 153, `Error occurred on GPUID: 100`) as a pair at the same instant: `df1` at
  12:52:39.750/12:52:41.520 inside its 90 s window, `df2` at 12:54:06/07, `fsr9` at 13:08:38/39, section 12's
  post-reboot `ctl4` at 12:30:31/33 and `fsr7` at 12:35:59/12:36:01. That driver event is what a GPU fault or a lost
  context logs, not what a creation refusal or a layer bookkeeping error logs; the timing also puts it in the window
  whose submissions run the content work, not next to a refused creation.
- **The event does not require a detected loss.** Section 11's Series-S arms `s61`/`s62` show the same driver event
  family (11:06:13/15 and 11:07:15/17) while the layer never reported a loss, because the title's own CPU-side stop
  came before any wait reached the GPU. The event is the GPU half of the fault; whether a boot reports it depends on
  whether it waits afterwards.
- **It is not the Series-S shape.** Section 12's control arm `ctl4` has no `PALMETTO_CONSOLE_TYPE`, no Series-S shape
  line and no FidelityFX mapping, and lost the device at 12:30:31/33, stopping at a `0x0` execute. A control arm cannot
  fault through a resource it never creates, so at least one member of this fault family is independent of the shape
  and the console branch.
- **It derails the boot after the fact.** `df1` lost the device at ~82 s, its own handler recorded an exception at
  `game+0x43A6B7A` reading `0x27600000036` at 84 s, and the boot was still at machine 2 when the 90 s window killed it.
  `fsr9` stopped at `game+0x86F981` reading `0x20787463C1` about two seconds after its loss.

### 13.3 The render-target-usage hypothesis is refuted at the creation

The shape's render-target flag was the section-12 change, so this slice first asked whether that flag's backing is the
destabiliser. It cannot be, at the image level:

- `texture_format` in `port/shims/d3d12_vulkan_device.cpp` serves both the Series-S shape and the UAV-only R9G9B9E5
  texture as one `Format`: the mutable `VK_FORMAT_R32_UINT` backing with `uint_backed =
  VK_FORMAT_E5B9G9R9_UFLOAT_PACK32`.
- `TextureCreateInfo` then builds one `VkImageCreateInfo` for both: `TRANSFER_SRC|TRANSFER_DST`, `COLOR_ATTACHMENT`,
  `STORAGE` and `SAMPLED`; `MUTABLE_FORMAT|EXTENDED_USAGE`; view formats `{R32_UINT, E5B9G9R9}`; one sample, one mip,
  one layer. The render-target flag's own `COLOR_ATTACHMENT` addition is redundant: the unordered-access half already
  adds the same usage, because the backing format (`R32_UINT`) is colour-attachment-usable on this host.
- So "create the shape without render-target usage" removes no image usage, and the only difference the flag makes in
  the layer is the D3D12 flag in its binding plus the render-target view refused by name. The empirical half of the
  answer is the control arm above, which never creates the shape at all.

What remains is the GPU work the boot submits after the shape is served. In all three Series-S arms the loss follows
the shape's creation by 4-7 s (`df1` shape 78.0 s, loss ~82 s; `fsr9` shape ~58 s, loss ~64 s; section 12's `fsr7`
shape ~48 s, loss ~55.9 s), while the control arm's loss lands in the same loading-window phase without a shape. This
slice does not separate those two observations, and it does not claim the FSR path causes the fault.

### 13.4 Two attempts to name the fault, with their measured limits

**The validation layer does not reach the window.** The section-12 validation arm (`fsr3v`, `PALMETTO_VK_VALIDATION=1`,
light, 180 s) recorded 76,128 D3D12 calls and was still at loading 1/session 0 at 178 s, printing no validation
message. A normal boot reaches the fault window at about 144,000 calls in 57 s (about 2,500 calls/s); the validation
arm ran at about 420 calls/s and had not entered the loading window at 180 s, so the projection to reach the window is
past 600 s - not a bounded arm under the shared boot lock on this host.

**The driver's fault report crashes the driver.** `VK_EXT_device_fault` is listed by the GPU (`deviceFault = true`,
`deviceFaultVendorBinary = true`), so a bounded opt-in report was implemented for this slice: enable the extension and
its `deviceFault` feature, and on the first loss ask `vkGetDeviceFaultInfoEXT` for the address and vendor infos. In
`df2` the report call crashed inside the NVIDIA driver: `nvoglv64.dll+0xE86D7C` writing `0x0`, with the stack
`report_device_fault` <- `VulkanContext::mark_device_lost` <- `VulkanQueue::submit` <- `execute_now` <- `execute` <-
`ExecuteCommandLists` <- the title, symbol-resolved against this build's own PDB. This driver will not answer the
question, and the diagnostic was reverted: the tree carries no device-fault switch.

The next falsifier for this question is `VK_NV_device_diagnostic_checkpoints` (listed by the same GPU, revision 2): one
checkpoint per submission, then `vkGetQueueCheckpointDataNV` after a loss, would name the submission that faulted even
when the fault report cannot. It was not implemented here because a checkpoint query is one more driver call on a lost
device, the call family that just crashed.

### 13.5 The Series-S arms and the class-0xa question

- **No stage 3.** `df1`, `df2` and `fsr9` are all at machine 2 with loading 2 and session 1 in their last state traces
  (`df1` at 86.4 s with loading 2 since 78.0 s; `fsr9` at 64.0 s with loading 2 since 58.0 s). The stage-3 pump never
  fires: the alloc watch starts at `[rt] alloc watch: waiting for machine stage 3` and stays waiting.
- **The class-0xa command is absent, and for the honest reason.** `PALMETTO_ALLOC_WATCH` saw no stage-3 executor entry
  in any arm, so no FORMAT KEY key 0xa, no BLOCK ALLOC of 0 bytes for the 256x1 entry and no ENTRY WRITTEN with stride
  0. The executor pass is not reached; nothing measured says the kind-7 branch skips the command.
- **The FSR programs stay at 0 of 29.** Each arm records 2,689 slot-67 program calls (2,680 distinct kind-6 program
  hashes) and 0 of the 29 translated `amd_fidelityfx_upscaler_xbox` corpus hashes, with the FidelityFX DLL mapped. The
  section-12 count is unchanged.
- **No pixels move.** Every presenter capture in every arm is mean RGB 0.00 (13 per arm): the shape is created and
  nothing presents through it.
- **Goal verdict.** Stage 3 and the Series-S answer do not coincide in this slice either, so the class-0xa question
  section 11 raised stays unanswered. The question needs an arm that survives the loading window to the stage-3
  executor pass with `PALMETTO_CONSOLE_TYPE=6`; no such arm exists in this tree's measurements.

### 13.6 Limits

- **The host GPU/driver state is the limiting variable.** Every post-reboot arm that ran lost the device: section 12's
  `ctl4` (12:29) and `fsr7` (12:35), this section's `df1` (12:51), `df2` (12:53) and `fsr9` (13:07). Before the host
  reboot at 12:07, four arms reached machine stage 3 without a detected loss (`def3`, `s61`, `s62`, and the control
  `ctl1` that records the class-0xa sequence) while `def1`, `def2`, `fsr1` and `fsr2` did lose it, so the pre-reboot
  window was not clean either and the loss is not proven to come from the reboot. The samples are small on both sides
  (five post-reboot losses, four pre-reboot survivals), so this is a variable to name, not a proven cause.
- **The samples are small and the boot is a race.** Three Series-S arms here and three in section 12 all stop before
  stage 3; nothing measured here shows a longer or luckier window could not reach it.
- **The driver event has no address.** "GPU-side error" is inferred from the nvlddmkm event and the detected loss; the
  fault address is exactly what the two attempts in 13.4 could not obtain.
- **The binary of the arms is not the final tree.** It is the section-12 tree plus a reverted diagnostic that the arms
  do not switch on; the Series-S creation and dispatch path they exercise is the section-12 one. No game data is added:
  RVAs, format numbers, flag words, event IDs, counts, hashes and outcomes only.

### 13.7 Tests and gates

- **No code change survives this section.** The validation switch and the fault-report diagnostic were probes; the
  diagnostic's source change is reverted, so the section is a docs-only change and the section-12 code paths are
  untouched.
- **Tests.** `_setup\build_gta6rt.cmd` prints `PALMETTO_OK` and `_setup\build_tests.cmd` prints `TESTS_BUILT` on this
  tree; the 17 fast gate-relevant CTest targets (graphics_record, graphics_pso, buffer_va_index, deferred_compile, the
  three pe_loader targets, unimplemented_policy, dstorage, the six xs_proxy_mock variants, xapu_chad and frame_dump)
  pass, and every `tests/test_docs_*.py` check passes. The two GPU-bound gate binaries (`d3d12_fence_timing_test`,
  `xbox_present_test`) were built but not run in this session: the host's GPU state is the measured limiting variable
  (13.6), and another device loss was not worth risking for a docs-only change.
- **The arms' artifacts** (reports, logs, captures) stay outside the repository at D:\leonida-work\fsrstable; the
  uncommitted files at the end of the slice are this page and the parallel movie probe's `port/src/main.cpp` changes,
  which are not this section's.

### 13.8 Attribution

This section was implemented from this project's own measurements: the boot logs of the arms above, the Windows
system log's NVIDIA driver events, the crash stacks symbol-resolved against this build's PDB, and the device code the
section-12 page already documents. The Vulkan feature names (`VK_EXT_device_fault`,
`VK_NV_device_diagnostic_checkpoints`) and their query calls are the Vulkan specification's, not a peer project's. No
peer code was used and no game data is added: RVAs, format numbers, flag words, dimensions, counts, program hashes and
outcomes only.

