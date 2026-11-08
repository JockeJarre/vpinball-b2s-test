# B2S Pro — KeepAspectRatio handover

Date: 2026-11-08 · Branch `b2s-pro`, code tip `1a1baa010` (this document and its updates on top)
· Repo `JockeJarre/vpinball-b2s-test`

---

## 1. What is on the branch today

| Commit | Time | What it did |
|---|---|---|
| `9a111ce35` | 10:00 | Cherry-pick of upstream `5414cf9b3`: the `KeepAspectRatio` setting, black borders painted outside the image aspect |
| `f2e52cfb3` | 14:53 | Build B2S Pro as a **standalone** plugin: id `B2SPro`, libraries `plugin-b2spro` / `plugin-b2spro64`, folder `plugins/b2s-pro`; CI runs on pushes to `b2s-pro` |
| `16e6520b9` | 14:57 | Add `Motion.cpp`, `Physics.cpp`, `DynamicLight.cpp`, `Reel3D.cpp` to the plugin build (CMake + vcxproj + filters) — the link failure `Update for B2S pro` left behind |
| `9dbc6ad3c` | 15:25 | Fix the CI artifact upload: zip name vs upload path mismatch, unified in one `env.PKG_NAME`, `if-no-files-found: error` |
| `8a695bf11` | 16:45 | Aspect reference = **whole image (grill included)**; `MaxBorder` cap; **`.B2SPro` write guard removed** |
| `64fd4e15e` | 16:51 | `MaxBorder` default 15 % → 5 % per side |
| `6d831fca9` | 16:54 | `BorderAlignment` (centered / all top / all bottom) |
| `b351bceaa` | 17:16 | Whole-view **fit transform**: scale everything as one picture; `MaxBorder` and `BorderAlignment` dropped |
| `b060c3d8f` | 17:26 | Build script: 32-bit `libgcc_s_dw2-1.dll` (dwarf2) vs 64-bit `libgcc_s_seh-1.dll` (SEH); root-less mingw recipe documented; `.gitignore` |
| `103f86466` | 17:52 | **Surface padding replaces the per-object transform** (the current, final model) |
| `097249225` | 18:05 | `Center vertically` setting (default on; off = content on the floor) |
| `4eb064b7b` | 18:26 | Content **centered horizontally**, no setting for it; `DrawBackgroundPatch` double-offset fix |
| `79cb1f491` | 19:22 | handover: the save target of the DMD overlay edits spelled out |
| `1a1baa010` | 19:33 | **`Strict file name match`** setting: loose backglass file matching, table name matched up to its first `)` (§4) |

`git diff --stat 9a111ce35^..HEAD` — 19 files, +275 / −94.

---

## 2. The aspect-ratio problem, step by step

The target display that drove everything: **2560 × 2880 portrait** (aspect 0.889) used as a
backglass screen. The art is much wider than that (e.g. AFM 2000 × 1565 = 1.278, `300` 2500 × 2200
= 1.136), so the window shape and the image shape are far apart: the naive stretch distorts
heavily, and letterboxing eats a big slice of the view.

The host (VPinball) maps the plugin's source surface onto the window **axis by axis**
(`Renderer.cpp`: `srcX / ctx->srcWidth`, `srcY / ctx->srcHeight`). The plugin draws in *authored
image units*; the host does the stretching. That single fact is what all the designs turn on.

### Stage 1 — bars painted over the stretched view (`9a111ce35`)
Border strips drawn over the top/bottom (or left/right) of the already-stretched image.
Distortion stays (the image is still stretched, the bars just hide part of it) and artwork is cut.

### Stage 2 — the aspect reference must be grill-independent (`8a695bf11`)
Measured on real files: AFM (`2000 × 1565`, `GrillHeight=373`). Using the *source surface* aspect
(as before) drops the grill when the grill is hidden, and the borders jump to 23.5 % per side
(47 % of the view) instead of 15.2 %. Using the **whole image aspect, grill included**, makes the
borders independent of `Show Grill`, which is the behaviour a user expects.

