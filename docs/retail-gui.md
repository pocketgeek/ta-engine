# Retail GUI / menu screen system (TotalA.exe v3.1c)

What this covers: how retail Total Annihilation loads and draws its `.GUI` gadget files, the
front-end screen graph, the in-game interface layout, and where the fonts, palettes and cursors
come from. It is a behaviour description for a clean-room reimplementation. No code or data
from the binary goes into the engine.

Sources. Addresses are VAs in `ta_install/TotalA.exe` (base 0x400000). Data paths are under
`assets/tadata/`. The tools are `tools/re/gui_*.py`:

- `gui_survey.py`: key and value census across all .GUI files.
- `gui_list.py`: one-line dump of each gadget.
- `gui_gaf.py`: GAF sequence and frame lister.
- `gui_dump.py`: linear disassembly.
- `gui_calls.py` and `gui_handlers.py`: map GUI-open calls to their handlers and strings.

Confidence tags:
- **[C]** confirmed from code.
- **[D]** confirmed from data.
- **[I]** inferred from usage or naming.
- **[U]** unconfirmed or guessed.

## 1. GUI file format

### 1.1 Loader

- **File location [C].** `0x4aa8f0 OpenGui(ctx, name, flags)` builds the path as `<guidir>\<name>`
  with the extension forced to `GUI`. The GUI directory is `guis`, set at init `0x4914c3` through
  `0x49fba0`. The anims directory is `anims` (`0x4914d9` → `0x49fbf0`) and the fonts directory
  is `fonts` (`0x4914ef`).
- **Parser [C].** The parser is `0x4aeac0`. The file is TDF syntax with sections `[GADGET0]` …
  `[GADGETn]`, iterated in order until one is missing (`GADGET%d` at `0x4ae6db`). Each gadget
  record is 0x15b bytes, and `totalgadgets` is informational. After loading, the name of
  GADGET0 is **overwritten with the GUI filename** (`0x4aac98`, 16 chars). This matters for art
  lookup (§1.4).
- **`[COMMON]` keys [C]** (`0x4ad350`). Field offsets inside the record are in brackets:

| key | stored as |
|---|---|
| `id` | byte [+0], the gadget type |
| `assoc` | byte [+1], the group id (radio groups, etc.) |
| `name` | 16 chars [+2] |
| `xpos`, `ypos`, `width`, `height` | s16 [+0x13, +0x15, +0x17, +0x19] |
| `attribs` | u32 [+0x1b] |
| `colorf`, `colorb` | u32 [+0x1f, +0x23], palette indices |
| `texturenumber` | byte [+0x27] |
| `fontnumber` | byte [+0x28] |
| `active` | byte [+0x29] |
| `commonattribs` | byte [+0x2a] |
| `help` | 128 chars [+0x33], status-line help text, run through the translation lookup `0x4c5740` |
| `gaffile` | bit 0 of [+0xb4]. No retail file uses it. |

- **Type ids and their per-type keys [C].** Type dispatch is at `0x4aeb5f`, jump table
  `0x4aed70`. The census counts come from `gui_survey.py`.

| id | type | per-type keys (read by) |
|---|---|---|
| 0 | container (screen header) | `totalgadgets`, `[VERSION]{major,minor,revision}`, `panel`, `crdefault`, `escdefault`, `defaultfocus` (`0x4ad890`). One file also has `crtdefault`, which is a typo the game never reads. |
| 1 | button | `status` [+0x138], `text` (128) [+0xb6], `quickkey`, `grayedout` (bit 0 of [+0x13c]), `stages` [+0x136] (`0x4adc70`). For `quickkey`, a single alphabetic char is used as-is (IsCharAlphaA); anything else is atoi'd (13 = Enter, 27 = Esc). |
| 2 | listbox | `itemheight` |
| 3 | textfield | `maxchars` (clamped to 128), `text` |
| 4 | scrollbar/slider | `range`, `thick`, `knobpos`, `knobsize` |
| 5 | label | `text` (128), `link` (16 chars: the name of a gadget the label belongs to, e.g. `link=LineOfSight`) |
| 6 | surface / hot area | `hotornot` (bit). Draw area used by code: map pictures, logos, radar, `HOTR` unit picture. |
| 7 | font | `filename` (32). At init it loads `fonts\<filename>.FNT` (`0x4a8f5f`). |
| 8 | picture | `filename` (32). Not present in retail files. |
| 9 | (none) | no keys |
| 10 | "nuttin" | `nuttin` int. Not present in retail files. |
| 12 | background-picture gadget | no extra keys. Draws the GAF sequence named after the gadget from the screen GAF or commongui (for example `IGOPT`, `MUSICRT`, `OPTBG`, `CONSOLE`). [C] init at `0x4a8aca`; [I] for the purpose. |

- **Values the parser ignores [D].** `0x4ae460..0x4aea8c` is a GUI writer (the editor saves
  `.GUI` files), so it has no runtime role. Values `-51` / `52685` (0xCDCD) are
  uninitialised-memory filler written by that editor. Ignore them.

### 1.2 Placement [C] (`0x4a81e0`, GUI init)

- **Centring.**
  - Open-flag 0x100: the container is centred on screen (`xpos=ypos=-1`).
  - Open-flag 0x1000: the container is centred in the area right of the 128-px sidebar
    (`x = (W-128-w)/2 + 128`).
  - A container that does not fit on screen is re-centred.
  - A container larger than the screen makes the open fail.
