# Pre-flight: the installer sets up EDVR and Elite for a good first launch (design, 2026-10-14)

## Status

- **State (2026-10-14):** design only. Nothing is built, and nothing has flown.
  It waits on Sean's answers to Q1-Q5 (section 9).
- **Goal (Sean, 2026-10-14):** help users set up the mod and Elite's settings
  so they get a good experience on the first launch. Make it as simple and user
  friendly as possible.
- **Shape:**
  - One new installer tab, **Pre-flight**: a short checklist and one
    button, **Set up for me**.
  - A first install lands on it.
  - No wizard, no questions, no new `edvr.ini` key.
- **Phases:**
  - P1: the read-only rig card and checks, plus the EDVR keys.
  - P2: Elite's own graphics files (needs Q1).
  - P3: learn from the last flight, through `edvr_logs\flight_summary.ini`
    written by the runtime.
- **New ground:**
  - The installer would write Elite's `Options\Graphics` files for the first
    time: `Settings.xml` (`<PresetName>` only) and the Custom `.fxcfg`.
  - It never writes `DisplaySettings.xml`.
  - Writes happen only with the game closed, after a backup, and with an
    undo that restores values one at a time.
  - The runtime DLL keeps its never-write rule (`vr-supersampling-gate`).
- **Open hypotheses:** H1-H7 (section 8). Nothing is ruled out yet. Section 7
  lists what was considered and left out.
- **Next flight:** none until P2 is built. Then one sitting per rig, Quest 3
  over VDXR and Pimax over PiOpenXR, covering H1-H5 (section 8).
- **Temporary keys:** none.

## 1. Why: what goes wrong today

- **The good setup is documented but not applied.** EDVR ships with the
  upscaler off (`fix.temporal_aa = off`). A fresh install therefore runs
  Elite's native path at whatever HMD Image Quality the preset set. The setup
  that works is scattered across several places:
  - an upscaler, with HMD Image Quality below 1.0;
  - Elite's AA off;
  - Supersampling at 1.0;
  - terrain checkerboard off.

  Those live in README.md:148-161, an in-headset toast, and several Status
  blocks. The installer's only hardware test is "is there an NVIDIA adapter",
  which decides whether it places `nvngx_dlss.dll`.