### Stage 3 — capping the borders (`8a695bf11`, `64fd4e15e`)
`MaxBorder` (percent per side, 0 = no limit) was added to stop a far-off window shape from eating
half the view. 15 % default does not clamp a portrait screen: `300` on 2560 × 2880 needs 10.9 %
per side (measured 10.77 % in the screenshot) → default moved to **5 %**, which is above what the
common shapes need (16:9 art on a 4:3 window needs 4.7 %), so ordinary setups stay untouched.

### Stage 4 — alignment (`6d831fca9`)
AFM authors its dot matrix **inside the backglass**: `<DMD Target="Backglass" X=481 Y=1243 W=1038
H=208>` → rows 79 %..93 % of the 1565 px image. A centered 15.2 % letterbox on a portrait window
covers rows 85 %..100 %, i.e. it cuts the dot matrix in half (measured: 15.19 % top / 14.85 %
bottom bars in the user screenshot). Hence the content is authored **at the bottom** of the image
and must not be cut from there → `BorderAlignment` (centered / all top / all bottom).

### Stage 5 — fit the whole view, no crop (`b351bceaa`)
Bars and caps are band-aids: as long as the surface is stretched, "no crop" and "no distortion"
fight each other. The fix at that point was a view transform applied to every drawn element
(background, illuminations, reels, dot matrices, motion and physics balls, pivot animations) into
the largest rectangle of the authored shape that fits the window. Worked geometrically, but touched
every drawing call site — and the user's review said it plainly: *there should not be any
recalculation needed on each and every object on the screen; set the size of the fictitious screen
to the new value and the B2S plugin handles the rest.*

### Stage 6 — pad the surface (`103f86466`, current)
That is exactly what the host mechanism allows:

```
outWidth/outHeight  = window shape
srcWidth/srcHeight  = drawing surface (authored units), stretched axis by axis onto the window
```

Pad the surface to the **window shape** and the two axis scales become equal
(`outWidth/srcWidth == outHeight/srcHeight`) → uniform scaling, the image keeps its shape, a round
bulb stays round, nothing is cropped, **no drawing code is involved**:

```cpp
const float wndAspect = ctx->outWidth / ctx->outHeight;
ctx->srcWidth = m_padW = (contentW / contentH > wndAspect) ? contentW   : contentH * wndAspect;
ctx->srcHeight = m_padH = (contentW / contentH > wndAspect) ? contentW / wndAspect : contentH;
```

`m_padW` / `m_padH` are the authored content rectangle; `RenderAspectPadding()` paints the leftover
black at the **end** of the frame (animations, illuminations and score displays are authored on the
image and can spill into the padding area). All the per-object transform code (`ViewTransform`,
`DrawImageX`, `SetViewTransform` on seg/DMD/motion/physics) was reverted to the original, and
`MaxBorder` / `BorderAlignment` were dropped (with no cropping they are meaningless): settings went
11 → 9.

### Stage 7 — placement of the content in the padding (`097249225`, `4eb064b7b`)
Padding alone pins the content to the top-left corner. Placement is a pure **translation**, the
scale stays with the padded surface:

```cpp
m_layoutOY = centerVerticallyProp_Get() ? 0.5f * (m_padH - contentH) : m_padH - contentH;
m_layoutOX = 0.5f * (m_padW - contentW);                 // always centered, no setting
```

* **Center vertically** (default on): black split above and below. Off: content on the floor, all
  black on top — which is what dot-matrix-heavy backglasses want.
* Horizontal: centered, no setting — there is nothing sensible to choose, and on a portrait
  backglass screen the horizontal padding does not even occur.
* Applied through `ContentX()` / `ContentY()` at the drawing sites, `SetViewOffset(ox, oy)` on the
  segment displays and the DMD overlays, `ox`/`oy` in `SegGraphics::DrawToContext`; the padding is
  filled on the four sides of the placed content. Settings: 9 → 10 page items.

