# Retail TA game-data parsing (TotalA.exe 3.1c, GOG)

Observed behaviour of `TotalA.exe` (base 0x400000) when it reads TDF/FBI/GUI data.
Everything here comes from static analysis plus unicorn emulation of the retail
routines on the extracted retail data (`assets/tadata`). No binary code or data
is reproduced in the engine; addresses are given as evidence only.

Tools (all observe-only) in `tools/re/`:
`data_dis.py` (plain disassembly), `data_dump.py` (disassembly annotated with TDF-getter
names and strings), `data_callers.py` (E8 call-site finder), `data_emu.py` (unicorn
harness: `tdf`, `unit NAME`, `weapon FILE N`, `move`).

Legend: **[C]** confirmed by emulation of the retail routine, **[S]** read from the
disassembly (static, unambiguous), **[U]** unconfirmed / inferred. `fx16` means
16.16 fixed point (value × 65536). Addresses are VAs.

Game object: `G = [0x511de8]`.

---

## 1. TDF syntax (shared by FBI, weapons, features, gamedata, download, GUI)

Loader `TDF_load` 0x4c2f60 (file through the VFS), `TDF_parseMem` 0x4c3120 (same pipeline from a
buffer). Both run the comment stripper 0x4c33a0 and then the recursive node parser
0x4c3e40 on a root node named `"root"`.

| Rule | Behaviour | Evidence |
|---|---|---|
| Line comments | `//` up to (not including) `\n` is overwritten with spaces, **anywhere**, including inside values. `url=http://x;` loses everything from `//` to end of line, so the value runs on to the next `;` on a later line and the key on that line is swallowed. | 0x4c33a0 [C] |
| Block comments | `/* ... */` overwritten with spaces, no nesting; unterminated runs to EOF. | 0x4c33a0 [S][C] |
| Whitespace | Space, TAB, CR and LF are skipped between tokens. Keys, values and section names are trimmed of those four characters at both ends (0x4c4340). Inner whitespace is kept (`sp = a b  c ;` gives `"a b  c"`). | [C] |
| Section | `[name]` then optional whitespace then `{` (anything else: error "Sub-record - opening '{' not found"). Name is the trimmed text between `[` and the first `]`. Sections nest. | 0x4c4068 [S] |
| Key/value | `key = value ;`. The key is the text from the current position to the first `=`; the value is the text after `=` up to the first `;` (it can span lines, and it can never contain `;`). There is no quoting or escaping. | 0x4c3ef5 [S][C] |
| End of section | `}`. A `}` at root level ends parsing; the rest of the file is ignored. EOF inside a section raises "End of file - nextblock not zero". | 0x4c4165, 0x4c41de [S] |
| Errors | Missing `=`, `;`, `]` or `{` are reported through 0x4b6290, the same routine that reports fatal "Can't load X.TDF" messages, with " - name = '%s' from file %s" appended. | 0x4c411b.. [S] |
| Section lookup | `TDF_enter` 0x4c3410: linear, case-insensitive (`_stricmp` 0x4f8a70) search of the **direct children of the current node** (or of root when the current node is null). The **first** match wins, so for duplicate sections only the first one is used. It sets the current node (null when nothing is found). Nested sections are not reachable from root. | [C] |
| Key lookup | Each node keeps keys in a vector sorted case-insensitively and looks them up by binary search. A key that repeats (in any case) overwrites or shadows the earlier one, so the **last occurrence wins**. Keys are case-insensitive. A subsection's keys are not visible from its parent. | 0x4c3f4a insert, 0x4c46c0 lookup [C] |
| `GetInt(key,def)` 0x4c46c0 | MSVC `atol` (0x4e4ed0): skips leading whitespace, accepts one `+` or `-`, takes decimal digits only and stops at the first non-digit. **No hex** (`0x1F` gives 0), `2.9` gives 2, `-2.9` gives -2, and overflow wraps modulo 2^32 (`4294967297` gives 1). Returns `def` only when the key is absent; a present-but-empty value gives 0. | [C] |
| `GetDouble(key,def)` 0x4c4760 | MSVC `atof` (strtod semantics), returned as a double. | [S] |
| `GetFixed16(out,key,defRaw)` 0x4c4800 | `trunc(atof(v) * 65536.0)` (`_ftol` truncates toward zero). If absent, `*out = defRaw` (already fixed). `1.5` gives 0x18000, `2.9` gives 0x2e666, `-2.9` gives 0xfffd199a, `-0.00001` gives 0. | [C] |
| `GetStr(dst,key,max,def)` 0x4c48c0 | `strncpy(dst,val,max); dst[max-1]=0`, so values are truncated to `max-1` characters. If absent, the default is copied and 0 is returned (1 when found). | [C] |
| `GetLocStr` 0x4c58a0 | Tries `<langprefix><key>` first (for example `FrenchName`; the prefix is the global at 0x51fdc0, empty in the English build) and then `<key>`. Used for unit `name`/`description`. | [S]; prefix source [U] |
| Floating point | The CRT FP init (0x4e4320 → 0x4ea5b0 = `_controlfp(_PC_53,_MCW_PC)`) puts the x87 in 53-bit precision, so every `atof*k` scale below behaves like IEEE-754 double multiplication followed by truncation. With 64-bit precision the result differs, e.g. weaponvelocity 450 gives 983039 instead of 983040. | [S][C] |
| Section CRC | Each node stores a checksum (0x4b6ba0) of its body text at node+0x25. It is used for the unit/weapon sync checksum (§2.1). | [S] |