- **Container `ypos < 0`.** The container is placed at `ypos + screenH`, so it is anchored to
  the bottom edge (`0x4a84f2`). Examples:
  - TABMENU: `130,-33`
  - TALK2: `128,-164`
  - SHARE: `324,-429`
  - CONTROL: `437,-402`
- **Child gadget coordinates** are relative to the container. The init code adds the container
  x/y (`0x4aaba9`).
- **Open-flag 0x200: embed in a parent [C]** (`0x4aaa2f`). The GUI is merged into an
  already-open parent GUI and centred on the parent's gadget named `PANEL`. This is how the
  in-game `*RT.GUI` option pages appear inside `PREFS.GUI`.
- **Default buttons.** If `crdefault` is empty, the first button whose name starts with `OK`
  or `NEXT` becomes the Enter default (`0x4aacd2`).

### 1.3 Attribute meanings

- Button `attribs`, from code:
  - 0x80: checkbox art [C]
  - 0x100: use the last frame when grayed [C]
  - 0x1800: no pressed-frame offset [C]
- Button `attribs`, from usage [I]:

| value | meaning | where it is used |
|---|---|---|
| 2 | plain push button | |
| 1 | multi-stage cycling button | `stages`>0 |
| 0x12 (18) | radio button within its `assoc` group | e.g. Arm/Core, option tabs |
| 0x10 | tab toggle | sidebar ORDERS/BUILD |
| 0x40 | latched order button | stays down while that command mode is active: MOVE/ATTACK/PATROL… |
| 0x20 | build-picture button | 64×64 unit pics |
| 0x100 | multi-stage order button with a disabled frame | FIREORD/ONOFF/CLOAK |
| 0x402 (1026) | custom-art button with its own GAF sequence and no text | `Credits`, `AnyMsn`, `Side0/1` |

- Label attribs (0x11, 0x21, 0x2, …) look like alignment flags [U].

### 1.4 How art is found [C]

**Screen GAF.**
- For every GUI, the container loads `anims\<GUI file basename>.GAF` if it exists
  (`0x4a8537..0x4a85c3`). Examples: `MAINMENU.GAF`, `SKIRMISH.GAF`, `ARMCOM1.GAF`.
- `anims\commongui.gaf` is loaded once at init (`0x491543`). Localised copies exist:
  `commongui_french`, `commongui_german`.

**Container background** (`0x4a85c9..0x4a8658`). The `panel=` value names a GAF sequence. The
game looks it up:
1. in the screen GAF;
2. then in commongui (e.g. `ARMPAN`, `ARMPAN2`, `CORPAN`, `CORPAN2`, all 128×352);
3. otherwise it uses commongui `BackTile`, tiled with its frame offsets zeroed.

When the screen also has a full-screen bitmap loaded by `0x4288d0`, that bitmap is drawn
instead of the panel (`0x4a90f4..0x4a912c`). This applies to every front-end page.

**Per-gadget override** (`0x4a842e..0x4a84d8`). If `anims\<gadgetname>_gadget.GAF` exists, the
gadget uses that GAF and its sequence `<gadgetname>`. This is how the add-on units listed in
`download\*.tdf` get build pictures; there are 60+ files like `armaas_gadget.gaf`.

**Button art** (`0x4a8b4b..0x4a8df8`), in order:
1. Sequence `<gadget name>` in the screen GAF, then in commongui. For example, `ARMATTACK` comes
   from commongui and `ARMSOLAR` from `ARMCOM1.GAF`.
2. Otherwise:
   - `attribs & 0x80`: commongui `CHECKBOX`.
   - Else if `stages > 0`: commongui `stagebuttn<min(stages,4)>`. `stages==1` or `text=="Off|On"`
     uses `stagebuttn1` with `stages` forced to 2.
   - Else commongui `BUTTONS0`.
3. With `BUTTONS0`, the game picks the 4-frame group whose size best matches the gadget
   (minimum |dw|+|dh|), and **the gadget is resized to the frame size**. Groups by base frame:

   | base frame | size |
   |---|---|
   | 0 | 16×16 |
   | 4 | 321×20 |
   | 8 | 120×20 |
   | 12 | 96×20 |
   | 16 | 112×20 |
   | 20 | 80×20 |
   | 24 | 96×31 |

4. Multi-stage `text` is split on `|` into one label per stage (`0x4a8e07`).

**Button frame selection** (draw, `0x4a6062..0x4a616c`):

| state | non-stage button | stage button (`stages`=s, sequence frame count n) |
|---|---|---|
| normal | base+0 | frame = current stage (0..s-1) |
| pressed (`status`≠0) | base+status (base+1) | frame n-2 |
| grayed (`grayedout`) | base+min(status+2, n-1), i.e. base+2 (and +3 when grayed and pressed) | the stage frame, or frame n-1 if `attribs & 0x100` |

**Frame counts [D].**
- `BUTTONS0` / `CHECKBOX` groups have 4 frames: up, down, disabled, disabled-down.
- Unit build pics (`<GUI>.GAF` seq `<UNIT>`) and order buttons (`ARMMOVE`…) have 3 frames: up,
  down, disabled.
- `ARMFIREORD` / `ARMMOVEORD` have 6 frames (3 stages); `ARMONOFF` / `ARMCLOAK` have 5
  (2 stages); `ARMACTIVATE` has 4.

