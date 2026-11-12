# B2S Pro — KeepAspectRatio handover

Date: 2026-11-09 · Branch `b2s-pro`, code tip `ffe11e1cf` (this document and its updates on top)
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
| `cae1a15e6` | 11:54 | **Fix**: DMD overlays at the wrong height with `Keep aspect ratio` on (§2, stage 8) |
| `f1ea3dd5f` | 12:19 | **`Keep aspect ratio`** becomes a percentage 0..100 (§2, stage 9) |
| `ffe11e1cf` | 17:48 | **`Transparent parts shine through`** setting (§11) |
| `a098ab1ca` | 05:14 | **Composited window renderers**: the less prioritized ones (PinUp player, …) shine through the areas the backglass leaves untouched — host loop + the `isLayeredOverlay` declaration (§12) |
| *(branch tip)* | 06:15 | **CI**: the workflow set trimmed to Windows x64 — the B2S Pro plugin alone, and the application (BGFX) with its plugins (§5) |

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

### Stage 8 — the DMD overlay quad is bottom-up (`cae1a15e6`)

Test report on the 2560 × 2880 display: with `Keep aspect ratio` **on**, the DMD of the document
showed at the wrong position; with the setting off (zero offset) it was fine.

`DrawImage` takes its destination rectangle **top-down**, and the plugin draws in authored top-down
units — which is why `m_layoutOX` / `m_layoutOY` are plain additions everywhere. `DrawDisplay` (the
DMD glass + emitter quad) is fed a **bottom-up** quad: `DMDOverlay::Render` flips the authored bounds
through the surface (`srcHeight - y - h`) before calling it. Adding a top-down placement offset there
moves the DMD **the double** of the border height away from its authored row; it has to be
**subtracted**, horizontally unchanged (no flip):

```cpp
frame = vec4<int>(display.x + viewOX, int(ctx->srcHeight) - display.y - display.h - viewOY, display.w, display.h);
```

Applied where the quad is built, not at the `DrawDisplay` call site, so the "fully clipped away" test
right below compares the coordinates the quad actually uses.

Scope: **only the B2S Pro plugin.** `plugins/b2s/DMDOverlay/` and `plugins/pup/DMDOverlay/` are two
copies of one shared folder (by design, one per plugin); only the B2S copy carries the view offsets,
the PUP copy is untouched and has none. Nothing outside `plugins/b2s/` is touched by the
KeepAspectRatio work, and on this branch `plugins/b2s/` is built solely as
`plugins/b2s-pro/plugin-b2spro64`.

---

### Stage 9 — a percentage control (`f1ea3dd5f`)

The switch became a slider: **`Keep aspect ratio` 0..100** (int setting, host storage, default 100).
`0` is the plain stretch, `100` the full fit of stage 6, and in between the padding that the full fit
needs is applied that part of the way — the one place the value enters is the surface padding, so the
whole renderer keeps working in authored units:

```cpp
const float fitW = wideContent ? contentW : contentH * wndAspect;   // the stage 6 surface
const float fitH = wideContent ? contentW / wndAspect : contentH;
const float part = (keep >= 100) ? 1.f : (0.01f * static_cast<float>(keep));
ctx->srcWidth = m_padW = contentW + part * (fitW - contentW);
ctx->srcHeight = m_padH = contentH + part * (fitH - contentH);
```

Both endpoints are exact and everything between is monotonic: the two axis scale factors run from the
window/image ratio to 1, the black area from nothing to its full width, both linearly in the value
(the distortion itself is not linear in the value — it is a ratio of the two interpolated scales).
`m_layoutOX` / `m_layoutOY` and everything built on them (bulbs, reels, seg displays, the DMD quad of
stage 8, the four padding strips) follow the same value with no extra code. See §7 for the numbers.