---

## 2. Units (FBI)

### 2.1 Load pipeline
1. **Weapon TDF cache for checksums** 0x42a8d0: every `weapons\*.tdf` is loaded as a TDF object (array 0x5122a0, count 0x5122a4).
2. **Pre-pass** 0x42a8d0: enumerate `units\*.fbi`. Record size **0x249**. Index 0 is a dummy record named `None`, flagged valid. For each file:
   - +0x13e = checksum of the file bytes (0x4b6ba0), optionally replaced by an `OVR` "Compatability" override.
   - `[UNITINFO]` reads `name` (localized), `unitname`, `side` (+0xa0 char[30]), `ai_weight` (+0xbe char[64]), `ai_limit` (+0xfe char[64]), `objectname`, `buildcostenergy`, `buildcostmetal`, `norestrict`, `wacky` (flagsB bit16).
   - +0x146 = XOR of the section CRCs of the `weapon1/2/3/explodeas/selfdestructas` weapon sections. Each is found by searching every loaded weapon TDF for a top-level section with that name.
   - **Version gate**: `Version` (double) is split into maj=floor(v) and min=floor((v−maj)·10). The unit is valid (flagsA bit23) iff maj < G[+1] or (maj == G[+1] and min ≤ G[+2]). The values of G[+1]/G[+2] are [U]; all retail Versions (1, 1.2, 3.0) load.
   - **Copyright gate**: `Copyright` (default `"Run to the Village!  Warn your brother!"`, max 0x80) has the 4 characters at offset 10 replaced by `0000` and must then equal `Copyright 0000 Humongous Entertainment. All rights reserved.` exactly (case-sensitive). Otherwise the unit is silently invalid. All 278 retail FBIs carry the line.
   - Two more gates use globals 0x50289c / 0x511de4 and whether the file came from an archive [U].
   - Invalid units are compacted out. "Incompatible units found..." is shown only when units were dropped by the version gate alone.
3. **Sort**: valid records are sorted case-insensitively by `unitname` (0x42d4ad..0x42d612, comparator 0x42db60). Each record's **type id** (+0x21e) = index after the sort; 0 is the dummy. Bits needed for ids → G+0x14393. Unit lookup by name (0x488b10) is a binary search; id 0 means not found.
4. **Full parse** 0x42bf40 for each unit (table below), model `objects3d\<objectname>.3DO`, GUI page count (§5), script `scripts\<unitname>.COB`.
5. `moveinfo.tdf` 0x42d2e0 is loaded before step 4. `sidedata.tdf [CANBUILD]` and `download\*.tdf` come after (§5).

### 2.2 `[UNITINFO]` keys (0x42bf40). Record = 0x249 bytes.
Default is 0 unless stated. "int" = GetInt (atol), "dbl" = GetDouble, "fx16" = GetFixed16.