**Other commongui sequences [D].**
- `SLIDERS` (20 frames): scrollbar parts. Init is `0x4a8663`.
- `LISTBOX` (9).
- `TEXTINPUT` (3 frames: left, inner, right; 16×32).
- `LIGHTBAR`.
- `FRONTPAN`.
- `IGOPT`, `MUSICRT`, `SOUNDSRT`, `SPEEDSRT`, `VISUALSRT`: in-game option backgrounds.
- `igpatch`: the empty build-slot filler, `IGPATCH` buttons, grayed.

`anims\frontend.gaf` has another 9-slice set (`Back*`, `dia*`, `Button*x*`, `text16*`,
`slider*`), and nothing in the exe references it by name. It is likely editor-only [U].

### 1.5 Fonts and text

**`.FNT` format [D]:**
- `u16 height`, then `u16 unknown` (1 or 2), then `u16 offset[255]` for chars 1..255
  (0 means no glyph).
- A glyph is `u8 width` followed by `height×width` bits, packed MSB-first and continuous
  (`ceil(h*w/8)` bytes). Checked: hatt12 'A' w=13 h=14 takes 23 bytes.
- Fonts are 1-bpp and drawn in a palette colour.

**Front-end fonts.** These are GAF fonts `anims\hattfont12.gaf` and `hattfont11.gaf`: sequence
"Haettenschweiler", 256 frames, one per character. They are loaded at `0x49155e` / `0x491571`.
`fontnumber` 0 most likely selects hattfont12 and 1 hattfont11 [I]. The GAF glyphs carry their
own colours. For 1-bpp text, `colorf` / `colorb` are the foreground / background palette
indices (15 = white text on most pages; 0 on skirmish and sidebar pages) [I].

**In-game fonts.** A type-7 FONT gadget (`filename=armbutt` / `corbutt`) loads
`fonts\<name>.FNT` for that GUI's button text. `gamedata\sidedata.tdf` `fontgui=armbutt|corbutt`
and `font=console` set the console / info font [D].

**Other `.FNT` files [D]:**
- `guis\roman10.fnt`, `guis\roman12.fnt`
- `fonts\smlfont.fnt` (loaded at `0x42a367`)
- `fonts\console.fnt`, `hatt12.fnt`, `hatt14.fnt`, `comix.fnt`, `courier.fnt`, `mscript.fnt`

### 1.6 Palette, cursor and sounds

- **Palette [C].** Front-end screens use `palettes\guipal.pal` (`0x4264cf`). In-game screens use
  the game palette `palettes\palette.pal`.
- **Cursors [C].** Cursors come from `anims\cursors.gaf` (`0x429c90`). The sequences are:
  - `cursornormal`
  - `cursorselect`
  - `cursormove` (8 frames, animated)
  - `cursorattack`
  - `cursorpatrol`
  - `cursordefend`
  - `cursorrepair`
  - `cursorreclamate`
  - `cursorcapture`
  - `cursorload`
  - `cursorunload`
  - `cursorpickup`
  - `cursorteleport`
  - `cursorairstrike`
  - `cursorrevive`
  - `cursorfindsite`
  - `cursortoofar`
  - `cursorred`
  - `cursorgrn`
  - `cursorhourglass`
  - `cursorprotect`, `pathicon` (data only)

  `0x491c80(i)` sets cursor index i. 0x14 is the hourglass, set before every screen change.
- **GUI sounds [C/D].** Handlers call `0x47f1a0(name)`, and the name is resolved through
  `gamedata\allsound.tdf`:

  | allsound entry | sound |
  |---|---|
  | BIGBUTTON | butmain1 |
  | SMLBUTTON | butmain4 |
  | SMALLBUTTON / SLIDER | butscro2 |
  | PREVIOUS | button1 |
  | OPTIONS | butoptn |
  | SKIRMISH | butnskir |
  | MULTI | butnagr1 |
  | EXIT | button2 |
  | SIDESELECT / SIDESELECT2 | butnsid1 / butnsid3 |
  | ORDERSBUTTON / BUILDBUTTON | butnmbl1 |
  | NEXTBUILDMENU | butscro1 |
  | SETFIREORDERS | butnagr2 |
  | SETMOVEORDERS | butnmbl2 |
  | SPECIALORDERS / IMMEDIATEORDERS | button5 |
  | ADDBUILD / SUBBUILD | button9 / button7 |
  | PANEL | servsml6 |

- **Menu music.** See `docs/retail-music.md`. The front end plays the `BGM` sound
  (`allsound [BGM] sound=drone2`) through `0x47f210` at `0x426453`. There is no CD music in
  menus.

## 2. Front-end screen graph

**Screen API [C].** Each screen object holds a handler at `[obj+8]` and a per-frame callback
at `[obj+0x1c]`.
- `0x4ab060` opens a GUI as a modal popup.
- `0x4288d0(name,…)` loads a full-screen backdrop, `bitmaps\<name>.pcx`.
- `0x49fd60(obj,"NAME")` checks whether the named gadget was activated.

**Navigation [C].** Navigation sets the state byte `[ctx+0x2bc0]`. Known values: 2 = start
game, 3 = previous menu, 5 = single, 6 = multi, 8 = exit, 9 = credits, 0xA = new campaign,
0xB = skirmish, 0xE = "any mission", 0xF / 0x10 = campaign → briefing. The state machine
itself (`0x426e80`, jump table `0x42859c`) is only partly decoded [U].