It is an int setting and an int slider (`MSGPI_INT_VAL_SETTING`, `PAGEDUI_ITEM_INT`, `%d %%` format); the
settings page keeps 11 items in the same order. The id is **`KeepAspectRatioPercent`**, deliberately not the
old `KeepAspectRatio`: the host stores the old switch as a bool under that id, and reading a stored 0/1 back
as a percentage would hand everybody who used the switch an aspect ratio kept by 0 % (stretched) or 1 %.

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
* **Plugin-only so far**: `Renderer.cpp`, `Window.cpp` and the rest of `src/` are untouched, the branch builds
  the plugin targets only (`cmake --build build-b2s --target B2SPlugin`). With §12 the branch is no longer
  plugin-only: it carries the host half of the composited renderers (3 files under `src/` and the shared plugin
  header), which is where the host/plugin boundary of that feature sits.
* **CI artifacts**: `Package the plugin` writes `b2s-pro-<platform>-<arch>-<config>.zip` while
  `upload-artifact` still pointed at `b2s-plugin-…`, and with `if-no-files-found: warn` it uploaded
  nothing silently. One `env.PKG_NAME` is now the single source of truth for zip name, artifact
  name and upload path, plus a non-empty test and `error` on missing files. Run `42494729602`
  produced all 3 artifacts.
* **What CI builds on this branch** (two workflows, Windows x64 only): `b2s-plugin.yml` builds the
  **B2S Pro plugin alone** (`-DBUILD_APP=OFF --target B2SPlugin`) and uploads `plugins/b2s-pro/` as a
  zip. `vpinball-windows-bgfx.yml` builds the **application with its plugins**, the official BGFX
  Windows x64 flavor: MSYS2 tooling, the external dependencies built by
  `platforms/windows-x64/external.sh` (cached by a hash of `platforms/config.sh` + that script), the
  version stamp in `src/core/git_version.h`, `cmake -DRENDERER=BGFX -DPLATFORM=windows -DARCH=x64
  -G "Visual Studio 18 2026" -A x64`, then the `VPinballX_BGFX64.exe` + PDBs + DLLs + `assets`,
  `plugins`, `scripts`, `docs` zipped — plus the prebuilt `third-party/` archive as its own artifact
  (the archive the 'Quickstart Windows' section of `make/README.md` points at).
  The upstream workflows of `origin/master` (`vpinball.yml` with its 8 desktop configurations,
  `prerelease.yml`, `vpx-test.yml`) are **not on this branch**: `41e85cc22` removed them. These two
  files are the trimmed replacement for the one configuration tested. The 32-bit and the mingw
  plugin builds are made locally with `tools/build_b2s_plugin_windows.sh` (§6).
  On this branch the `plugins/b2s` sources **are** the B2S Pro ones, and the app builds them as
  `plugins/b2s-pro/` (plugin.cfg id `B2SPro`, renderer id `B2SPro`, `plugin-b2spro64.dll`): the app
  artifact is a **complete** test build of §12 (host support + plugin), with no stock `B2S` plugin in
  it — the app loads plugins dynamically, the statically linked table of `src/core/player.cpp:160` is
  the `__EMSCRIPTEN__` web build only. The plugin zip of `b2s-plugin.yml` targets the **official**
  shipped app (stock `B2S` renderer): there the stock plugin must be disabled (`[Plugin.B2S]
  Enable = 0`), and with both enabled and layering on both draw the backglass, one over the other
  (priority `-1` on the stock one).
* **CI status, and the runner quota**: runs `42504150668`, `42964578836`, `42964799055`, `42964832809`
  all ended with `conclusion: failure`, `steps: []`, `runner_name: ""` after ~6 s: **no Windows runner was
  provisioned**, the private repository Windows minutes quota (110/month, **metered ×2** for private repos)
  was exhausted. The repository was made **public** (public jobs are not metered, fair use only), and
  `42971169764` then built the plugin job in ~1 min with a real MSVC compile. Keep the job count to the
  minimum (one configuration each), which is why the matrices are single entries.
* **Both workflows are green on `b6a3a3af9`**: plugin job `43069250944` (~1 min) and application job
  `43069250946` (~25 min, the external dependency cache of `42971169803` hit). The application job is the
  **only MSVC compile of the host sources** available (no local app build: the linux `third-party/build-libs`
  are empty, `external.sh` needs system dev packages, §6), so a host syntax error shows up there, not locally.