---

## 3. `.B2SPro` write guard removed (`8a695bf11`)

`common.cpp` refused to save DMD-overlay edits into `.B2SPro` documents (`IsB2SProFile`, commit
`d027e00b1`): a `.B2SPro` edit was rejected with "DMD overlay edits are not saved into .B2SPro files".
A `.B2SPro` file **is** a `.directb2s` XML document plus the extra B2S Pro tags, so the guard was
protecting nothing; `.B2SPro` is edited and saved exactly like `.directb2s`.

The save target, spelled out (this is what the handover left implicit, and what was decided after the
review):

| | guard present | guard removed (current) |
|---|---|---|
| table has a `.B2SPro` (the lookup picks it) | save refused, log warning | saved into **that `.B2SPro`** |
| table has only a `.directb2s` | saved into the `.directb2s` | saved into **that `.directb2s`** |

The save always goes to *the file the document was loaded from*: `m_overlayDoc.path = m_b2sPath`, and
`FindB2SFile()` prefers `.B2SPro` over `.directb2s` (both by table name and by folder name). No
redirect to a `.B2SPro` sibling — the idea of writing a `.B2SPro` for a legacy-only table (commit
`4d51d1684`) was reviewed and dropped, it creates a second document that can drift from the loaded
one. An opened `.directb2s` saves as `.directb2s`, an opened `.B2SPro` saves as `.B2SPro`.

`WriteDisplays` rewrites only the `<DMD>` children of `<Displays>`, everything else in the document
(including the embedded base64 images) is preserved; the write goes through a `.tmp` file renamed over
the original, so a failed save cannot leave a truncated file, on a worker thread so a big document
cannot stall the play thread.

---

## 4. Backglass file lookup — loose matching (`1a1baa010`)

A feature of the original B2S Server: the backglass file does not have to be named exactly after the
table. New setting **`Strict file name match`** (id `StrictFileNameMatch`, on the backglass settings
page, **on** by default).

`FindB2SFile()` (`B2SServer.cpp`) in priority order:

1. **full name match, always first, whatever the setting says** — `<tableName>.B2SPro`,
   `<tableName>.directb2s`, then the same two with the *folder* name (the file layout where the table
   shares its folder with `b2s` / `pup` / `flex` / music companions);
2. only with the setting **off**, and only when step 1 found nothing: the name **prefix**, which is the
   table name through its first `)` — the `FlipperName (Company Year)` the pair is conventionally named
   after — scanned over the folder: `Big Guns (Williams 1987) Something Else.vpx` takes
   `Big Guns (Williams 1987).B2SPro`, and `Big Guns (Williams 1987).vpx` also takes it (its full name
   match is found in step 1 anyway).

Details:

- `.B2SPro` outranks `.directb2s` at every stage, both in the exact and in the loose stage.
- A name **without** `)` has the whole name as prefix, so it stays an exact match: `Big Guns 1.2.vpx`
  does **not** accept `Big Guns.B2SPro`.
- A folder with several matching variants resolves to the name **closest** to the prefix (the shortest,
  then alphabetically), so it resolves the same way at every load; the prefix comparison is
  case-insensitive, like everything else in the lookup.
- Changing the setting takes effect **at the next table load** — the document lookup runs in
  `TryLoadB2S()`, which is called on table start, on `SetTableName` and on `SetWorkingDir`, and it
  never re-reads a document that is already loaded.

Exercised with a standalone copy of the algorithm (`/tmp/b2sfind_test.cpp`, 8 cases: exact-wins,
strict-off-loose, no-parenthesis, multi-variant tie-break), all as specified. Field case at hand: the
share holds `Big Guns (Williams 1987)1-1.vpx` — prefix `Big Guns (Williams 1987)` — with no backglass
file of that name next to it, so it is the table to try this on (`Big Guns (Williams 1987).B2SPro` from
the B2S Pro release goes next to it, loose matching has to be turned off for it).

---

## 5. Packaging and CI