All front-end GUIs are 640×480 (container 0,0,640,480) unless noted. The backdrop column is
`bitmaps\*.pcx`.

| Screen | GUI | backdrop | handler / evidence | Controls → effect |
|---|---|---|---|---|
| Main menu | MAINMENU.GUI | `frontendx` | `0x42641c`, hdlr `0x425d80` | SINGLE→state 5 (single menu); MULTI→Multiplayer CD check (`multiplay\maps`)→state 6; INTRO→intro movie (needs full screen); EXIT→state 8 (quit); `Credits` (anim button, `mainmenu.gaf` seq `Credits` 5 frames)→state 9; `DebugString` label shows the version "v3.1" (`0x426551`). defaultfocus=SINGLE |
| Single player | SINGLE.GUI | `singlebg` | `0x4777ae`, hdlr `0x4775a0` | NewCamp→state 0xA (Campaign CD check); Skirmish→0xB; LoadGame→LOADGAME.GUI; Options→options (§2.1); PrevMenu→state 3; AnyMsn (hidden, active=0; `single.gaf` `AnyMsn`)→0xE |
| New campaign | NEWGAME.GUI | `newcampaign4x` / `newcampaign4`; `playanygame4` in any-mission mode | `0x478266`, hdlr `0x477ab0` | Side0/Side1 (`frontend.gaf` `Side0/1` 160×160) and Arm/Core radio (assoc 35) pick the side; Campaign list + CampaignKnob (`camps\*.tdf`); Missions list (any-mission only); Difficulty `Easy|Medium|Hard`; Start→state 0x10/0xF (briefing); PrevMenu→3 |
| Mission briefing | MSNBRIEF.GUI | `mbrief%s` → `mbriefarm` / `mbriefcor` | `0x478ef1`, hdlr `0x478cb0` | Start→state 2 (launch); PrevMenu→3; SHUTUP `No Narration|Narration`; TextRegion/MOREBAR scrolling text; PLANET/PANORAMA/SOLARSYSTEM surfaces animate `anims\<world>brief.gaf` (`GreenPan`, `GreenRotate`, … per planet) |
| Skirmish setup | SKIRMISH.GUI | `Skirmsetup4x` | `0x47bbc3`, hdlr `0x47ae60` | Start→validation (terrain exists, player count ≤ map, not everyone in one ally group, ≥1 Player + ≥1 Computer)→state 2; PrevMenu→3; SelectMap→SELMAP.GUI; CommanderDeath `Game ends|Continues`; StartLocation `Fixed|Random`; Mapping `Unmapped|Mapped`; LineOfSight `Permanent|True|Circular`; Difficulty `Easy|Medium|Hard`; HELPTEXT shows the descriptive string for the hovered option. The player rows are drawn by code with `skirmish.gaf` (`skirmname` 112×20, `SIDEx` 44×20, `TEAMICONSx` / `ally icons` 38×20, `skirmmet` 46×20): name cycles Open / Player / Computer, plus Side, Color, Allies, Metal, Energy |
| Select map | SELMAP.GUI (494×420, centred over the parent) | `DSELECTMAP2` | `0x47ab3d` / `0x444f8d` | MAPNAMES list, SLIDER, MAPPIC preview, DESCRIPTION, SIZE; LOAD "Select Map"; PREVMENU Cancel |
| Load / Save game | LOADGAME.GUI (used for **both**) | `DLOADGAME2` (load), `DSAVEGAME2` (save) | `0x49307f`, `0x4931e2` | GAMES list, SLIDER, GAMENAME text, RADAR preview, GAMETYPE / SIDE / MISSION / DIFF / TIME labels, LOAD (OK), CANCEL, DELETE. SAVEGAME.GUI exists but nothing opens it [U] |
| Options (front end) | STARTOPT.GUI | `options4x` | `0x45cfc0` picks STARTOPT when not in game | SOUND / MUSIC / SPEEDS("INTERFACE") / VISUALS radio (assoc 30) open sub-pages; PREV=OK; CANCEL; RESTORE; UNDO |
| — Music | MUSIC.GUI | `optmusic4x` | `0x45d815` | NOTRAK `Off|On` (CD music), MUSICVOL slider, TRACKMODE `Play All|Random|Repeat|Custom`, CDPREV / CDSTOP / CDPLAY / CDNEXT (16×16 → BUTTONS0 16×16 group), TRACKNUM, TRACKTYPE `Building|Battle|Victory|Defeat|Unused` |
| — Sound | SOUNDS.GUI | `optsound4x` | `0x45de82` | FXVOL, SPEECH `Off|Medium|Full`, MODE `Off|Mono|3D`, TEST |
| — Visuals | VISUALS.GUI | `optvisual4x` | `0x45e667` | SHADING, ANTI, BSHADOWS `Off|On`; GAMMA; VIDSLDR + VIDVAL (screen size); SELVMODE.GUI popup |
| — Interface | SPEEDS.GUI | `optinterface4x` | `0x45eda4` | GAME speed, SCREEN scroll, TXTSCROL, MAXLINES sliders; LEFTCLICK `Left Click|Right Click`; UNITCHAT `Off|Medium|Full` |
| Multiplayer: provider | SELPROV.GUI | `selconnect2` | `0x444634` | DPLAY list of connection types, SELECT, SETTINGS, PREVMENU; SERVICEX |
| — TCP / Serial / Modem | TCP.GUI / SERIAL.GUI / MODEM.GUI | ? | `0x442204` / `0x442574` / `0x443139` | ADDRESS (TCP), OK, PREV |
| — Game list | SELGAME.GUI | `selectgame2x` | `0x443ccb` | GAMENAME / PLAYERS / MAPNAME / STATUS… lists, JOINGAME, STARTNEW, WATCH, UPDATE, NICKNAME, PASSWORD, PREVMENU |
| — New game | NEWMULTI.GUI | `createnew` | `0x441093` | GAMENAME / NICKNAME / PASSWORD; OK "Next"; CANCEL |
| — Battle room | LOUNGE2.GUI | `battleroom` | `0x449c91` | per-player rows (PLAYERx / SIDEx / ALLYx / READYx / RESx / LOGOx / PINGx / MEMx), MESSAGE + OUTPUT chat, MAP→VIEWMAP / SELMAP, COMMANDER `Continues|Game ends|Deathmatch`, MAPPING, LOSTYPE `True|Circular|Permanent`, CHEATING, FIXEDLOC `Random|Fixed`, WATCHING, GAMEOPEN, METAL / ENERGY / MAXUNITS sliders, RESTRICTIONS→RESTRICT2.GUI (`UnitRestrict5x`; save and load lists use SAVELIST / LOADLIST with `DSaveList` / `DLoadList`), START, PREVMENU; LOGOSEL.GUI picks a team logo (`logos.gaf`) |
| Mission end (campaign) | ENDMSN.GUI | `outcome1` / `outcome0` (one each for victory and defeat) | `0x41f0d4` | Start (next mission), LoadGame, SaveGame, MainMenu, Missions list, Difficulty |
| Multiplayer end | ENDMULTI.GUI | ? | `0x44b089` | OK, RESULT |
| Score reporting | REPORT.GUI | `scorebg` | `0x4435b5` | CHK0..15 checkboxes, OK |
| Popups | YESORNO, MSGBOX, CDCHECK, CONFIRM*, CHOICE3*, INPUT*, NOTEXIST*, FILEREQ* (*not shipped) | — | `0x4abb27`… | CHOICE1 / CHOICE2 / TITLE; OK |
| Credits | CREDITS.GUI (state 9) | ? | [U] | OK |

