# Model rendering: root cause and correction plan

Status: **RESOLVED (2026-09-05, commit 7d9d71e).** Final fix = negate BOTH piece X and Y rotations in
`scriptRot`, and all flyers face `−heading` (flyHalfTurn deleted). Defect C (full-body walk) also landed.
Scope: `src/client/main.cpp` (render-only). Sim state, heading semantics, and the lockstep hash are untouched.

## FINAL resolution (2026-09-05, 7d9d71e) — read this first

The real defect was **the piece X-rotation sign, not just Y.** `scriptRot` now negates BOTH `rot[0]` (pitch)
and `rot[1]` (yaw): the models are authored front=−z / right=−x (a mirrored basis), so scripted X- AND Y-turns
play mirrored unless negated. Negating X fixed two user-visible symptoms at once:
- **Walkers' legs swung backward** ("feet on backwards / walk running backwards") — `walk_legs` is entirely
  X-axis leg swings.
- **Flyers flew supine** ("back to the ground") — the `fly` script pitches the body with X-axis TURNs on
  hips/torso (positive ~35–57°), and `+X` rolled them belly-up.

With X negated the flyer body sits upright, and then **every flyer flies forward at plain `−heading`** — so
`flyHalfTurn` is gone (all movers, flyers included, use `−heading`; matches the ICD "no flyer facing branch",
root matrix 0x4ee620 identical for all units). The earlier per-flyer π−heading split (`flyHalfTurnOf`) only
existed to make the *supine* flyers look forward; it was masking the X bug.

**Sequence of wrong turns (so nobody repeats them):** c0ab21a deleted flyHalfTurn (all −h) → flyers flew
backward because they were still supine → reverted (fc3a04b) → then found the X-sign bug → 7d9d71e negates X
AND deletes flyHalfTurn together. The two are coupled: −h is only correct once X is negated.

**Method that finally worked** (by-eye screenshots failed repeatedly): (1) for facing, tint front pieces red /
back pieces blue in `collect()`, render moving east, compare red vs blue centroid-x automatically; (2) for the
X-sign, the *user* judged it live — a runtime `TA_NEGX` toggle let them confirm the walk stayed correct and
the flyer went belly-down. Verified across nine flyers of all five factions. The rest-pose `modeltool obj`
oracle does NOT predict airborne facing (the fly pose re-orients the body).

## (superseded) Correction attempt: flyHalfTurn was RIGHT; deleting it was WRONG