* **Test builds on the share** (`\\vpinball\B2Spro`):
  `VPinballX_BGFX-10.8.1-6693-4ca20cc3f-windows-x64-Release.zip` (202 MB, the current app build: layering master
  switch on the Backglass display page, saveable; the composited renderer path; the DMD behind the artwork; the B2S Pro
  plugin built as `plugins/b2s-pro`), plus the older `VPinballX_BGFX-10.8.1-6689-b6a3a3af9-windows-x64-Release.zip` (202 MB, the runnable folder: `VPinballX_BGFX64.exe`,
  PDBs, DLLs, `assets`, `scripts`, `docs`, `plugins/` — **including `plugins/b2s-pro/plugin-b2spro64.dll`**, verified
  in the artifact, so it is the complete §12 test build), and `b2s-pro-windows-x64-msvc-b6a3a3af9.zip` (655 KB,
  `b2s-pro/plugin-b2spro64.dll` + `plugin.cfg`, for an official shipped app). The local mingw zips (9 MB, with the
  runtime DLLs) are equivalent for testing; the 32-bit build is local only (§6).

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

Artifacts dropped on the share (x64, commit `ffe11e1cf`, **zip only** — no unpacked folder):

```
/mnt/pinball/B2Spro/b2s-pro-windows-x64-ffe11e1cf.zip   9.08 MB  md5 c8e86e0fb32af63a24fa81d6694b7db1
    plugin-b2spro64.dll   plugin.cfg
    libgcc_s_seh-1.dll    libstdc++-6.dll    libwinpthread-1.dll
```

Install as `plugins/b2s-pro/`. The `f1ea3dd5f` zip is removed from the share.

---

## 7. Reference geometry (measured / computed)

| Case | Numbers |
|---|---|
| Target display | 2560 × 2880, aspect 0.889 (portrait) |
| `300 (Gottlieb 1975)` — img 2500 × 2200, `GrillHeight=0` | needs 10.77 % per side |
| AFM — img 2000 × 1565, grill shown | needs 15.11 % per side (grill-hidden must match) |
| 16:9 art on a 4:3 window | needs 12.50 % (reference for the old 5 % cap) |
| AFM dot matrix | `X=481 Y=1243 W=1038 H=208` → rows 79 %..93 % of 1565 px |
| **`Keep aspect ratio` morph** — `300` (2500 × 2200, aspect 1.136) on 2560 × 2880 | value 0: window filled, height/width scale ratio 1.278 · 25: 93.5 % high, 3.3 % borders, 1.195 · 50: 87.8 %, 6.1 %, 1.122 · 75: 82.7 %, 8.6 %, 1.058 · 100: 78.2 %, 10.9 %, 1.000 (no distortion) |
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

## 9. Settings page (`cae1a15e6`)

Once a backglass file is loaded the plugin replaces its implicit settings page with a sectioned
**Backglass Settings** sheet — 12 items under 2 headers — and registers the **`Edit "<file>"`** sheet
(the DMD overlay editor: position, size, style, dots, tint, glass, bounds detection, save) below it.
All items are `PAGEDUI_STORAGE_HOST`, keyed by the plugin id `B2SPro`, so they persist in the host
across tables. Today:

| Header | Items |
|---|---|
| Backglass Settings | Show Grill (bool) · Dual Mode (enum) · Upgraded segment rendering (bool) · **Keep aspect ratio (%)** (int slider 0..100, default 100) · **Center vertically** (bool) · **Transparent parts shine through** (bool) · Used LED type (enum) · **Strict file name match** (bool) |
| Modify Backglass | Global animation slowdown · Per-animation slowdowns · (the `Edit "…"` sheet is appended by the host) |

Changelog entries for the renderer work: `docs/Changelog.txt` (the KeepAspectRatio entries in FEATURES
and the DMD quad placement fix in FIXES).

The master switch of the whole transparency/layering feature is not on this page: it is a host setting, on the
**Display** page (§12.1).

---

## 10. Open items