| Key | Off | Stored as | Parse / scale | Default | Notes |
|---|---|---|---|---|---|
| name | +0x00 | char[32] | GetLocStr max 32 | (unchanged) | [C] |
| unitname | +0x20 | char[32] | str | "" | primary key; sort key [C] |
| description | +0x40 | char[64] | GetLocStr | | [C] |
| objectname | +0x80 | char[32] | str | = unitname | 3DO name [C] |
| side | +0xa0 | char[30] | str | "" | pre-pass only [S] |
| ai_weight / ai_limit | +0xbe / +0xfe | char[64] | str | "" | pre-pass [S] |
| defaultmissiontype | +0x230 | u8 | name looked up in mission table (case-insensitive binary search 0x438760) | | values seen: Standby, VTOL_standby, GUARD_NOMOVE, Standby_Mine; not-found result [U] |
| wpri_badTargetCategory / wsec_ / wspe_ | +0x231/+0x235/+0x239 | ptr to category bitset | whole string (max 99 chars) is **one** category name | "none" | [C] for wpri |
| noChaseCategory | +0x23d | ptr | as above | "none" | [S] |
| buildcostenergy / buildcostmetal | +0x186 / +0x18a | float | **int** then float (12.9 gives 12.0) | | [C] |
| maxvelocity | +0x192 | i32 | fx16 | 0 | 1.2 gives 78643 [C] |
| (derived) | +0x196 | i32 | maxvelocity·65536 / ((maxslope+1)·65536) as 64-bit integer division | | 1.2 & maxslope 20 give 3744 [C]; use [U] |
| brakerate / acceleration | +0x19a / +0x19e | i32 | fx16 | 0 | [C] |
| bankscale | +0x1a2 | i32 | fx16 | 0x10000 | [C] |
| pitchscale | +0x1a6 | i32 | fx16 | 0 | [C] |
| damagemodifier | +0x1aa | i32 | fx16 | 0x10000 | [C] |
| moverate1 / moverate2 | +0x1ae / +0x1b2 | i32 | fx16 | 2·maxvelocity (raw) | [C] |
| turnrate | +0x1ba | i16 | int, truncated to 16 bits (70000 gives 4464) | | [C] |
| waterline | +0x22c | u8 | int, low byte (300 gives 44) | | [C] |
| transportsize / transportcapacity | +0x22a / +0x22b | u8 | int | | [C] |
| energymake / energyuse / metalmake / extractsmetal | +0x1c2/+0x1c6/+0x1ca/+0x1ce | float | dbl | | [C] |
| makesmetal | +0x22d | u8 | **int** | | [S] |
| windgenerator / tidalgenerator | +0x1d2 / +0x1d6 | float | dbl | | [C] |
| energystorage / metalstorage | +0x1e2 / +0x1e6 | float | dbl | | [C] |
| buildtime | +0x1ea | i32 | int | | [C] |
| workertime / healtime | +0x1fe / +0x200 | i16 | int | | [C] |
| maxdamage | +0x1fa | i32 | int | | [C] |
| sightdistance, radardistance, sonardistance | +0x202/+0x204/+0x206 | i16 | int | | [C] |
| radardistancejam, sonardistancejam | +0x20a/+0x20c | i16 | int | | [C] |
| bmcode | +0x22f | u8 | int | 0 | 0 = building: the yardmap is parsed only then [C] |
| cloakcost | +0x1da | float | **int** then float (0.5 gives 0) | | [C] |
| cloakcostmoving | +0x1de | float | int | = int(cloakcost) | [C] |
| mincloakdistance | +0x208 | i16 | int | 0; **80 if cloakcost>0 and the value is 0** (0x42d12a) | [C] |
| buildangle / builddistance | +0x210 / +0x212 | i16 | int | | [C] |
| sortbias / cruisealt | +0x21a / +0x21c | i16 | int | | [C] |
| maneuverleashlength / attackrunlength / kamikazedistance | +0x214/+0x216/+0x218 | i16 | int | | [C] |
| selfdestructcountdown | flagsB bits20–22 | 3 bits | atol & 7 (9 gives 1) | **5** when absent | [C] |
| category | bitsets | — | tokens split on whitespace (`" %s %n"`, max 99 chars); each token names a category (created on demand, case-insensitive) whose 512-bit set gets this unit id; the unit is also added to `ALL` | | 0x488e70 [S] |
| soundcategory | +0x20e | u16 | index of the `sound.tdf` section with that name (stricmp); if none matches, **atol(value)**; absent gives 0 | | [S] |
| corpse | +0x1bc | u16 | feature index (loads the feature on demand, 0x422e40) | 0xffff | [C] |
| movementclass | +0x1b6 | ptr | moveinfo class by `name` (stricmp, 0x440420). **If found, the class supplies footprint/water depth/slopes and the FBI's own values are ignored**; otherwise the FBI keys below are read | null | [C] |
| footprintx / footprintz | +0x14a / +0x14c | i16 | int (FBI or class) | 0 | [C] |
| maxwaterdepth / minwaterdepth | +0x1be / +0x1c0 | i16 | int | 10000 / −10000 | [C] |
| maxslope / maxwaterslope | +0x228 / +0x229 | u8 | int; then maxslope = min(maxslope, maxwaterslope) | 255 / 255 | badslope/badwaterslope are parsed but not copied [C] |
| weapon1/2/3 | +0x1ee/+0x1f2/+0x1f6 | ptr to weapon record | name (max 127) matched against weapon **section names**, IDs 0..255 in order, stricmp (0x49e5b0); missing, empty or unknown gives **weapon ID 0** (`NOWEAPON`) | weapon[0] | [C] |
| explodeas / selfdestructas | +0x220 / +0x224 | ptr | same lookup | weapon[0] | [C] |
| (derived) has-weapon | flagsA bit16 | | set iff any of weapon1/2/3 ≠ weapon[0] | | [S] |
| yardmap | +0x14e | u8[fx·fz] | see §2.4 (only when bmcode==0; max 1023 chars) | | [C] |
| (derived) footprint box | +0x15e/+0x166 = −(fx·16·65536)/2, −(fz…)/2; +0x16a/+0x172 = +; +0x176/+0x17e = size; +0x182 = (sx+sz)/3 | fx16 world units, 16 per cell | | | [C] |
| (model) | +0x162=0, +0x16e = model height (0x4cb5f0), +0x17a = height | | | | [S] |

**flagsA (+0x241)** — one bit per key, value `atol(v)&1` unless noted [C/S]:
bits0–1 standingmoveorder (&3, default **2**); bits2–3 standingfireorder (&3, default **2**);
4 init_cloaked; 5 downloadable; 6 builder; 7 zbuffer; 8 stealth; 9 isairbase;
10 istargetingupgrade; 11 canfly; 12 canhover; 13 teleporter; 14 hidedamage; 15 shootme;
16 has-weapon (derived); 17 armoredstate; 18 activatewhenbuilt; 19 floater; 20 upright;
21 amphibious; 23 *valid/compatible* (pre-pass); 24 isfeature; 25 noshadow;
26 immunetoparalyzer; 27 hoverattack; 28 kamikaze; 29 antiweapons; 30 digger;
31 `guis\<unitname>0.GUI` exists (derived).