Commit `c0ab21a` deleted `flyHalfTurn` (every flyer → `−heading`), justified by a rest-pose static oracle
(`modeltool obj`: head/breast at −z, tail/hair at +z for all five Zhon flyers) plus a movement-geometry
derivation. **Both were wrong for airborne flyers**, and the user immediately saw zonhunt (Thirsha) fly
backward/feet-first in-game. Root of the error: the static oracle measures the REST pose, but the `fly`
animation re-orients the body — the adversarial review flagged exactly this ("the fly pose applies ~569
x-turns; the standing oracle is weakened"), and it was ignored.

**The decisive method that finally worked** (after screenshots-by-eye repeatedly misled, because a valkyrie's
head vs feet are unreadable at game zoom): render the flyer moving east with FRONT pieces tinted bright red
and BACK/feet pieces tinted blue (`TA_TINT` throwaway in `collect()`), then compute the red-centroid-x vs
blue-centroid-x automatically. front_x > back_x ⇒ head leads the movement ⇒ forward. Measured per flyer,
both facings:

| flyer | −heading | π−heading | correct | flyHalfTurnOf says |
|---|---|---|---|---|
| zonhunt | BACKWARD | FORWARD | **π−h** | π (TRUE) ✓ |
| zondrake | FORWARD | BACKWARD | **−h** | − (FALSE) ✓ |
| zongod | BACKWARD | FORWARD | **π−h** | π (TRUE) ✓ |
| zonharp | BACKWARD | FORWARD | **π−h** | π (TRUE) ✓ |
| zonroc | (head occluded by cargo basket) | — | — | − (FALSE) |

`flyHalfTurnOf` matched the empirical answer on all four measurable flyers. So it was reverted (`git revert
c0ab21a`) and kept. **Lesson: for flyer facing, the rest-pose front/back is NOT the airborne front/back;
validate against the actual `fly`-pose render (the red/blue centroid method), not static geometry, and never
against a single by-eye screenshot.** The `TA_NO_HALFTURN` dev gate remains (default off = flyHalfTurn on).

## Progress (2026-09-04)

- **Defect A — piece-yaw sign — FIXED.** `scriptRot()` negates the composed piece Y before `Xform::then()`,
  matching retail's `fchs` at 0x4eea98. Confirmed decisively by the *consistency* argument (not eyeballing):
  our root-heading yaw (main.cpp ~4570) and the piece `Ry` in `then()` reduce to the identical matrix
  `[cy 0 sy; 0 1 0; -sy 0 cy]`; the root is `Ry(-u.heading)` (via `facing = -u.heading`) and is verified
  correct (statics face right); retail applies the same `fchs` negation to root AND piece; so the piece must
  carry it too. The model-viewer attack-swing montage corroborated (the fix produces a coherent overhead→down
  arc vs the incoherent current one). Determinism unchanged (mpai `3eef6e0f`, render-only).
- **Defect B — flyHalfTurn — DEFERRED, dev-gated.** `TA_NO_HALFTURN=1` forces `flyHalfTurn=false` (default
  off = current behavior), left in as a toggle. Not deleted: the in-game flyer-facing A/B was visually
  ambiguous at readable zoom (zonhunt is a harpy with hair+wings+humanoid body; zondrake read backward under
  BOTH settings, which the plan did not predict), and the adversarial review specifically required a numeric
  measurement before deletion. The numeric front/back **origin** dump is not sensitive to the fix (a piece's
  yaw rotates its children, not its own origin). Next: a probe that composes a *child* tip (beak/tail-tip)
  world position relative to the movement vector at cruise, per flyer, before touching `flyHalfTurn`.
- **Defect C — full-body walk — FIXED.** The anim loop preferred legs-only `walk_legs` over the full-body
  `walk` (hip/torso/head/arm sway); the `||` ordering was incidental. Now prefers `walk` (fallback
  `walk_legs`), paired with full-body `restore_x` on stop, at both live and sprite-bake sites. Verified
  coherent across arasword/araknigh/versword/zonorc/arapal in the model viewer; determinism unchanged. With
  Defect A in place the sway reads correctly.

---
## Original plan


This document supersedes the "axis reflection in the projection" working hypothesis and the memory notes
`zhon-models-mirrored-facing` and `flyer-billboard-facing-coupling`, both of which rest on a misreading of
the art. Evidence: KINGDOMS.icd disassembly (instruction-verified), raw 3DO piece data (dumped from the
shipped models), COB bytecode (disassembled with `cobtool`), and pixel-level re-examination of symptom
screenshots. An adversarial review then re-verified the claims against the scripts the engine *actually
executes* and amended the plan; those amendments are folded in below.

---

## 1. ROOT CAUSE

### 1.1 The one hard fact the art itself supplies

TAK models are authored **front = −z, right = −x, up = +y** in raw 3DO coordinates. Proven by piece *data*,
which cannot be misread the way rendered pixels can:

| Model | Piece evidence (raw 3DO, world units) |
|---|---|
| arasword | `FootL/FootR` toes extend to z = **−6.5** (heels +1.8); `Sword` under `HandR` at x = **−10.08**; `Shield` under `HandL` at x = **+10.08**; `WakeL/WakeR` water-wake emitters at z = **+3.0..+11.7** (behind) |
| araking | `Cape1..Cape4` hang at z = **+3.2..+6.3** (a cape is on the back); toes z = **−7.6**; `EmitWpn` at blade tip z = **−41.5** |
| tardemon | `tail1/tail2` extend to z = **+15.2/+11.4**; `lhoof/rhoof` toes z = **−11.0..0** |
| zonhunt | `hair1..hair8` trail at z up to **+16.8**; `winganchor_L/R` + `wing1` attach at z = **+6.5** (wings root on the back); `breast_L/R` at z = **−4.4..+1.9** (front) |

(Reproduce by walking the 3DO tree printing FromParent offsets and vertex bboxes.)

This matches TA-classic: Spring negates z at 3DO import precisely because raw TA data is front = −z in a
right-handed reading; the TA Design Guild's "front is +z" describes the authoring tool's display convention,
not the bytes. **The Zhon models are authored exactly like everyone else's — the "ZON models mirrored E/W"
memory is false.**

### 1.2 What is actually correct today (and was misdiagnosed)

- **Root facing `−heading` is not a hack — it is retail.** KINGDOMS.icd builds the unit matrix with
  `Ry(−heading)` (`fchs` at 0x4ee6a9) and our `facing = −u.heading` reproduces it. With front = −z,
  `Ry(−h)` puts face, toes, and lowered blade toward the movement direction `(sin h, cos h)` — exactly what
  the original stationary-sword test observed. The test's *reading* was right; its *interpretation*
  ("models are E/W-inverted, −heading compensates") was wrong.
- **The planar projection matches retail sign-for-sign.** Both compose to `sx = x·cos h − z·sin h`,
  `sy_down ∝ −(x·sin h + z·cos h)`, planar det −1, chirality anchored by the `HandR`/`HandL` piece names and
  the south-facing card. Only the *constants* differ (our tilt-trig vs retail's ¼-shear oblique) — a style
  difference, not a bug. The **depth channel differs** (ours `ct·rz − st·wy`; retail `v.y + 25` with unknown
  compare direction) — our painter's sort is self-consistent at every facing, and "visible-surface results
  agree with retail" has NOT been verified; it just has no observed counterexample.
- **The stationary cards are correct today.** Re-examined at pixel level: the "backwards head" reading came
  from mistaking the gold chainmail **aventail on the BACK of the helmet** for the face. The face side is
  the dark surface (visible facing south, where the face must be). Every "head reads backward" / "wrong body
  side" observation in the symptom report cascades from this one texture misread. The cape/tail cards re-read
  as correct under the front = −z key.

### 1.3 Why the reflection hypothesis was wrong (and why every past mirror attempt failed)

The pipeline analysis computed det = −1 for our model→screen basis and concluded "a reflection a rotation
can't fix". The computation was right; the reference frame was wrong: BOTH label systems (world
`x=east, y=up, z=south` and model `right=−x, up=+y, front=−z`) carry one mirror relative to physical space,
so a correct renderer's label-matrix determinant here **must** be −1. The two mirrors cancel semantically —
there is no perceived reflection, and re-examination confirms the E/W cards show the correct body side.

This also finally explains the hardest historical observation — that every combination of
{h, h+π, −h, π−h} × X-mirror failed: an X-mirror conjugation fixes the Ry sign but flips Rz and mirrors
static geometry. No root-level rotation or reflection can ever fix a **per-channel sign error in the piece
composition** — which is what was actually wrong.

### 1.4 The real defects (three)

**Defect A — COB piece Y-rotation sign is inverted relative to retail (proven).**
Retail composes every piece as `Ry(−y_angle) · Rx(+x_angle) · Rz(+z_angle)` — the y negation is a literal
`fchs` at 0x4eea98 in the piece-matrix call site (same negation on the root heading at 0x4ee6a9 and in the
vector-rotate at 0x535d50: three independent confirmations). Our `Xform::then()` builds
`Ry(+rot[1])·Rx(+rot[0])·Rz(+rot[2])` — y not negated. Consequence: **every scripted y-axis TURN/SPIN plays
mirrored.** The animations are dense yaw streams (arasword `walk`: 57 y-turns on Hip/Torso/Head/arms;
zonhunt `fly`: 386 y-turns in the compound wing stroke) — mirroring one axis of three garbles them.
Independent behavioral corroboration (found in review): arasword `attack1/2` carry large negative y-turns
(≈ −85°); under retail `Ry(−a)` the windup rotates toward the **sword** side (−x) — the anatomically correct
forehand. Under our `Ry(+a)` it lands on the shield side.

**Defect B — `flyHalfTurn` renders half the Zhon air force flying backward (strong, gated on a probe).**
Retail has **no** flyer-specific facing (the root matrix call 0x4ee620 is identical for all units). Our
`flyHalfTurn` machinery gives zongod/zonharp/zonhunt `π−heading` — 180° from retail — so those models fly
tail-first: trailing hair (+z) and back-rooted wings *lead*, which reads as "hair/cape/head on backwards".
The classifier is numerology (keyed on the sign of the first vertical hip MOVE in `fly` — a crouch offset
with no relation to any turn); it only produced a per-model split because the y-mirrored fly poses (Defect A)
were too garbled to judge by eye. Review caveat: this overrides the symptom report's empirical reading of the
same pixels, and the fly pose's ~569 x-turns of body pitch weaken the standing-pose oracle — so the deletion
is **gated on a numeric probe** (below), not on document confidence. (Checked: no scripted 180° body yaw
exists in `fly` — largest y-turn constant is ≈ 85° — so no alternative explanation survives.)

**Defect C — wrong walk script: we play legs-only, retail plays the full body (found in review; proven).**
Our engine prefers `walk_legs` over `walk`. Disassembly of the shipped bytecode: arasword `walk_legs` = 70
TURN_NOWs, **all x-axis** — zero y-turns. The full `walk` script (148 x / 57 y / 51 z — the torso/head/arm
counter-sway) is what retail runs: the ICD's known script-name strings include Create/Killed/AimWeapon/…
but NOT "walk", because retail drives walking from the script side — `MoveWatcher` polls
`GET_UNIT_VALUE 29` into a static and `walk` self-gates on it at entry. Our legs-only selection leaves the
torso/head/arms **dead while marching** — a big share of "the walk looks janky" — and it means Defect A alone
provably does NOT change arasword's walk (its executed script has no y-turns to fix).

### 1.5 Symptom-by-symptom accounting

| Reported symptom | Explanation |
|---|---|
| Weapon points along movement only at `−heading` | Correct behavior, correctly measured: `−heading` IS retail. Only the inference ("models mirrored") was wrong. |
| Capes/heads read backwards | (1) zongod/zonharp/zonhunt genuinely fly backward (Defect B); (2) mirrored head/torso yaw during animation (Defect A); (3) the aventail-as-face misread manufactured phantom backward heads in stills that are actually correct. |
| Feet look on backwards / walk janky | Dead upper body from the legs-only script (Defect C) + mirrored y-sway wherever the executed script has y-turns (Defect A; e.g. araking's `walk_legs` has 24). Caveat: the leg-swing channel itself (x-turns) was always sign-correct; if walks still read wrong after C+A, the residue needs fresh analysis (candidates: turn-in-place `TurnDirection`, MOVE easing). |
| Wing beat reads as downstroke; unfixable by any facing change | The stroke is keyframed across x/y/z; the y channel played mirrored (A) and the whole bird was reversed (B). Facing changes rotate a garbled pose — they cannot un-mirror a channel; exactly why every {h, h+π, −h, π−h} × mirror attempt failed. |
| The π−heading per-model flyer split | Artifact of judging y-mirrored poses by eye, then encoding the judgment in a spurious bytecode heuristic. Retail has no split; it dies with the fix (after the probe). |

---

## 2. THE RETAIL TRANSFORM (as evidenced)

Chain, model file → screen. Confidence per element: **[P]**roven / **[S]**trongly-implied / **[A]**ssumption.

1. **3DO load**: vertices/piece offsets read as-is, ×1/65536; no axis change; root piece is a 4-vertex ground
   plate, not drawn. [P]
2. **Model space**: front −z, right −x, up +y (left-handed labels, shared with TA-classic). [P — §1.1]
3. **COB piece state**: angles 65536/circle, linear 65536/unit, per-second speeds, shortest-path turns,
   non-blocking `start-script`; state stored in script-space signs. [P — prior ICD work]
4. **Piece composition**: `world = parent · T(FromParent + move) · Ry(−ry) · Rx(+rx) · Rz(+rz)` — translation
   before rotation, down the tree; **only y negated, at composition time, not in stored state**.
   [P — composer 0x5ad090 verified instruction-by-instruction; `fchs` 0x4eea98; axis-field pairing via
   GetPieceWorldPos 0x4dd104]
5. **Root**: same composer with `(rz, −heading, rx)` from unit fields +0x7c/7e/80; **no flyer facing branch**
   in the render matrix [P — 0x4ee620, `fchs` 0x4ee6a9]. (This proves the shared matrix; that retail's flyer
   *sim* writes heading with ground-unit semantics is closed by verification §4.3, not by the ICD.) Flyer
   altitude appearing in the blit rather than the matrix: [A — not located]. Unit pitch/roll fields exist and
   feed the same matrix; FBI `upright`/`pitchscale`/`bankscale` wiring: [S].
6. **Projection** (unit-frame v, 16.16): `sx = v.x + v.y/4`, `sy = −v.z − v.y/4`, `depth = v.y + 25`; fixed
   oblique, 1 unit = 1 px, cached sprite re-rendered when orientation moves ≥ 8 angle units [P — vertex loop
   0x4edb01; deadband 0x4ee9a0]. Depth-compare direction inside the sprite: [A — unresolved; irrelevant to us,
   our painter sort is consistent at every facing].
7. **Blit**: `(unit.x − cam_x + 5, unit.z − cam_z − terrain_height/2)`. [P — 0x511170 caller]

Known loose end: the ICD report's derived retail velocity `(−sin h, −cos h)` contradicts §1.1+§2.6 (it would
make retail lead with +z); exactly one sign in that derivation chain (velocity idiom vs projection vs fchs
reading) is misattributed. The composite used here is over-determined by the art and the verified cards, so
the fix does not depend on resolving it — but it is why Defect A lands env-gated with a signed oracle rather
than on document confidence alone.

---

## 3. IMPLEMENTATION PLAN (ordered; render-only)

**Forbidden:** any change to `src/sim/*` or `src/cob/vm.cpp`. Heading semantics, `atan2(dx,dz)` bearings,
movement `(sin h, cos h)` are retail-consistent and lockstep-hashed.

**Step 0 — env-gated decisive experiment (~1 hour, before any permanent change):**
- `TA_YNEG=1` → negate the composed piece yaw. `TA_NO_HALFTURN=1` → force `flyHalfTurn = false`. Two
  lines, no deletions.
- **Attack-swing signed test** (the direction oracle): spawn arasword vs an enemy, screenshot mid-`attack1`
  with/without `TA_YNEG`. Flag ON ⇒ windup must rotate torso/sword toward the **sword (−x)** side. This
  distinguishes the correct sign from a wrongly-negated one — statics and "sway flipped" checks cannot.
- **Flyer numeric probe**: with both flags on, compose the fly pose at fixed timestamps and print world
  positions of head/breast vs hair-tip pieces relative to the movement vector for **all six** Zhon flyers at
  `−heading`. Front pieces must lead for every model. Gate Step 2 on this output, per model.

**Step 1 — piece yaw sign (Defect A).** Negate the y angle where the COB rot reaches the composer — at the
two `ps->rot` call sites (game `collect` and the ModelView preview), NOT inside the generic `Xform::then()`
(a future caller passing a world yaw must not inherit a hidden negation); alternatively rename the parameter
`scriptRot` if negating in `then()`. Comment cites retail `fchs` 0x4eea98. Sprite-bake and impostor paths
reuse `collect`, so they inherit the fix. Script-space state, `WAIT_TURN`, shortest-path logic, and
`vm.cpp` stay untouched — retail also negates only inside its composer.

**Step 2 — delete `flyHalfTurn` (Defect B), gated on the Step 0 probe.** Remove the `Anim::flyHalfTurn`
field, `flyHalfTurnOf()`, its assignment, both bake uses, and the live branch. All mobile units, air
included, use `facing = −u.heading`. Also removes today's live-vs-impostor facing inconsistency (the impostor
bake never applied the half turn).