* **Standalone coexistence**: `Update for B2S pro` renamed the plugin (`plugin.cfg` id `B2SPro`,
  `plugin-b2spro[64]`) but not the build. CMake, the VS projects and the build script now emit
  `plugin-b2spro` / `plugin-b2spro64` in `plugins/b2s-pro/`, so B2S Pro installs **next to** the
  stock B2S plugin instead of replacing it.
* **CI artifacts**: `Package the plugin` writes `b2s-pro-<platform>-<arch>-<config>.zip` while
  `upload-artifact` still pointed at `b2s-plugin-…`, and with `if-no-files-found: warn` it uploaded
  nothing silently. One `env.PKG_NAME` is now the single source of truth for zip name, artifact
  name and upload path, plus a non-empty test and `error` on missing files. Run `42494729602`
  produced all 3 artifacts.
* **Current CI blocker (infrastructure, not code)**: runs `42504150668` and its re-run end with
  `conclusion: failure`, `steps: []`, `runner_name: ""` after ~6 s. GitHub refuses to provision a
  `windows-latest` runner — Actions spending limit / billing quota
  (<https://github.com/settings/bits>). The code compiles cleanly locally; options are to raise the
  spending limit / add a payment method, or move the job to `ubuntu-latest` with the mingw
  cross-build.

---

## 6. Local builds (no CI needed for testing)

Linux native (plugin only):

```bash
cmake -DPLATFORM=linux -DARCH=x64 -DBUILD_APP=OFF -DPOST_BUILD_COPY_EXT_LIBS=OFF \
      -B build-b2s -DCMAKE_BUILD_TYPE=Release
cmake --build build-b2s --target B2SPlugin -j"$(nproc)"
```

Windows cross-build with mingw-w64:

```bash
PATH=~/.local/mingw/root/usr/bin:$PATH ./tools/build_b2s_plugin_windows.sh x64 Release
PATH=~/.local/mingw/root/usr/bin:$PATH ./tools/build_b2s_plugin_windows.sh x86 Release
```

* Root-less toolchain (documented in the script header): `apt-get download` +
  `dpkg -x … ~/.local/mingw/root` (12 .deb for x64, 9 more for i686, ~730 MB), `-posix` variants
  symlinked to the bare names (`win32` threads have no POSIX threads).
* Runtime DLLs per architecture: x64 → `libgcc_s_seh-1.dll`, x86 → `libgcc_s_dw2-1.dll`
  (dwarf2, 32-bit mingw has no SEH), plus `libstdc++-6.dll`, `libwinpthread-1.dll`.
* Export check: `objdump -p …| grep B2SProPlugin` → `B2SProPluginLoad`, `B2SProPluginUnload`.
* MSVC `windows-x64` / `windows-x86` still need a Windows/MSVC box (CI); the mingw build is
  functionally the same plugin, only the C/C++ runtime differs.

* The build script **does not zip** the result: it only stages `plugin-b2spro64.dll` /
  `plugin-b2spro.dll`, `plugin.cfg` and the runtime DLLs in
  `build-b2s-win-<arch>/plugins/b2s-pro/`. Zip that folder and name the zip after the commit — a zip
  left over from an earlier build is easy to ship by accident (it happened, hence the commit-named
  files). No `unzip` on this box, use python:

```bash
python3 - <<'EOF'
import zipfile, os
out = "build-b2s-win-x64/plugins/b2s-pro"
with zipfile.ZipFile("b2s-pro-windows-x64-<sha>.zip", "w", zipfile.ZIP_DEFLATED) as z:
    for f in sorted(os.listdir(out)): z.write(os.path.join(out, f), f)
EOF
```

Artifacts dropped on the share (x64, commit `1a1baa010`):

```
/mnt/pinball/B2Spro/b2s-pro-windows-x64-1a1baa010.zip   9.08 MB  md5 a63afc302b1ca5c67663e1923175734f
/mnt/pinball/B2Spro/b2s-pro-1a1baa010/                  unpacked, drop-in ready
    plugin-b2spro64.dll   plugin.cfg
    libgcc_s_seh-1.dll    libstdc++-6.dll    libwinpthread-1.dll
```

Install as `plugins/b2s-pro/`. The `4eb064b7b` artifacts were removed from the share (same renderer,
they lack the file-name matching).

---

## 7. Reference geometry (measured / computed)

| Case | Numbers |
|---|---|
| Target display | 2560 × 2880, aspect 0.889 (portrait) |
| `300 (Gottlieb 1975)` — img 2500 × 2200, `GrillHeight=0` | needs 10.77 % per side |
| AFM — img 2000 × 1565, grill shown | needs 15.11 % per side (grill-hidden must match) |
| 16:9 art on a 4:3 window | needs 12.50 % (reference for the old 5 % cap) |
| AFM dot matrix | `X=481 Y=1243 W=1038 H=208` → rows 79 %..93 % of 1565 px |
| Hurricane (Cyclone) screenshot | window 1307 × 1468 (0.8903), content 1.0802 → 8.7 % top/bottom; the wheel in the image is 579 × 569 (1.0176), i.e. near square, so a stretch is obvious to the eye |

Sample files: `resources/backglass/` (`300 (Gottlieb 1975).B2SPro`, `Attack from Mars (Bally 1995)
2.0.1 v78.2.directb2s`, `300.png`, `AttackFromMars.png`, `AttackFromMarsOldPlugin.png`,
`Hurricane.png`) — left **untracked** on purpose (test data, not source).

---

## 8. Upstream comparison (`evilwraith/vpinball@b2spro-plugin`, Wes Smith, 19 commits on `42fd8405a`)

Verdict: near-identical siblings from the same `Update for B2S pro` base.

* Upstream is ahead on: mergeability into master, diagnostics/logging, and several correctness fixes
  (notably `B2SProSequence.cpp` exit routes / trough & sequence-group semantics).
* This branch is ahead on: **KeepAspectRatio** (all of the above), 2-phase lighting, standalone
  packaging (`plugins/b2s-pro`, coexistence with stock B2S), a working CI, richer pivot/grid handling.
* Fix/diagnostics commits worth cherry-picking here: `e817465de`, `9fca05a26`, `6994c26ef`,
  `9bf69cbc0`, `86d2b4549`, `b8fda591f`.
* The upstream sequence semantics (`B2SProSequence.cpp`) are still to be studied before any merge.

---

## 9. Open items

1. **Test on the real display** (`4eb064b7b`): round stays round, nothing cropped, black only in the
   leftover area, AFM DMD rows 79–93 % fully visible, grill shown/hidden → identical border
   geometry, with and without `Center vertically`.
2. **Test the loose file matching** (`1a1baa010`): `Big Guns (Williams 1987)1-1.vpx` on the share,
   with the backglass of the pair name next to it, strict off — and confirm strict on finds nothing.
3. **CI**: raise the Actions spending limit, or move the job to `ubuntu-latest` + mingw.
4. Rebuild the Windows artifacts after any further renderer change (only the mingw ones are current).
5. Cherry-pick the final commits onto the main `vpinball` repo branch `b2s-keep-aspect-ratio`.
6. Decide on the 6 upstream fix/diagnostics cherry-picks (§8).
7. **Open decision, closed on review**: saving a document loaded from a legacy `.directb2s` into a
   `.B2SPro` sibling (`4d51d1684`) was implemented, reviewed and **dropped** — saves stay in the
   loaded file (§3).
8. `resources/` is untracked; add `.gitattributes` and commit if the samples should live in the repo.

---

## 10. Settings today (backglass page, 11 items)

Show Grill · Dual Mode · Upgraded segment rendering · **Keep aspect ratio** · **Center vertically**
· Used LED type · **Strict file name match** · All slowdown · Slowdowns · (2 section headers)

Changelog entries: `docs/Changelog.txt` lines 19–34.