### Skirmish defaults [C]

Settings are read from the registry under "Total Annihilation" (`0x430780..0x430e00`,
reader `0x4b69d0`); the exact hive root is [U]. The values below apply when no key exists.

| key | default |
|---|---|
| `SkirmishCommanderDeath` | 1 → stage 0 "Game ends" |
| `SkirmishMapping` | 1 → "Unmapped" (terrain blacked out until explored) |
| `SkirmishLineOfSight` | 1 |
| `SkirmishLOSType` | 1 → "True" (LineOfSight=0 means Permanent; LOSType=1 means True; otherwise Circular; `0x47a656`) |
| `SkirmishLocation` | 1 → "Fixed" |
| `SkirmishDifficulty` | 1 → Medium [I] |
| `SkirmishMap` | the first map in the skirmish map list (`0x430b8b`) |
| `NumSkirmishPlayers` | 4 |

Per player `n`, under "Total Annihilation\Skirmish":

| key | default |
|---|---|
| `Player<n>Controller` | 0 (0 = Open, 1 = Player, 2 = Computer; `0x479855`) |
| `Player<n>Side` | n&1, so sides alternate Arm/Core |
| `Player<n>Color` | n |
| `Player<n>AllyGroup` | 5 (none) |
| `Player<n>Metal` | 1000 |
| `Player<n>Energy` | 1000 |

On first run, slot 0 is presumably forced to Player; this is [U]. Multiplayer equivalents:
`MultiCommanderDeath`, `MultiMapping`, `MultiLineOfSight`, `MultiLOSType`, all defaulting to 1.

## 3. In-game interface

**Screen layout [C].** TotalA.exe lays the in-game screen out the same way at every
resolution: a fixed-size frame, and the game view takes whatever is left.
- Game view: (128, 32)-(W-1, H-33), so the view is (W-128)×(H-64) (`0x4981c9..0x498237`:
  left 0x80, top 0x20, right W-1, bottom H-0x21). The view grows with the window.
- Side panel: the left 128 columns, full height.
- Top bar: rows 0..31 right of the side panel.
- Bottom bar: rows H-32..H-1 right of the side panel.

**Frame art [C]** (`0x429f61`, `0x467d70`). `gamedata\sidedata.tdf` `intgaf=ARMINT|CORINT`
selects `anims\<intgaf>.gaf`. The loader (`0x429f8b..0x429fd9`) keeps three sequences per side:
`PANELTOP` at game `+0x1481f`, `PANELBOT` at `+0x14833` and `PANELSIDE` at `+0x14847`.
- `PANELSIDE` (129×480) is drawn at (0,0) into the screen-sized OFFSCREEN surface after the
  surface is cleared to colour 0 (`0x467d7f..0x467ddf`). Nothing is drawn under it, so below
  row 480 the side panel is black.