**Step 3 — full-body walk (Defect C).** Play `walk` (not `walk_legs`) when it exists, matching the
bytecode's own design: run the script and let its entry gate (static fed by the `MoveWatcher` /
`GET_UNIT_VALUE 29` pattern) control it; keep `walk_legs` as the fallback for models without `walk`.
Implementation detail to resolve on the way in: which unit-value index 29 is and how our anim driver should
feed it (likely "is moving"). This is a separate commit from Steps 1–2 with its own A/B.

**Step 4 — dead code + record.** Delete the dead `mirror` parameter chain in `collect`. Update
`docs/retail-engine.md` (piece-composition y-negation; no flyer branch; walk state machine). Rewrite the two
superseded memory notes. Portrait facing untouched.

**Kept, explicitly:** `facing = −u.heading`; the projection formulas and tilt; the painter sort;
ground-plate skipping; `facingIndex`; all of `vm.cpp`.

**Deferred follow-ups (separate commits, each behind its own A/B):** retail projection constants
(`TA_RETAIL_PROJ=1` first — changes proportions everywhere; shadows/anchors/occlusion must be retuned);
root pitch/roll from `upright`/`pitchscale`/`bankscale` (flyer banking, slope tilt; render-side state only);
piece-transform fire points (`EmitWpn`) for muzzle flashes — composes correctly once Step 1 is in.