**flagsB (+0x245)**: 0 mobilestandorders; 1 firestandorders; 2 onoffable; 3 canstop;
4 canattack; 5 canguard; 6 canpatrol; 7 canmove; 8 canload; 10 canreclamate (**bit 9 = copy of
bit 10**); 11 canresurrect; 12 cancapture; 13 *can cloak* (derived: int(cloakcost) > 0);
14 candgun; 15 norestrict; 16 wacky; 17 showplayername; 18 commander;
19 cantbetransported; 20–22 selfdestructcountdown.

ARMCOM emulated: flagsA 0x0414c0c8, flagsB 0x0056f6fb (matches the table) [C].

### 2.3 Keys present in retail FBIs that TotalA.exe never reads
No string for these exists in the exe [S]: `TEDClass`, `ThreeD`, `UnitNumber`,
`Designation`, `SteeringMode`, `Ovradjust`, `Scale`, `BadTargetCategory` (unprefixed),
`NoAutoFire`, `AltFromSeaLevel`, `TransMaxUnits`/`TransportMaxUnits`, `Resurrect`.
`<Lang>Name`/`<Lang>Description` are read only through the language prefix.
**There is no `canbuild` key in FBIs**; see §5.

### 2.4 YardMap
0x42cf3e..0x42d071. Read only when bmcode==0. Cells are filled row-major (x fastest) for
fx·fz cells. Characters outside the table below (including spaces and lowercase `g`) are
**skipped without consuming a cell**. When the string ends early, the **last valid
character repeats** for every remaining cell; extra characters are ignored. Retail data
depends on both: 46 buildings have yardmaps shorter or longer than fx·fz. An absent or
empty yardmap on a bmcode-0 unit scans past the buffer (undefined; does not occur in
retail) [C].

| Char | Code | Bits set |
|---|---|---|
| `.` | 0x00 | – |
| `y` | 0x29 | 0,3,5 |
| `O` | 0x2b | 0,1,3,5 |
| `c` | 0x2d | 0,2,3,5 |
| `o` | 0x2f | 0,1,2,3,5 |
| `Y` | 0x31 | 0,4,5 |
| `C` | 0x35 | 0,2,4,5 |
| `w` | 0x37 | 0,1,2,4,5 |
| `f` | 0x6f | 0,1,2,3,5,6 |
| `G` | 0x8f | 0,1,2,3,7 |

Observed consumers:
- bit0: cell belongs to the building (0x47ce2d marks map cells) [S].
- bit1 / bit2: block movement, chosen by the unit's yard state. The mask is 2 when unit+0x10f bit2 is set, else 4 (0x47c804, 0x47cd88) [S]. So `o`/`w`/`f`/`G` always block, `y`/`Y` never block, `c`/`C` block in one yard state only, `O` in the other. Which state is "open" is [U].
- bit3: cell's terrain height takes part in the min/max used for the placement height. bit4: max only, and a water-level placement path is used when these dominate (0x47d8ce..0x47d948) [S].
- **Building placement test 0x47d2e0** (x ≥ 1, z ≥ 1, x+fx < W, z+fz < H first) [S]. Over
  the cells' 2×2 corner max/min heights (map cell +5/+6, retail-pathfinding §1.1): bit3
  cells give `lo` = min of their min and `hi` = max of their max; bit4 cells give `wHi` =
  max of their max (0x47d4f9..0x47d52c). With no bit3 cell the base is the u8 `sea −
  waterline` (0x47d773); otherwise `hi − lo > maxslope` (+0x228) fails and the base is `lo`.
  Then `wHi > base` fails (0x47d7b6), `lo < sea − maxwaterdepth` (+0x1be) fails (0x47d7ba)
  and `max(hi, wHi) > sea − minwaterdepth` (+0x1c0) fails (0x47d7df); the base is stored at
  0x51e684. An all-water yard (`w`/`C`/`Y`: floating structures, shipyards) therefore never
  meets its FBI maxwaterdepth (lo stays 255): it needs only its highest point at or below
  both `sea − waterline` and `sea − minwaterdepth`. bit0 cells fail on a map cell with
  +0xc bit 1, bit1/2 cells on another unit's occupancy, bit5 on a blocking feature, bit6 on
  a feature with def +0xff bit 1, and a bit7 yard needs some cell on a feature with def
  +0xfe bit 5 (geothermal) [S].
- bits 5/6/7: 7 = `G` (geothermal) and 6 = `f` are [U].

### 2.5 moveinfo.tdf (0x42d2e0 → 0x440340)
Static init 0x440230 gives each of the 32 classes: name null, fx=fz=0, maxwaterdepth 10000,
minwaterdepth −10000, all slopes 255. Then for `i=0..31` (each tried independently, gaps
allowed) `[CLASS<i>]`: `name` (max 99, strdup), `footprintx`, `footprintz`, `maxwaterdepth`,
`minwaterdepth`, `maxslope`, `badslope` (default maxslope/2), `maxwaterslope`,
`badwaterslope` (default maxwaterslope/2), all int. Clamps: maxslope = min(maxslope,
maxwaterslope); badslope = min(badslope, maxslope); badwaterslope = min(badwaterslope,
maxwaterslope). Class record 0x20 bytes at 0x512358 [C]. Emulated: CLASS0 KBOTSS2 → slope
32/16, water slope 255/127; CLASS13 TANKHOVER4 → 12/12/255/255 [C].