- The top bar is `PANELTOP` (513×32) at (129,0). After it come copies of `PANELBOT` at
  x = 642, 1155, … until x ≥ W (`0x469050..0x46909d`; the table index is side + 5·(x > 129)).
- The bottom bar is copies of `PANELBOT` (513×33) at y = H-32 for x = 129, 642, … until
  x ≥ W. This loop is in both bottom-bar redraws (`0x46a8b5..0x46a929` and
  `0x46ad69..0x46adc9`).
- Frame offsets cancel: the blit draws at (x - xoff, y - yoff) (`0x4b804a..0x4b805a`), and
  callers add the offsets first.
- The `bitmaps\armgui*tile.pcx` files are not used [U].

**Top bar readouts [C]** (`0x4690ab..0x46960b`). `sidedata.tdf` gives the rectangles in
640×480 space as x1/y1/x2/y2 (parsed at `0x431c44..0x432307`). They do not move with W.

| region | drawn | address |
|---|---|---|
| LOGO (132,5)-(152,25) | the player's colour emblem: `textures\logos.gaf` frame = colour (game `+0x148db`), stretched to the rectangle | `0x4690ab` → `0x467c00` |
| ENERGYBAR (471,12)-(598,14), METALBAR (218,12)-(345,14) | filled x1 .. x1 + (x2-x1)·stored/storage over y1..y2, in `energycolor=208` / `metalcolor=224` (palette.pal) | `0x4690f3..0x46912e`, `0x469391..` |
| (bar) | the share level as a 2-pixel marker at x1 + (x2-x1)·share/storage, when share > 0 and < stored | `0x469133..0x4691b2` |
| ENERGYNUM (529,18), METALNUM (278,18) | stored, `"%d"`, left at (x1,y1) | `0x4691bd..0x4691f3` |
| ENERGY0 (468,1), METAL0 (215,1) | `"0"` | `0x4691f8..0x469212` |
| ENERGYMAX (595,1), METALMAX (341,1) | storage, `"%d"`, right-aligned to x1 | `0x469217..0x46926b` |
| ENERGYPRODUCED (609,5) / CONSUMED (609,17) | `"%d"`, or `"%dK"` (in thousands) when the value is past ±99999 | `0x469270..0x46938c` |
| METALPRODUCED (358,5) / CONSUMED (358,17) | `"%.1f"` | `0x469578..0x46960b` |

- The text uses the side's `font=console` FNT (side record `+0x22e`).
- Produced and consumed take entries `+0xa` and `+0xc` of the interface colour table at game
  `+0xdcb`. Which palette slots those are has not been traced [U]; the engine draws green and
  red.
- TOTALUNITS and TOTALTIME are parsed but this routine does not draw them.

**Bottom bar readouts [C]** (`0x46a860..0x46b789`). Every footer rectangle moves down by
H - `baseheight` (`0x46a86f..0x46a87e`). `baseheight` is a `sidedata.tdf` key, default 480
(`0x429efc`).
- Selected unit, own or allied:
  - LOGO2 (132,455)-(152,475): the owner's emblem (`0x46b105`).
  - UNITNAME (245,452): the unit's name, centred on x1, in palette colour 0x53
    (`0x46afcf..0x46b00e`).
  - DAMAGEBAR (200,463)-(290,465): health, x1 .. x1 + (x2-x1)·hp/maxdamage in colour-table
    `+0xa`; the rest of the bar in `+0x4` (`0x46b04c..0x46b0f0`).
  - Under the bar at y2 + 2: `"%d kill(s)"`, with `" - Veteran"` past 4 kills
    (`0x46b2b8..0x46b3ae`).
- Shown player's units only, the make and use figures (`0x46b12e..0x46b2b3`):
  - UNITMETALMAKE (350,458): `"+%.1f"`.
  - UNITENERGYMAKE (400,458): `"+%.0f"`.
  - UNITMETALUSE (350,468): `"-%.1f"`.
  - UNITENERGYUSE (400,468): `"-%.0f"`.
- The unit under the pointer: UNITNAME2 (555,452) and DAMAGEBAR2 (510,463)-(600,465)
  (`0x46b492..0x46b55f`).
- NAME, DESCRIPTION, MISSIONTEXT and RELOAD1-3 are parsed. Their draw paths (`0x46b5cf..`,
  `0x46b70b` "Unidentified object") are not mapped yet [U].

**Minimap [C]** (`0x466780`). The map is fitted into a 126×126 box at the screen's top-left
corner:
- The longer side is 126 pixels, the other is 126 × its share of the longer one, and the
  short axis is centred (`0x46679f..0x4667fa`).
- A click maps (mouse - rect origin) × map size / rect size (`0x498d00`).
- The sidebar GUIs all sit at (0,128), sized 128×352.

**Engine layout.** `gameview_tahud.cpp` follows the rules above.
- `viewX0()` / `viewY0()` = (128, 32) × UI SCALE. The world is drawn through an SDL viewport
  there. `mouseX_` / `mouseY_` are view coordinates and `screenMouseX_` / `screenMouseY_` are
  window coordinates.
- UI SCALE (default 1.0) multiplies the frame. At 1.0 the side panel is 128 pixels wide at any
  resolution, as in retail.
- Engine extras sit inside the view: the stats column (top right), IDLE BUILDERS, the clock
  and scoreboard column (top left), chat and the replay bar.