1. **Test on the real display** (`f1ea3dd5f`): round stays round at **100**, nothing is cropped, black
   only in the leftover area, and the value walks smoothly down to 0 (window filled, stretched) with
   `Center vertically` on **and** off. Grill shown/hidden → identical border geometry at 100.
2. **Test the loose file matching** (`1a1baa010`): `Big Guns (Williams 1987)1-1.vpx` on the share,
   with the backglass of the pair name next to it, strict off — and confirm strict on finds nothing.
3. **Test the DMD placement fix** (`cae1a15e6`): AFM `<DMD Target="Backglass" X=481 Y=1243 W=1038
   H=208>` on the 2560 × 2880 display, `Keep aspect ratio` at 100 **and** at 50, `Center vertically`
   on **and** off — the DMD rows must sit at 79–93 % of the art, and a ScoreView target display must
   land in the middle of its own window.
4. **CI**: both jobs need a Windows runner, and the Actions quota currently refuses to give one (the
   runs end with `steps: []`, `runner_name: ""`). Raise the spending limit / add a payment method;
   the workflow set is already trimmed to a single Windows configuration each (§5).
5. Rebuild the Windows artifacts after any further renderer change (only the mingw ones are current).
6. Cherry-pick the final commits onto the main `vpinball` repo branch `b2s-keep-aspect-ratio`.
7. Decide on the 6 upstream fix/diagnostics cherry-picks (§8).
8. **Open decision, closed on review**: saving a document loaded from a legacy `.directb2s` into a
   `.B2SPro` sibling (`4d51d1684`) was implemented, reviewed and **dropped** — saves stay in the
   loaded file (§3).
9. `resources/` is untracked; add `.gitattributes` and commit if the samples should live in the repo.
10. **Test the shine-through flag** (`ffe11e1cf`, §11) on a backglass with transparent pixels (a die-cut
    canvas): with the flag on, the authored backing color under the artwork and the black leftover area
    must be gone. On a dedicated backglass window those areas stay black — the host opaque clear, §11.
11. **Shine-through, host side, done for one case** (§12): compositing the less prioritized window renderers is
    implemented (§12). Revealing the **playfield** under an embedded backglass (the alpha of the region clear,
    §11) and revealing the **desktop** through a dedicated window (translucent swapchain + OS layered window)
    are still open, and stay outside the plugin.
12. **Test the composited renderers** (§12): B2S Pro at priority 60, PinUp player (`PUP`) at 40 on the Backglass
    display page, `Transparent parts shine through` on — the PinUp content must show through the backglass'
    transparent areas. With the setting off, PinUp must not be drawn at all (the legacy path, check the frame
    time of a PUP-only setup is unchanged).
13. **Test builds**: the application artifact of this branch carries the host support **and** the B2S Pro plugin (§5),
    which is the build to test items 12 and 14 with. In a build where the stock B2S plugin and B2S Pro are both enabled
    (official app + the plugin zip), set the stock B2S priority to `-1` — with composited renderers both would draw the
    backglass, one over the other (§12).
14. **Test the layering master switch** (§12.1): off, a `Transparent parts shine through` backglass must stay fully
    opaque (no black holes, nothing drawn below) and the frame time of a PUP-only setup must be unchanged; on, item
    12 applies. Check the value survives a restart and that turning it on mid session needs no table reload.
15. **Master switch storage**: `Player / AncillaryLayering`, contextual (`PropBoolDyn`), saved globally or as a table
    override through the page save, applied live. History of the key, since the test builds on the share span it: the
    `b6a3a3af9` app has the first version (`Global / AncillaryLayering`, a row on the Display **home** page, which is
    `SaveMode::None` → the toggle applied live and was **never stored**); `ed30cfb36` had **no switch at all** (the
    commit which was meant to restore an earlier removal is not an ancestor of it, so the switch silently disappeared);
    `4ca20cc3f` is the version described in §12.1. A value stored under the old key is not read back. Nothing in the
    renderer layer depends on the section, so moving it to another one at the promotion is a two line change.

---

## 11. Transparent parts shine through (`ffe11e1cf`)