---

## 3. Weapons

### 3.1 Loading and resolution
- 0x42e310 clears 256 records of **0x115** bytes at G+0x2cf3, then for every
  `weapons\*.tdf` (VFS enumeration order, unsorted [S]; order [U]) parses every top-level
  section n (0x4c3490 = nth child) with 0x42e440.
- Slot = `ID` key (int, **default −1**, which would write in front of the table; no
  bounds check) [S]. A later section with the same ID overwrites. Retail has 198 sections
  with unique IDs 0..246; ID 0 = `NOWEAPON` (range 16, damage 0) [C].
- record+0 = **section name** (the lookup key for FBI weapon1 etc.); +0x20 = `name` key (max 63) [C].
- `gamedata\weapons.tdf` is **never opened**: the only weapon path is `Weapons\*.tdf`, and no
  string pairs `gamedata` with `weapons`. Same for `gamedata\category.tdf` [S]. Note that
  `weapons\weapons.tdf` is a normal weapons file (it defines NOWEAPON, ARMCOMLASER, …).

### 3.2 Keys (0x42e440)
Scales use the doubles 0x4fd240 = 65536/30, 0x4fd248 = 65536/900, 0x4fd250 = 30,
0x4fd258 = 1/30, 0x4fd260 = π/180. All are `trunc(atof·k)` in double precision.

| Key | Off | Type | Scale | Default | |
|---|---|---|---|---|---|
| weaponvelocity | +0x68 | i32 | ×65536/30 (fx16 per tick) | 0 | 450 gives 983040 [C] |
| startvelocity | +0x6c | i32 | ×65536/30 | 0 | [C] |
| weaponacceleration | +0x70 | i32 | ×65536/900 (fx16/tick²) | 0 | 121 gives 8810 [C] |
| range | +0xdc | i32 | int | **32767** | [C] |
| coverage | +0xe0 | i32 | int | 0 | [C] |
| reloadtime | +0xe4 | u16 | ×30 → ticks | 0 | 2 gives 60 [C] |
| energypershot / metalpershot | +0xc0 / +0xc4 | float | dbl | 0 | [C] |
| areaofeffect | +0xd6 | i16 | int | 0 | [C] |
| edgeeffectiveness | +0xd8 | float | dbl | 0 | [C] |
| weapontimer | +0xe6 | u16 | ×30 | 0 | [C] |
| noautorange | flags b27 | | | | [C] |
| turnrate | +0xe8 | u16 | ×1/30 (per tick) | 0 | 30000 gives 1000 [C] |
| burst | +0xea | u16 | int | 0 | [C] |
| burstrate | +0xec | u16 | ×30 | 0 | [C] |
| sprayangle | +0xee | u16 | int | 0 | [C] |
| duration | +0xf0 | u16 | ×30 | 0 | [C] |
| randomdecay | +0xf2 | u16 | ×30 | 0 | [C] |
| smokedelay | +0xfa | u16 | ×30 | 0 | .1 gives 3 [C] |
| flighttime | +0xfc | u16 | ×30 | 0 | [C] |
| holdtime | +0xfe | u16 | ×30 | 0 | [C] |
| minbarrelangle | +0xc8 | float | degrees × π/180 → radians | **−11.25°** | [C] |
| firestarter, rendertype, color, color2 | +0x10b..+0x10e | u8 | int | 0 | [C] |
| accuracy, tolerance, pitchtolerance | +0x104/+0x106/+0x108 | u16 | int | 0 | [C] |
| shakemagnitude | +0xcc | i32 | int | 0 | [S] |
| shakeduration | +0xd0 | i32 | ×30 | 0 | [S] |
| model | +0x74 | ptr | `objects3d\<model>.3DO`, shared between weapons by name | null | [S] |
| explosiongaf + explosionart | +0x78 | anim | GAF file + sequence; **both** required | null | [S] |
| lavaexplosiongaf/art **or** waterexplosiongaf/art | +0x7c | anim | lava pair if the current map is a lava world (map+0xd44≠0), else the water pair | null | [S] |
| soundstart / soundhit / soundwater | +0xf4/+0xf6/+0xf8 | u16 | sound id (0x429470) | 0xffff | [C] |
| `[DAMAGE]` default | +0xd4 | **u16** (truncated, read back zero-extended) | int | 0 (also when there is no [DAMAGE]) | [C] |
| `[DAMAGE]` `<unitname>=n` | +0x64 | sorted map name→i32 (stricmp) | int | — | keys need not be loaded units [C] |

**flags (+0x111)**, value `atol&1` [C]: 0 lineofsight, 1 ballistic, 2 shellweapon,
3 beamweapon, 4 vlaunch, 5 meteor, 6 noradar, 7 paralyzer, 8 dropped, 9 startsmoke,
10 endsmoke, 11 soundtrigger, 12 guidance, 13 tracks, 14 unitsonly, 15 groundbounce,
16 waterweapon, 17 toairweapon, 18 smoketrail, 19 turret, 20 selfprop, 21 propeller,
22 noexplode, 23 burnblow, 24 twophase, 25 cruise, 26 commandfire, 27 noautorange,
28 stockpile, 29 targetable, 30 interceptor.