**Sidebar GUIs [C/D]** (`0x41aa00..0x41b9c0`). Side prefix = sidedata `nameprefix`
(ARM / COR).

| situation | GUI file |
|---|---|
| nothing selected | `<SIDE>MAIN2.GUI` (`0x497ba5`): KILLS, LOSSES, TOTALUNITS labels, panel `ARMPAN2`. `ARMMAIN.GUI` (with OPTIONS / SHARE / ALLIES) exists but nothing references it [U] |
| unit with a build list | `guis\<UNIT><page>.GUI`, pages 1..N (`%s%d.GUI` at `0x41b287`) |
| orders page | `<SIDE>GEN.GUI` (`0x41b13c`) |
| other | `<SIDE>DL.GUI` (orders only; `%sDL`, `0x41ad8f`) [I]; `BUILDER.GUI` is a fallback (not shipped) |

**Build page layout [D]:**
- ARMORDERS at (3,4) 59×19 and ARMBUILD at (65,4) 59×19 are tab toggles (attr 0x10, keys o / b).
- Six 64×64 build slots, numbered 0..5: (0,27) (64,27) (0,91) (64,91) (0,155) (64,155).
- ARMPREV (8,222) and ARMNEXT (72,222), both 44×16 (`%sPREV` / `%sNEXT`, sound NEXTBUILDMENU).
- Movement orders along the bottom.

**Build buttons [D].** Each build button is named after the unit; the name is the unit to
build.

**Build button clicks [C]** (handler `0x41aa00`). The gadget's name gives the unit type
(`0x41aa39` → `0x488b10`).
- If the type's `bmcode` byte (`+0x22f`, stored at `0x42c422`, default 0) is 0, the click
  arms the placement: state 0xE with the type at `+0x2cc4`, sound `addbuild`
  (`0x41ab7f..0x41ab9c`). Either button does this.
- Anything else is queued in the unit whose menu is shown, through `0x419b00`
  (`0x41abd9..0x41ac3c`). That covers a mobile unit on a factory's page and the stockpile
  button MAKENUKE / MAKEANTI, which names no unit.
  - Left click: +1. Right click: -1. Shift: ±5.
  - `0x419b00` sends `MOBILEBUILD` or `BUILDINGBUILD` (or `BUILDWEAPON` for the stockpile)
    and plays `addbuild` / `subbuild`. Its art is seq `<UNIT>` in `anims\<UNIT><page>.gaf`, 3 frames. `IGPATCH` (grayed) fills
empty slots.

**Add-on units [C/D].** Each `download\*.tdf` holds `[MENUENTRYn]{UNITMENU=builder;
MENU=page(1-based); BUTTON=slot 0..5; UNITNAME=unit}` (parsed at `0x42dd6f`). A unit listed
there goes into that page and slot. Pages beyond the shipped `.GUI` files are synthesised.
The art is `anims\<UNIT>_gadget.gaf`. The game warns "Somebody forgot to set downloadable=1"
when that flag is missing.

**Order buttons** (`ARMGEN.GUI` positions, `commongui.gaf` art, quickkey in brackets):

| button | position | key | notes |
|---|---|---|---|
| ARMFIREORD | (6,35) 113×20 | f | 3 stages |
| ARMMOVEORD | (6,59) | v | 3 stages |
| ARMONOFF | (6,92) | x | |
| ARMCLOAK | (6,116) | k | |
| ARMRECLAIM | (5,172) | e | |
| ARMUNLOAD | (64,172) | u | |
| ARMREPAIR | (5,207) | r | |
| ARMCAPTURE | (64,207) | c | |
| ARMMOVE | (5,247) | m | |
| ARMSTOP | (64,247) | s | |
| ARMDEFEND (guard) | (5,282) | g | |
| ARMPATROL | (64,282) | p | |
| ARMATTACK | (5,317) | a | |
| ARMBLAST (D-gun) | (64,317) | d | shares this slot with ARMLOAD (l) |

- Small order buttons are 55×31 with 54×30 art, latched (attr 0x40).
- Buttons that do not apply are hidden or grayed by the code (`ONOFF` is checked at
  `0x41b011`).
- Other commongui sequences: ARMACTIVATE, ARMSPECIAL. The CORE versions are named `COR*`.

**Bottom-anchored popups:**

| GUI | geometry | contents / opened from |
|---|---|---|
| TABMENU.GUI | 510×33 at (130, H-33) | OPTIONS "Options Menu", SHARE, ALLIES, CONTROL, CANCEL; handler `0x494740` |
| TALK2.GUI | (128, H-164) | chat: TALK text, SENDTYPE `All|Allies|Enemies|Custom`, PLAYERx toggles |
| SHARE.GUI | (324, H-429) | METAL / ENERGY sliders, PLYRLIST, SHARUNIT, OK / CANCEL |
| CONTROL.GUI | (437, H-402) | PLAYERx reject toggles, WATCHING |
| ALLIES.GUI | (240,108) | ALLYx, TEAMICONSx (`allies.gaf`), LOGOx, VICTORY "Allied Victory" |
| SCORE.GUI | 511×133 at (129,347) | |
| UNITINFOx.GUI | 325×190 at (203,105) | HOTR = `unitpics\<UNIT>.pcx`, NAME, cost / statistics text, DONE |