Backglass images carry transparent pixels (die-cut canvases, a DMD background not covering its view, a
`.B2SPro` canvas with holes). The plugin was making those pixels opaque **itself**, in two places:

- the authored **canvas backing color** (`CanvasBacking`, the B2S Pro feature compositing transparent canvas
  pixels over an authored color, reference fB:617-627) is a 1×1 texture drawn over the **whole padded
  surface**, under everything
- the **`Keep aspect ratio` leftover area** is painted with the 1×1 opaque black texture (§2 stage 6)

The images themselves are drawn with their own alpha and the host alpha-blends them, so nothing more is
needed on the plugin side. The new **`Transparent parts shine through`** setting (default off: the view
stays fully opaque as before) leaves both out — no backing color under the artwork, no black in the
leftover area — so the plugin contributes nothing where its images are transparent.

**Where the limit is.** What is *underneath* is the host's decision, and the host clears an ancillary
window/region **opaque**:

- embedded backglass: `Renderer::ClearEmbeddedAncillaryWindow` → `DrawEmbeddedQuad(..., 0, 0, 0)` draws the
  region clear with `vColor_Intensity(r, g, b, **1.f**)` (`src/renderer/Renderer.cpp:1920`) — opaque black
  over the playfield scene that is already in that target
- a dedicated backglass window: the render pass takes the window back buffer with `useRTContent = false`
  (`Renderer.cpp:1893`), i.e. it is cleared, and VPX creates its windows without any translucency support
  (no transparent-window flag anywhere in `src/renderer/Window.cpp`)

So with the flag on, the transparent pixels show the **host clear** (black) instead of the authored backing
color and the plugin-painted black borders. To reveal the playfield (embedded) or the desktop (dedicated
window), the host clear has to keep alpha — the embedded one is a one-liner at that `DrawEmbeddedQuad`
call (alpha 0, and the region clear left out when the plugin asks for it), the dedicated window needs a
translucent swapchain. Both are **host** changes, outside the plugin-only scope of this branch: the flag is
the plugin-side half, ready for it.

If the host side is picked up, the plugin does not have to invent a channel: the setting is
`PAGEDUI_STORAGE_HOST`, so its value is stored by the host itself (`src/core/VPXPluginPagedUI.cpp`,
`src/ui/live/ingameui/PluginSettingsPage.cpp`) under the plugin id `B2SPro` — the host clear can read the
same value. `VPXRenderContext2D::rendererData` is **not** such a channel: the host fills it with its own
`AncillaryRenderSetup` (`Renderer.cpp:1591`), it is not plugin-provided.

**Update**: the host side was picked up for the renderer stacking case (§12), and the master switch of §12.1 gates
this flag: `ShineThrough()`, which the drawing tests, is the plugin setting **and** the host switch.


---

## 12. Composited window renderers (`a098ab1ca`)

§11 makes the plugin leave pixels unpainted; **what shows there is the host's decision**. The case asked for is
the one VPX already has settings for: the PinUp player sits at a lower priority than the backglass and should
shine *through* it.

**How the host chose a renderer before** (read from the code, all paths in this repo):

- `Player::OnAuxRendererChanged` (`src/core/player.cpp:2774`) collects what the plugins declare for the
  Backglass / ScoreView / Topper windows, registers a `Priority.<renderer id>` setting per window section
  ("Higher values are priorized other lower ones"), sorts the list **descending** and erases renderers whose
  priority is `< 0`.
- `Renderer::RenderAncillaryWindow` (`src/renderer/Renderer.cpp:2213`) walked that list and **stopped at the
  first renderer which claimed the window**. So a lower-priority PinUp player was not dimmed, not covered —
  it was simply never drawn, whatever its priority was.

**What the host does now**:

| File | Change |
|---|---|
| `plugins/plugins/VPXPlugin.h:147` | `AncillaryRendererDef` gains `int isLayeredOverlay;` — declared by a renderer which draws with alpha and leaves the areas it does not cover untouched |
| `src/core/player.cpp:2786` | the collected entries are value initialized (`assign(count, AncillaryRendererDef {})`): a plugin built against the older 5-field struct fills only the fields it knows, so the new one reads 0 (= the behaviour those plugins have), never whatever the allocation held |
| `src/renderer/Renderer.cpp:2207` | when **no** renderer of the window declares layering, the winner-takes-all loop is kept exactly as before; when one does, every claiming renderer is drawn, from the least to the most prioritized, each on a freshly reset `VPXRenderContext2D` and with `ResetRenderState()` between them, all in the same pass |