### 3.3 How retail classifies weapon behaviour
- **Aim/fire handler** (+0x60, set by 0x49e010 after parsing; the fire code at 0x49e1f2
  refuses to fire a weapon whose handler is null) [C]:
  `turret` → 0x49d580; else `vlaunch` → 0x49db70; else `lineofsight` or `selfprop` →
  0x49d9c0; else `dropped` → 0x49dd60; else **none** (e.g. pure explosion entries such as
  ATOMIC_BLAST that have no turret). The `ballistic` flag does not pick the handler; inside
  the turret handler it selects the arc solution (0x49d42e/0x49d5ac test bit1) [S].
- **Projectile motion per tick** (0x49b9ae…, first matching rule) [S, details U]:
  1. `selfprop`: powered. While the flight timer runs, speed += weaponacceleration up to
     weaponvelocity, steered when `guidance` (with `twophase`, only in phase ≥1). At expiry:
     `burnblow` detonates, otherwise gravity (G+0x14263) takes over, and `twophase`
     switches phase and re-arms the timer with `flighttime` (+0xfc). `propeller` spins the
     model (+0x400/tick).
  2. else `lineofsight`: straight line, no gravity; `beamweapon` also moves the beam
     tail and uses `duration`.
  3. else `ballistic`: gravity each tick plus a global vector G+0x37ecc [U]; with
     `weapontimer`≠0 it expires (burnblow → detonate).
  4. else `dropped`: gravity (same path).
  5. else `meteor`: straight line with model spin.
  `smoketrail` emits smoke every `smokedelay` ticks.
- **Damage to a unit** (0x499cd0): `map[targetUnitname]` if present, else the u16 default;
  × caller factor; × (100 + 6·min(kills/5, 5))/100 for a veteran owner; then the
  global cheat ×2 / ÷2 flags [S].

---

## 4. Other gamedata