---

## 4. VERIFICATION

All judgments use the **piece-data oracle**, never impressions: front = −z (toes, face), back = +z (cape,
tail, hair, wing roots), right = −x (arasword sword arm). Where a check could pass under either sign, it is
replaced by a signed test.

1. **Statics must not change** (regression guard): arasword/araking/tardemon N/E/S/W cards vs the existing
   goldens. Facing E: blade east, cape/aventail/tail trailing west. (Statics are y-sign-invariant — the
   restore scripts return y to 0 — which is exactly why statics alone can NOT validate the sign; see 2.)
2. **Attack-swing signed A/B** (direction oracle, from Step 0): windup toward the sword (−x) side with the
   fix, shield side without. Attack/death/build animations are otherwise absent from goldens — capture one
   of each on arasword as new goldens.
3. **Flyers**: zonhunt/zongod/zonharp at `−heading` lead with breasts/face (−z), trail hair/wings (+z), all
   four directions — i.e. flipped vs today's goldens. zonbat/zondrake/zonroc unchanged in facing, improved in
   beat coherence. Numeric probe (Step 0) is the gate; screenshots are the record. Any model still reading
   tail-first gets its 3DO dumped before any code reaction — data decides, not eyeballs.
4. **Walk A/B on araking** (its executed `walk_legs` has 24 y-turns; sway must flip and now match the
   stride). **arasword's walk is the documented NULL for Step 1** (its executed script has zero y-turns);
   it changes under Step 3 instead — upper body comes alive. Frame-step goldens at fixed ms offsets.