The order is bottom up because the drawing order is the stacking order: each renderer alpha blends onto what the
ones below it left, so the ones below show through the untouched areas of the ones above. Nothing else is needed:

- **no clear between renderers, no new render target, no shader change** — the plugin draws (`DrawImage`,
  `DrawDisplay`, the segment renderers) are plain alpha blends into the window target
- the dedicated window target is already cleared to `0x00000000`, alpha 0 (`Renderer.cpp:2205`)
- the content bounds kept are the last writer's, i.e. the most prioritized claimer — as in the non composited
  case, so the "fit the output to the content" behaviour follows the renderer on top
- **cost**: compositing draws renderers which end up covered by an opaque one. That is the price of the feature,
  and the reason it is declared by the renderers rather than always on
- the **embedded** path is untouched: its region clear stays the opaque quad of §11, so on an embedded backglass
  the areas left open show black. Revealing the playfield scene there is the one-liner of §11, still open
- ABI, both directions: a 6-field plugin on a 5-field host is copied with the host's (smaller) size, the extra
  field is simply not read; a 5-field plugin on a 6-field host reads 0 thanks to the value initialization above

**How the plugin declares it** (`B2SPro`):

- `B2SServer::OnGetRenderer` copies the renderer def and sets `isLayeredOverlay` from a `ShineThrough()`
  accessor: what is declared is exactly what the plugin draws that frame
- `Transparent parts shine through` is hand rolled from `MSGPI_BOOL_VAL_SETTING` (`B2SRenderer.cpp`) so that its
  setter broadcasts `VPXPI_EVT_AUX_RENDERER_CHG`: the host re-collects the renderers and picks the new
  declaration up. The statics the setter needs (`s_msgApi`, `s_endpointId`, `s_auxRendererChgId`) are set in
  `B2SRenderer::RegisterSettings`, which runs once per plugin load and is kept for the whole plugin life (a
  renderer is created and destroyed per table, the settings and the registration are not)
- with the setting off the plugin is opaque, declares 0, and the host spares drawing what would be covered:
  **no cost, and the previous behaviour**

| Backglass page setup | drawn (bottom → top) |
|---|---|
| B2S Pro 60 / PUP 40, `ShineThrough` off | B2S Pro alone (PUP not drawn, as before) |
| B2S Pro 60 / PUP 40, `ShineThrough` on | PUP, then B2S Pro: PinUp through the backglass' transparent areas |
| B2S Pro 60 / PUP 40, PUP has nothing to show | B2S Pro alone |
| B2S Pro 60 (layered) / PUP 40 / Nixie 20 | Nixie, PUP, B2S Pro: PUP covers Nixie (higher priority), B2S Pro reveals both where transparent |

**Renderer id, and the two plugin flavours**: the priority key is the renderer id. This branch declares `B2SPro`
everywhere (plugin.cfg id, exports, renderer id), including in its application build, so that artifact has a single
B2S family renderer and nothing is drawn twice. The case to keep in mind is the **official** app (renderer id `B2S`)
plus this plugin (`B2SPro`) at priority ≥ 0 with a composited window: both draw the backglass, one over the other —
set the stock B2S priority to `-1` (it is erased from the list). Once B2S Pro replaces B2S there is a single `B2S`
renderer, so this is moot.

**Checked**: the walk was exercised as a standalone stub (`/tmp/host_layer_test.cpp`, 10 cases, all passing):
nobody declares layering with two renderers → exactly today's draws; single legacy renderer; nobody claims;
layered top over opaque PUP; layered top which does not claim; layered with an opaque claimer in between; all
layered; single layered renderer; nobody claims with one layered. The plugin builds natively and with mingw.