Retail opens exactly these files: `gamedata\` + version, sidedata (×3), moveinfo, sound,
allsound, los, meteor, help, translate (the only refs to the `gamedata` string at 0x503360).
**It never opens category.tdf, gamedata\weapons.tdf, buildinfo.tdf or unitview.tdf** [S].

### 4.1 Features (`features\**\*.tdf`)
0x4222e0 loads every TDF under `features` (0x4bcb50, recursive [U]) and keeps them. A
feature is materialised **on demand by name** (0x4224b0 / 0x422e40): it uses the first
loaded file, in enumeration order, that has a top-level `[name]` (stricmp); if none has one
it reports `Record "%s" missing from feature files`. Records are 0x100 bytes in
G+0x1426f, count G+0x14253; lookup by name 0x422dd0 returns 0xffff for none [S].

| Key | Off | Type | Default | Notes |
|---|---|---|---|---|
| (section name) | +0x00 | char[128] | | |
| description | +0x80 | char[20] (max 19) | | |
| footprintx/z | +0x94/+0x96 | i16 | 0 | |
| height | +0xfa | u8 | 0 | |
| object | +0x98 | 3DO ptr | — | if present: 3D feature, flag bit0=0 |
| filename | +0xa8 | GAF `anims\<filename>.GAF` | — | if no object: bit0=1. A GAF already used by a feature with the same first 16 chars is reused (+0x98 text = "REUSE") |
| seqname, seqnameshad, seqnameburn, seqnameburnshad, seqnamedie, seqnamedieshad, seqnamereclamate, seqnamereclamateshad | +0xac..+0xc8 | sequence ptrs | null | empty string gives null |
| spreadchance, reproduce, reproducearea | +0xfb/+0xfc/+0xfd | u8 | 0 | |
| metal, energy | +0xf0/+0xec | float | 0 | int **& 0xffff** then float |
| damage | +0xea | i16 | 0 | |
| sparktime | +0xe8 | u16 | 0 | ×30 |
| burnweapon | +0xe4 | weapon ptr | null | by weapon section name |
| animating, animtrans, shadtrans, flamable, geothermal, blocking, reclaimable, autoreclaimable, indestructible, nodisplayinfo, nodrawundergray | flags +0xfe bits 1,2,3,4,5,6,7,8,9,10,11 | | **autoreclaimable default 1**, all others 0 (**blocking defaults to 0**) | |
| (name rule) | bit11 | | | sections named `DragonsTeeth`, `DragonsTeeth_Core`, `Fortification`, `Fortification_Core` always get bit 11 |
| featuredead, featurereclamate, featureburnt | +0xf4 / +0xf8 / +0xf6 | u16 feature index, loaded on demand | 0xffff | resolved by one pass over the feature table (0x422ea0); the count is re-read each step, so features loaded on demand by a chain are resolved too |

### 4.2 sidedata.tdf
- `[GENERAL] baseheight` (int, default 480) → G+0x147a7 (0x429ecf) [S].
- `[SIDE<i>]`, i = 0,1,… until the first one missing (0x429f2b, 0x431aaf), up to 5 records
  of 0x232 bytes at G+0x37f3d [S]:
  `name` (max 29) +0; `nameprefix` (**max 3 chars**) +0x1e; `commander` (max 31) +0x22;
  `font` → `fonts\<font>.FNT`; `energycolor`, `metalcolor` (int); `intgaf` → GAF with
  sequences PANELTOP/PANELSIDE/PANELBOT (first loader). Rectangles `{x1,y1,x2,y2}` (int)
  for LOGO, ENERGYBAR, ENERGYNUM, METALBAR, METALNUM, TOTALUNITS, TOTALTIME, ENERGY0,
  METAL0, ENERGYMAX, METALMAX, ENERGYPRODUCED/CONSUMED, METALPRODUCED/CONSUMED, LOGO2,
  UNITNAME, DAMAGEBAR, UNITMETALMAKE/USE, UNITENERGYMAKE/USE, MISSIONTEXT, UNITNAME2,
  DAMAGEBAR2, NAME, DESCRIPTION, RELOAD<n>. A missing rectangle reports "No [%s] in
  GAMEDATA/SIDEDATA.TDF for side:%s". **`fontgui` is never read** (no string).
- The side's `nameprefix` names the build-menu fallback GUI `<prefix>DL.gui` and the
  `<prefix>PREV`/`<prefix>NEXT` gadgets [S].
- `[CANBUILD]` see §5.

### 4.3 sound.tdf / allsound.tdf
- `sound.tdf` (0x42f580): every top-level section, by index, is a **sound class**
  (0x160 bytes, G+0x37e13, count G+0x37e17). The class name is the section name (max 63).
  There are 23 slots (table 0x5086fc): select, underattack, activate, deactivate, ok,
  arrived, cant, unitcomplete, build, repair, working, load, unload, cloak, uncloak,
  capture, count5, count4, count3, count2, count1, count0, canceldestruct. For each slot K
  the variants read are `K` then `K1`, `K2`, … up to the first missing number; each
  variant's text key is `<variant>text`. The unit's `soundcategory` indexes this table [S].
- `allsound.tdf` (0x42f7e0): every section `[NAME] { sound=<wav>; }` registers a named
  global sound (0x429470). It runs before sound.tdf [S].

### 4.4 category.tdf
Not loaded (§4). Categories exist only through the FBI `category` tokens (plus `ALL`
and the bad-target defaults `none`) [S].

---

## 5. Build lists and menus

### 5.1 Player build menu = GUI pages + download entries
- GUI pages: after the full parse (0x42d80f..0x42d8d2) retail probes
  `guis\<unitname>0.GUI` (sets flagsA bit31) and then `guis\<unitname><n>.GUI` for n=1,2,…
  up to the first missing n. **+0x22e = n_first_missing** (pages 1..n−1 exist) when page 1
  exists; otherwise 1 if page 0 exists, else 0 [S].
- `download\*.tdf` (0x42dcf0): files in VFS order. Each **top-level section** of a file
  (any name, by index) is one entry: `UNITMENU` (builder unitname; the entry is ignored if
  no such unit), `MENU` (int, u8), `BUTTON` (int, u8), `UNITNAME` (max 31). The per-file
  table holds 5 entries (0xbd bytes) with no bound check [S]. Afterwards, for every builder:
  **+0x22e = max(+0x22e, MENU)** of its entries (0x42df50). Any unit named by an entry
  gets `downloadable` forced on ("Hey! Somebody forgot to set downloadable=1") [S].
- Menu cycling (0x41be4c, 0x41bf62): the unit's menu state is a "build mode" bit plus a
  3-bit page p. NEXT goes ORDERS → p=1 → … → p=(+0x22e−1) → ORDERS. PREV runs the other
  way. A builder with +0x22e < 2 hides PREV/NEXT (0x41af37) [S].
- Showing page p (0x41b7a4 → 0x41ace0): load `<unitname><p>.GUI`; if missing, use
  `<sidenameprefix>DL.gui` (ARMDL/CORDL). Then **every download entry with builder ==
  this unit and MENU−1 == p** overwrites gadget **BUTTON+4** with that unit name (GUI
  gadgets 0..3 are header, font, PREV, NEXT; retail pages put 6 build buttons at gadgets
  4..9). So `MENU` = page number + 1 (the orders menu counts as menu 1), and `BUTTON` =
  0-based slot [S].
- Build buttons whose name is not a loaded unit are disabled (0x41ac90) [S, U on the
  exact gadget attribute].

### 5.2 `sidedata.tdf [CANBUILD] [<unitname>] canbuild1..N`
For every unit with `builder=1` (0x42d9a3): enter `[CANBUILD]`, then the subsection named
after the unitname, and read `canbuild<i>` for i=1.. up to the first missing i. Names that
don't resolve to a unit are skipped. The list is stored as u16 ids at +0x156 with count
+0x152 (copied from a 30-entry temporary list; there is no bound check while filling).
0x42be30 then appends every download entry whose builder is this unit while count ≤ 30.
Consumers include AI code (0x40814b, 0x4a26dd, …); this list is **not** what draws the
player menu [S; consumer roles U]. In retail the CANBUILD lists and the GUI pages cover
the same units.

---

## 6. Engine implementation spec

1. **TDF reader**:
   - Strip `//`→EOL and `/* */` globally first (inside values too).
   - Sections: `[name]` + `{`; keep the first duplicate, look up only direct children,
     case-insensitive.
   - Keys: everything up to `=`, value up to `;`, trim SP/TAB/CR/LF, case-insensitive,
     last duplicate wins. A root-level `}` ends the file.
   - `getInt` = atol (decimal, optional sign, stop at first non-digit, wrap mod 2^32;
     default only when absent).
   - `getDouble` = strtod.
   - `getFixed` = trunc(strtod·65536) in double, default raw.
   - `getStr(max)` truncates to max−1.
   - Do all scale math in IEEE double then truncate toward zero (53-bit x87 in retail).