- **Issues: setup and first launch, by category.**
  - Which VR runtime, and in what launch order:
    characterecho-sean/edvr-unofficial-patch#15, #19, #43, #45.
  - Confusion about Elite's own settings (HMD Quality, AA): #53, #55,
    #59, #90.
  - Other software: RivaTuner crashed the game 10 s in (#87) and its
    overlay vanishes (#74).
  - After a crash at startup, the guard switches EDVR's fixes off for the
    next launch. That launch looks "fine" and does nothing (#6, #21, #45).
  - Stale hand-edited ini files: #4 and #67. The standard advice is "start
    from a fresh `edvr.ini`".
- **The user to design for** (#45): "I just want to launch my game and play
  it."

## 2. What the user sees

The installer keeps its rule of one screen per job and no wizard
(gui.cpp:1-13). The new job, "make my first flight good", gets its own tab
between Setup and Settings:

```
  [ Setup ] [ Pre-flight ] [ Settings ]

  YOUR RIG
  ●  Graphics card         NVIDIA GeForce RTX 4070 Ti · 12 GB
  ●  VR runtime            Virtual Desktop  (change it in Virtual Desktop)
  ○  Headset               known after your first flight

  BEFORE YOU FLY                                    now        after
  [x] Upscaler             DLSS, sharper and faster   off     →  DLSS
  [x] Render size          Elite's HMD Image Quality  1.00    →  0.75
  [x] Anti-aliasing        Elite's own, not needed    SMAA    →  Off
  [x] Terrain checkerboard shimmers in VR             On      →  Off
  ▲  RivaTuner is running. It has crashed Elite with EDVR: close it first.

  [ Set up for me ]      Put my old settings back
  Elite must be closed. Everything changed is copied to edvr_backup\ first.
```

- **Each row says four things in plain words:** what it is, why (one line), the
  value now, and the value after. Where the user will meet the setting in
  Elite's own menu, the row uses Elite's name for it ("HMD Image Quality").
- **The flow on a first install:**
  1. Install, then Done.
  2. The window switches to Pre-flight with every row ticked.
  3. **Set up for me** shows the plan in the report pane.
  4. The same default-No confirm as Install (gui.cpp:928-941).
  5. Apply.

  That is two clicks after Install.
- **Existing installs:** the Setup tab's status card gets one more row,
  "Pre-flight". It reads "Ready to fly" (Good) or "2 things to set" (Warn), and
  clicking it switches tab.
- **Respect what the user chose:**
  - A row is ticked only while the current value is one the user never
    chose: a shipped EDVR value (it matches `edvr_install\edvr.ini.base`)
    or an Elite stock preset.
  - A value the user chose is shown but left unticked.
  - Known conflicts are the exception and stay ticked: terrain
    checkerboard on under an upscaler.
- **When Elite is running,** the Elite rows are greyed out with the line "Close
  Elite first: it saves its settings over ours when it exits." The installer
  already knows (`gameRunState`, detect.cpp:414-460).
- **After a flight (P3),** a "Your last flight" card replaces the headset
  placeholder. It shows the measured result and at most one suggestion (section
  5).
- **Put my old settings back** undoes the last Set up for me (section 4).

## 3. The checklist

The written rows (ticked by default, under the rule above):

| Row | When it shows | After | Writes | Evidence |
|---|---|---|---|---|
| Upscaler (VR) | scope has VR | `dlss` on an RTX card with `nvngx_dlss.dll` placed; `xess` on Intel Arc; otherwise Q3 | `edvr.ini` `fix.temporal_aa` | README:139-161; ini 321-346 |
| Render size | upscaler row is on | 0.75 (Q2); never under 0.5 | fxcfg `HMDRenderTargetMultiplier` | DLSS floor 0.5 (anti-aliasing.md Status); field values 0.5-0.75; #90 |
| Anti-aliasing | upscaler row is on | Off | fxcfg `AAMode` | README:153-154 |
| Terrain checkerboard | scope has VR | Off | fxcfg `TerrainCheckerboardRenderingEnabled` | every stock VR preset ships it on and it shimmers under DLSS (`terrain_checkerboard_reader.h`) |
| Upscaler (flat) | scope has flat | same rule as VR | `edvr-flat.ini` `fix.temporal_aa` | release/README.txt |
| Supersampling | flat upscaler row is on | 1.0 | fxcfg `SSAAMultiplier` | "DLAA requires native SS"; VR holds 1.0 anyway (`vr-supersampling-gate`) |

- **Shared settings.** Elite has one graphics configuration for both 3D modes,
  so AA Off and Supersampling 1.0 apply to flat and VR alike. Both profiles
  want those values, so there is no conflict.
- **RTX test.** The test is the DXGI adapter name containing "RTX". A wrong
  guess costs quality, not a crash: without NGX, EDVR falls back to its own TAA
  and logs it (ini 321-323).

The advice rows have no checkbox and write nothing:

| Row | Tone | Says | Detected by |
|---|---|---|---|
| Graphics card | info | name, VRAM, vendor; drives the upscaler row | DXGI: vendor id, `Description`, `DedicatedVideoMemory` |
| VR runtime | Good, or Bad if missing | the active OpenXR runtime's name, and where to change it: the runtime's own app | `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` (64-bit view), or `XR_RUNTIME_JSON` when set; name from the manifest's `runtime.name`; Bad when unset or the manifest is missing |
| RivaTuner | Warn | "It has crashed Elite with EDVR: close it before you start Elite." | `RTSS.exe` in a process snapshot |

These are the post-flight rows (P3, read from `flight_summary.ini`, section 5):

| Row | Tone | Says |
|---|---|---|
| Startup guard | Bad | "Elite crashed while EDVR was starting, so EDVR switched its fixes off for safety. Press Save logs and ask on Discord." |
| Render size step | Warn or info | "Last flight: 61 fps of 90, 31% of frames late. Lower the render size to 0.65?" One click, one step. |
| Flat post chain | Warn | Only when the flat pass refused frames for the post chain. It offers Bloom, Depth of Field or AA off, whichever the stand-down named. This is the same evidence gate `flat_elite_settings.h` uses. |
| Pimax Parallel Projection | info | "Turn Parallel Projection off in Pimax Play: it cut GPU time by up to 30%." (README:50-57) |

## 4. Writing Elite's settings (P2)

- **Files:** only `Settings.xml` (`<PresetName>`) and the highest
  `Custom.<major>.<minor>.fxcfg`, found the way `elite_graphics_folder.h` finds
  them.
- **Never:**
  - `DisplaySettings.xml`. An in-memory hold once corrupted the saved file
    (`vr_display_config_guard.h`).
  - `GraphicsConfigurationOverride.xml`.
  - AppConfig.
- **A stock preset becomes Custom.** If `<PresetName>` names a stock preset,
  Custom is seeded from `<game>\OptionDefaults\<Preset>.fxcfg` and
  `<PresetName>` is set to `Custom`, which is what Elite does when a player
  changes one setting under a preset (H2). The row text says Elite will show
  "Custom" afterwards.
- **The edit rule** is a pure function:
  - It replaces the text of an element that already exists.
  - That element must occur exactly once; otherwise the row is refused
    with "Elite's file is not in the shape EDVR knows; nothing changed".
  - It never inserts or reformats anything; bytes outside the element are
    untouched.

  This is the live-ini discipline of AGENTS.md, applied to Elite's files, with
  a rig.
- **Gate:** `gameRunState` must be NotRunning; Unknown blocks too. The
  installer already refuses every Elite exe but the pinned one
  (`kQualifiedEliteSha256`), so the file version it edits is the one that build
  reads.
- **Machinery:** the existing plan, preview, confirm and apply path. The steps
  are Backup and WriteText with `expectSha`. It brings the stale-plan guard,
  atomic writes, rollback, and a `--dry-run` that writes nothing; the self-test
  asserts that.
- **Backup:** each file goes to `edvr_backup\elite-settings-<stamp>\` and into
  the `%LOCALAPPDATA%\EDVR\` mirror.
- **Undo restores values, not files:**
  - `state.ini` gains a `[preflight]` section listing file, key or element,
    before, and after.
  - **Put my old settings back** writes `before` only where the value is
    still `after`.
  - Anything the user changed since is left alone and listed. A file
    restore would throw away their in-game changes.
- **Proton and Wine:** the folder may not be where Windows puts it
  (unified-install H5). When it is absent, the Elite rows say "Elite's settings
  were not found", and nothing is written.

## 5. Learning from the first flight (P3)

The installer cannot know the headset or its per-eye size before a flight. The
runtime learns both, and the installer can read them afterwards.

- **The runtime writes `edvr_logs\flight_summary.ini`** atomically, every 30 s
  and at shutdown, one `key=value` per line. It holds:
  - `version`, `build`, `profile`, `headset_key`, `eye_size`,
    `refresh_hz`;
  - `frames`, `late_frames`, `fps`;
  - `upscaler` (and any fallback reason), `hmd_quality_read`,
    `terrain_checkerboard_read`;
  - `startup_guard_tripped`, `flat_standdown`.
- **Why a file and not log parsing.** The installer is C++ and `edvr_log.py` is
  Python. AGENTS.md wants a parser that drifts from its format to fail in the
  build. So the format is one versioned file with one fixture:
  - the runtime's rig writes it;
  - the installer's rig reads it;
  - `tools\edvr_log.py` gains a reader under its `--self-test`.
- **Right build first.** A summary from another build than the one installed
  shows "Your last flight was before this update" and offers nothing. A stale
  log is not evidence.
- **If the writer never ran,** there is no file, and the card says "No flight
  with this build yet". That is distinct from a flight that went well.
- **The suggestion rule (Q4):**
  - one step of 0.05 or 0.1 per flight, toward most frames on time at the
    headset's refresh;
  - never below 0.5 and never above 1.0 (DLAA);
  - constants live in code, not in the ini.

  It is a hypothesis (H6) until a flight shows that the late-frame share tracks
  the judder a pilot sees. Which existing trace lines carry fps and late frames
  in a default-settings log (`native_frame_cycle_window`,
  `native_pacing_summary`) is checked with `edvr_log.py` before the writer is
  built.
- The rig card then shows the headset, e.g. "Quest 3 via Virtual Desktop,
  3072×3264 per eye".

## 6. Implementation map

- **`detect.cpp`:**
  - `gpuAdapters()` replaces `nvidiaAdapterPresent`, keeping the
    `nvngx_dlss.dll` decision intact.
  - `activeOpenXrRuntime()`.
  - `processRunning(L"RTSS.exe")`.
- **New `elite_settings.{h,cpp}`:** locate the files and read the values. Pure
  `setElement(text, tag, value) -> optional<text>`.
- **New `preflight.{h,cpp}`:**
  - pure `recommend(survey, rig, elite, flight, scope) -> rows`;
  - `planPreflight(rows, ticked) -> Plan`;
  - `planPreflightUndo(state) -> Plan`.
- **`gui.cpp`:**
  - the third tab, whose rows reuse the status-row drawing and the
    settings list's checks;
  - the switch to the tab after a first install;
  - the Setup status row.
  - The checklist must fit the fixed 720x688 window without scrolling,
    about 8 rows.
- **`app.cpp`:**
  - `--preflight` prints the checklist;
  - `--preflight --apply [--dry-run]`;
  - `--preflight --undo`.
- **`state.cpp`:** the `[preflight]` section.
- **The runtime (P3):** the `flight_summary.ini` writer, plus one shared header
  with the key names.
- **Gates:**
  - a rig for the pure functions (edit rule, recommend, plan, undo,
    dry-run writes nothing), added to `build.bat`;
  - the summary fixture in `edvr_log.py --self-test`.
- **Docs:**
  - a section in `installer.md`;
  - a short "First flight" section in README pointing at the tab.

## 7. Considered and left out

- **A GPU-tier to render-size table.** There is no evidence across tiers; most
  field data is from 5090s. The headset's per-eye size varies by roughly 3x for
  the same Quest (Link against Virtual Desktop), so the flight measures it
  instead.
- **Asking the user "smooth or sharp?".** That is one more decision for a new
  user, and the post-flight step reaches the same place with evidence. It can
  be added later as a slider.
- **Starting the OpenXR runtime from the installer** to learn the headset
  before a flight. It would start SteamVR or Link unasked.
- **Switching the system's active OpenXR runtime.** That is system-wide,
  changes every other VR game and needs admin. The runtime's own app is the
  right place, and the row says where.
- **Bloom and Depth of Field off before any flight.** `flat_elite_settings.h`
  gates on a refusal on purpose: settings-only rules fire on combinations that
  work.
- **Shadows, terrain, textures, AO, FOV.** The docs are silent, and there is no
  evidence they matter to EDVR.
- **Sharpening, panel distance, panel curvature.** These are preferences, and
  the Settings tab already offers "try 0.7".
- **ReShade, OpenXR Toolkit and other implicit OpenXR layers.** The evidence is
  mixed or absent, so they go in Save logs instead.
- **A USB wheel (#45) or SteamVR already running (#15).** Each has a single
  report and no confirmed mechanism.

## 8. Hypotheses, and the sitting that settles them

- **H1 open:** Elite keeps values written into a Custom fxcfg while it is
  closed, across a launch and exit. Evidence: the file after exit carries them;
  EDVR's log reads the new HMD multiplier (the `texture_lod_bias auto` line);
  and no checkerboard notice fires.
- **H2 open:** a stock `OptionDefaults\<Preset>.fxcfg` copied to Custom is
  complete, and Elite does not reset it. Evidence: diff `VRHigh.fxcfg` against
  a Custom file the game wrote after one menu change; no tag resets after a
  flight.
- **H3 open:** `<PresetName>Custom` written to `Settings.xml` survives a launch
  and exit.
- **H4 open:** `ActiveRuntime` names the same runtime as the log's
  `headset_key`, on Quest 3 over VDXR and Pimax over PiOpenXR.
- **H5 open:** the starting point runs at least as many frames on time as stock
  settings. The starting point is the upscaler on, 0.75, AA off and
  checkerboard off. Evidence: the frame-cycle fps and late frames of both
  launches.
- **H6 open (P3):** the late-frame share tracks the judder the pilot sees.
- **H7 open:** where the `Options\Graphics` folder lives on Epic, Frontier and
  Proton. This is the same question as unified-install H5.
- **The sitting, per rig:**
  1. Launch on stock settings and fly a fixed route for 3 minutes, then
     exit.
  2. Press **Set up for me**.
  3. Launch, fly the same route for 3 minutes, then exit and read both
     logs.
  4. Press **Put my old settings back** and diff the files.

  One sitting per rig covers H1-H5.

## 9. Decisions for Sean

- **Q1:** may the installer write Elite's `Options\Graphics` files
  (`Settings.xml` `<PresetName>` and the Custom fxcfg)? The conditions are the
  game closed, a backup first and an undo that restores values. Recommended:
  yes; the runtime's never-write rule stays as it is.
- **Q2:** the starting render size with an upscaler:
  - 0.75 for everyone (recommended);
  - a GPU-tier table;
  - leave Elite's value alone.
- **Q3:** the upscaler on GPUs other than NVIDIA. Intel Arc gets `xess`. For
  AMD and the rest:
  - `fsr` (recommended), with the row noting it is not yet checked on AMD
    (`fsr-upscaler-design` Status);
  - `on`, EDVR's own TAA.
- **Q4:** after a flight:
  - suggest one step with one click (recommended);
  - or apply it automatically at the next install.
- **Q5:** the names. The tab is "Pre-flight", the button "Set up for me", and
  the undo "Put my old settings back".