**In-game menu [C]:**
- `ARMOPT.GUI` sits at (0,128), 128×352, background `armopt.gaf` `OPTBG` (`0x460cd1`). Buttons:
  LOADGAME, SAVEGAME, PREFS "Options", MISSION "Briefing", HELP, EXIT, OK "Resume".
- PREFS opens `PREFS.GUI` (0,126), background seq `IGOPT`, with tabs SOUND / MUSIC / SPEEDS /
  VISUALS plus PREV (OK) and CANCEL. It embeds `SOUNDSRT` / `MUSICRT` / `SPEEDSRT` /
  `VISUALRT.GUI` at (128,128), 150×352, background seqs `*RT` from commongui. Each of these
  pages also has RESTORE and UNDO.
- EXIT opens `EXITMENU.GUI` (279,117): MAINMENU, EXITGAME, RESTART→`RESTART.GUI`
  (`drestart`; Difficulty), CANCEL.
- Briefing opens `BRIEFING.GUI` (`igmbrief`); HELP opens `HELP.GUI` (`dhelp`, Page 1-3);
  `GAMEOPTIONS.GUI` uses `GameSettings`.
- Load and save open LOADGAME.GUI as in §2.
- Opening the menu pauses the CD (see retail-music.md).

## 4. Fonts, palette and cursor (summary)

See §1.5 and §1.6. In short:
- Menus use `guipal.pal`, the `hattfont11` / `12` GAF fonts, and commongui / screen GAF art.
- In-game uses `palette.pal`, `armbutt` / `corbutt` / `console` FNT fonts, and the
  `ARMINT` / `CORINT` frame.
- Cursors come from `cursors.gaf`.

## Engine implementation spec (priority order)

**P0: GUI runtime (needed by everything)**
1. A TDF `.GUI` loader for types 0-7 and 12, with the field semantics and limits from §1.1.
   Ignore -51 / 0xCDCD filler. Rename the container to the file basename.
2. Placement per §1.2: negative container y anchors to the bottom, -1 / -2 centring, child
   coordinates relative to the container, and PANEL embedding.
3. The art resolver from §1.4: screen GAF `anims\<gui>.gaf`, `anims\<name>_gadget.gaf`, then
   commongui fallbacks (CHECKBOX / stagebuttnN / BUTTONS0 best-size group with resize). Frame
   selection per the table. `|`-split stage labels. Panel / BackTile / backdrop-PCX background.
4. Widgets:
   - button: push, stage-cycle, radio by `assoc`, latch, tab;
   - listbox with SLIDERS scrollbar;
   - textfield (TEXTINPUT 3-slice, `maxchars`);
   - slider (`range` / `knobsize`);
   - label (`link` → hover help);
   - surface: a hook for code-drawn content.
5. Keyboard: `quickkey`, `crdefault` (Enter), `escdefault` (Esc), `defaultfocus`.
6. Fonts: the FNT and GAF-font renderers. `guipal` for menus.
7. GUI sounds through allsound.tdf, the BGM drone, and the cursors.gaf cursor set.

**P1: play skirmish**
1. MAINMENU (`frontendx`): SINGLE / MULTI / EXIT.
2. SINGLE (`singlebg`).
3. SKIRMISH (`Skirmsetup4x`): code-drawn player rows with `skirmish.gaf`; option buttons;
   defaults from §2; the validation messages; persist the settings.
4. SELMAP popup (`DSELECTMAP2`; map list + minimap preview).
5. In-game frame (INT gaf, top and bottom bars per sidedata).
6. `<SIDE>MAIN2` / `<SIDE>GEN` / `<UNIT>n.GUI` sidebar: ORDERS / BUILD tabs, PREV / NEXT
   paging, download MENUENTRY merging with `_gadget` art, IGPATCH fillers.
7. Order buttons and their hotkeys.
8. TABMENU.
9. ARMOPT → EXITMENU (Exit to Menu / Exit Game).

**P2: campaign**
1. NEWGAME (`newcampaign4x`; side, campaign list, difficulty).
2. MSNBRIEF (`mbriefarm` / `mbriefcor`, planet GAF animations, scrolling text, narration toggle).
3. ENDMSN (`outcome0` / `outcome1`).
4. RESTART.
5. BRIEFING (`igmbrief`).
6. LOADGAME (load and save, with the two backdrops).

**P3: multiplayer**
1. SELPROV → TCP → SELGAME → NEWMULTI → LOUNGE2 (`battleroom`; plus LOGOSEL, VIEWMAP,
   RESTRICT2 / SAVELIST / LOADLIST).
2. In game: TALK2, SHARE, ALLIES, CONTROL, SCORE.
3. Afterwards: ENDMULTI and REPORT.

**P4: polish**
1. STARTOPT + MUSIC / SOUNDS / VISUALS / SPEEDS (+SELVMODE).
2. The in-game PREFS with the `*RT` pages.
3. HELP, UNITINFOx, credits / intro, YESORNO / MSGBOX / CDCHECK.

**Open questions [U]:**
- Whether the retail front end switches to 640×480 or centres the screens. The GUIs are 640×480
  either way, so centre or scale.
- The exact label alignment bits.
- The `fontnumber` → font mapping.
- Front-end states 0xE / 0xF / 0x10 and the credits / intro paths.
- Who opens TABMENU (a key or the bottom bar).
- Whether `ARMMAIN.GUI` / `SAVEGAME.GUI` / `ANYMSN.GUI` are still used.