2. **Unit registry**:
   - Parse every `units/*.fbi [UNITINFO]` with the §2.2 table: same types, widths and
     truncations (u8/i16 wraps, int-then-float costs, cloakcost as int).
   - Apply the Version and Copyright gates.
   - Sort valid units by unitname (case-insensitive); id = 1 + rank (0 = none); max 511
     units (512-bit category sets).
   - The movementclass, when found, overrides footprint/water depth/slopes; otherwise read
     them from the FBI. Clamp maxslope ≤ maxwaterslope.
   - Defaults: standing orders 2/2, selfdestructcountdown 5, bankscale and damagemodifier
     1.0, moverate1/2 = 2·maxvelocity, mincloakdistance 80 when cloakcost > 0, objectname =
     unitname, corpse none, soundcategory from sound.tdf with numeric fallback.
   - Ignore TEDClass, ThreeD, SteeringMode, UnitNumber, Scale, NoAutoFire,
     BadTargetCategory.
   - Yardmap: only for bmcode 0; skip unknown characters; repeat the last valid character
     to fill fx·fz; codes per §2.4.
3. **Weapons**:
   - Load every top-level section of every `weapons/*.tdf` into slot `ID` (0..255;
     reject −1 and out-of-range).
   - FBI weapon refs resolve by section name with the lowest ID first; unknown or empty
     → ID 0 (`NOWEAPON`).
   - "Has weapon" = any of weapon1–3 ≠ slot 0.
   - Scales per §3.2 (velocities ×65536/30, acceleration ×65536/900, times ×30 → ticks,
     weapon turnrate /30, minbarrelangle degrees → radians default −11.25°, range default
     32767).
   - Damage default stored as u16; per-unitname map (i32, case-insensitive).
   - Aim handler: turret > vlaunch > (lineofsight | selfprop) > dropped > none (cannot
     fire).
   - Projectile model per §3.3.
   - Do not load `gamedata/weapons.tdf` or `gamedata/category.tdf`.
4. **Move classes**:
   - `gamedata/moveinfo.tdf [CLASS0..31]`. Defaults: depth ±10000, slopes 255,
     badslope = maxslope/2, badwaterslope = maxwaterslope/2.
   - Apply the three clamps.
5. **Features**: on-demand by name across `features/**.tdf` (first file wins). Use the
   §4.1 keys: blocking default 0, autoreclaimable default 1, metal/energy as
   int&0xffff→float, sparktime ×30, the four name-forced bit-11 features, and
   dead/reclamate/burnt chains resolved after load.
6. **Sides**: `[SIDE0..]` until the first gap. Read name, 3-char nameprefix, commander,
   font, energycolor, metalcolor, intgaf, the UI rectangles and `[GENERAL] baseheight`
   (480).
7. **Build menus**:
   - Pages = `guis/<unit>1..N.gui` (consecutive). pageCount = N+1, raised to the max
     `MENU` of the download entries.
   - Cycle ORDERS → 1..pageCount−1 → ORDERS.
   - Page p shows `<unit><p>.gui`, else `<sideprefix>DL.gui`, then download entries with
     MENU−1 == p placed at slot BUTTON (gadget BUTTON+4).
   - Entries force `downloadable`.
   - Separately keep `sidedata [CANBUILD]` + download entries (cap 30) as the builder's
     canbuild list for AI and validation.
8. **Sounds**: `allsound.tdf` named globals; `sound.tdf` sections = classes indexed by
   order, 23 slots with `K`, `K1..Kn` variants and `<variant>text`.

### Confidence
| Section | Confidence |
|---|---|
| 1 TDF syntax | high (emulated) |
| 2.1 load pipeline | high; version-byte values and the archive gates are U |
| 2.2 FBI keys/offsets/defaults/flags | high (emulated on ARMCOM, ARMSY and synthetic FBIs) |
| 2.4 yardmap codes | parsing high (emulated); bit semantics medium/U |
| 2.5 moveinfo | high (emulated) |
| 3.1–3.2 weapon keys/scales | high (emulated on weapons.tdf and armaas) |
| 3.3 behaviour classes | handler selection high; projectile motion medium |
| 4.1 features | high static, not emulated |
| 4.2 sidedata, 4.3 sounds | high static, not emulated |
| 5 build menus | medium-high static; CANBUILD consumer roles U |