### 12.1 Master switch (`Player / AncillaryLayering`, default off)

The feature above is global cost (a whole renderer drawn and covered) and global semantics (pixels of one plugin
appearing in the window of another). One host switch turns it off for every plugin at once, and it is off by
default: an app started with default settings behaves **exactly** as the shipped one, and one where it is never
turned on pays nothing for the feature.

| Piece | Where |
|---|---|
| Setting `Player / AncillaryLayering`, `PropBoolDyn` (contextual), default **false** | `src/core/Settings_properties.inl`. Contextual, like the display settings: saved **globally** or as a **table override**, ini key `Player / AncillaryLayering` |
| In game UI: **Display → Backglass display** page, header *Plugin rendering*, item *Transparent Plugin Rendering* | `src/ui/live/ingameui/DisplaySettingsPage.cpp`, end of `DisplaySettingsPage::BuildWindowPage`, gated on `m_wndId == VPXWindowId::VPXWINDOW_Backglass`. That page is `SaveMode::Both`, so the item gets the page's *save globally / save as table override* buttons |
| Live values | `Player::m_ancillaryLayering`, `Renderer::m_ancillaryLayering`. **No settings lookup in the render loop**: both cached. The player seeds its copy from the active settings at construction, the renderer in `UpdateSettings` (table load), and `Player::SetAncillaryLayering` is the only writer. The UI item follows the pattern of every item of that page: its *live* value is `Player::GetAncillaryLayering()`, its setter only applies the change live, and the page's **Save** (SaveMode::Both) stores it, globally (resetting the overrides) or as a table override, through the generic `Settings::Set(propId, v, isTableOverride)` API. The modified state is the live value against the stored one, so the save button shows up as soon as you toggle it. An external ini edit is picked up at the next table load |
| Gate | `Renderer::RenderAncillaryWindow`: `composited = m_ancillaryLayering && any renderer declares isLayeredOverlay`. Off, the winner-takes-all loop runs, byte for byte the shipped behaviour |
| Pushed to the plugins | `VPXRenderContext2D` gains `int isLayered` (**appended** at the end), written by the host at **both** ancillary call sites: 1 in the composited loop, 0 in the winner-takes-all one. It says « this window is composited, the renderers below you are drawn », which is exactly what a renderer needs to know when it draws. An host without the field leaves it at 0 |
| Plugin side | `ShineThrough()` is our own setting and drives the **declaration** (`isLayeredOverlay`). `TransparencyActive(ctx)` = that setting **and** `ctx->isLayered`, and drives the **drawing** (canvas backing, aspect padding, the dot matrix behind the artwork). The two deliberately differ: we declare a capability, the host decides whether it is used, and we draw for what it decided — so areas are never left open in a window where nothing is drawn below them |

Why the state is carried **at the render call** rather than at the renderer collection: the render call is where the
host knows it (the set of renderers claiming the window and their declarations is evaluated there), and it is where
the plugin draws. With compositing off, the areas a transparent renderer leaves untouched show the **window clear**
(black), not the renderers below, so the renderer has to know and draw its whole window opaquely. An earlier version
carried the state in `GetAncillaryRendererMsg::layeringEnabled` with sentinel values; it is gone — the render context
field replaced it, with no settings transfer, no sentinels and nothing to keep in sync between a collection and a
frame. Compatibility is by construction: a plugin which does not know `isLayered` never reads it (it draws opaquely,
as it always did), and a host which does not know it leaves it at 0, which reads as « not composited ».

**Cost when off**: per window and per frame, the walk of the (tiny, already existing) priority list plus a member
bool read. No settings lookup, no message, no allocation, no extra render state. The UI is one row on an existing
page.

**Live**: `Player::SetAncillaryLayering` updates the renderer value and re-collects the renderers, which makes them
re-declare; the composited state itself reaches a plugin at the next frame, through `ctx->isLayered`. No table reload,
no restart. A change made by editing the ini by hand is picked up at the next table load (the Player seeds from the
active settings, the renderer in `UpdateSettings`).