5. **Retired criteria** (superseded by §1.2's texture-misread finding): the symptom report's "facing E shows
   sword-arm side toward camera", "gold visage leads", and "tardemon E/W must be mirror images" are void —
   the first two derived from the aventail misread; for the third, dump `tail1/tail2` x-offsets to confirm
   the pose asymmetry (side-curled tail) that legitimately breaks E/W mirror symmetry, closing the one
   unexplained observation with data.
6. **Determinism regression**: headless lockstep run — hashes byte-identical to pre-fix (render-only), plus
   a smoke run of both bake paths (sprites + impostors) confirming they re-bake with the new composition.

---

## 5. RISKS & OPEN QUESTIONS

1. **ICD velocity-idiom sign** (§2 loose end): zero impact on this fix (composite over-determined); cheap
   close-out: re-disassemble around 0x41701c with context to classify the negated delta.
2. **Walk jank fully explained?** If walks still read wrong after Steps 1+3, candidates: turn-in-place
   `TurnDirection` handling, MOVE easing, script selection for other gaits. Frame-step and compare hip/torso
   yaw phase against the disassembled keyframe list.
3. **Retail depth-compare direction** in the sprite rasterizer: unproven, documented for completeness;
   our painter order is verified consistent at every facing.
4. **Aim scripts**: when bearing/pitch get passed into `AimWeapon`, use script-space signs unchanged (retail
   negates only at composition) — a trap worth this written warning.
5. **Projection constants adoption** (deferred): risk to tuned shadow/anchor/occlusion offsets; ships behind
   `TA_RETAIL_PROJ=1` with side-by-side shots first.
6. **FBI `orientation`** (retail UNIT_DEF +0x22c): no shipped FBI sets it (verified by grep) — ignorable.

**Bottom line:** keep `−heading`; negate one composed sine (env-gated, proven by the attack-swing oracle);
delete `flyHalfTurn` (gated on the flyer probe); play the full-body `walk`; and stop trusting the gold side
of a helmet.