**Why it lives on the Backglass display page**: the first version was a row on the Display *home* page, which is
`SaveMode::None` (its items are expected to persist themselves, and the page shows **no save button**, `InGameUIPage::Render`
draws a placeholder in place of it). A `Global`, non contextual property cannot be saved by any page save flow, so the
toggle applied live and was lost on restart, with no way to store it, globally or per table. As a contextual property on
the per window display page, it saves like every other display setting.

**The build must contain it** (that is how it went missing): the switch is host code, so a plugin zip alone can never
provide it. `ed30cfb36` reached the share with no switch at all, because the commit which restored it after the
plugin side work was not an ancestor of that commit — `git log --oneline` of the build is the check, and
`grep -rn AncillaryLayering src/` in the tree of the build is the one minute test. Installed builds on the share to
be aware of: `b6a3a3af9` (row on the Display home page, not saveable), `ed30cfb36` (no row), `4ca20cc3f` (the row on
Display → Backglass display, saveable).

### 12.2 The dot matrix behind the artwork (read from the image, no setting)

A grill can be authored with a hole in it (the die cut, the window of a backbox). Drawing the DMD **over** the
artwork then paints the dot matrix on the facade, which is the shipped behaviour. The display sits behind that
hole, so when it *can* be seen through, the display is drawn **before** the backglass images: the openings of the
artwork reveal it, its opaque parts (mesh, frame, screws) cover it, as in a real backbox.

| Piece | Where |
|---|---|
| Detection | `B2SRenderer::ArtworkHasOpening(artwork, x, y, w, h)`: the authored area of the display (`<Displays><DMD X/Y/W/H>`, in artwork pixels) is sampled in the artwork. **No alpha channel in the file → no opening** (`VPXTEXFMT_sRGBA8` is the only format carrying one), fully opaque over the area → no opening. It counts the pixels whose alpha is below half and requires at least a quarter of the area transparent, so soft edges and softly matted artwork do not flip it. Cached per (texture, area) pair, sampled once |
| When it applies | `(ctx->isLayered != 0) && ShineThrough()`: the window is **composited by the host** (the display settings master switch, §12.1) **and** our transparent parts shine through. Opaque, the artwork would hide the display, so the shipped order (display on top) is kept, at no cost |
| The two passes | `B2SRenderer::RenderBackglass`: `RenderDisplays(..., behindArtworkPass=true, ...)` before the off/on images, `RenderDisplays(..., behindArtworkPass=false, ...)` after them. `RenderDisplays` classifies each display and skips the ones of the other pass, so each display is drawn in exactly one of the two |
| ABI | `VPXRenderContext2D` gains `int isLayered` (appended at the end), written by the host at **both** ancillary call sites: 1 in the composited loop, 0 in the winner-takes-all one. Zero is also what a host without the field gives, and an embedded (3D) render is never composited |

The state is available exactly where it is used: the host knows at the render call whether the window is composited,
so the plugin stores nothing, and the rule of §12.1 (a renderer leaving areas open must know when nothing is
composited below it) is carried by the same field. An opening is looked for in the image, not in a setting: a table
whose artwork covers the display area is a table where it cannot be applied, and there is nothing to choose there.

**ABI compatibility** is by construction, in both directions: the fields are **appended** at the end of their
struct, and every one of them reads as "the feature is off" for a counterpart which does not know it.

| Situation | Result |
|---|---|
| New host, **old plugin** | The plugin fills the renderer def with a 6 field aggregate initializer: the host reads its `isLayeredOverlay` as 0, so the window is not composited; the plugin never reads `isLayered` and draws opaquely, as it always did |
| **Old host**, new plugin | `entries` has the old stride, so the plugin never reaches `isLayeredOverlay` (reads 0, declares opaque); the render context leaves `isLayered` at 0, so `TransparencyActive` is false and the plugin paints its whole window |
| New host, new plugin | Layered rendering with the master switch |

The structs are all in `plugins/plugins/VPXPlugin.h`, the file the plugin SDK is built from: the plugin side compiles
against the same header as the host in this tree, and an out of tree plugin built from an older SDK keeps working.
That is also why the fields are appended and not inserted.
