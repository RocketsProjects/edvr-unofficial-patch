# Pre-flight: the installer sets up EDVR and Elite for a good first launch (design, 2026-10-14)

## Status

- **State (2026-10-14):** P1 is being built on `claude/vibrant-maxwell-9gehb1`.
  Nothing has flown. Sean answered Q1-Q6 on 2026-10-14 (section 10); Q7 and Q8
  are open.
- **Goal (Sean, 2026-10-14):** help users set up the mod and Elite's settings
  so they get a good experience on the first launch. Make it as simple and user
  friendly as possible.
- **Shape:**
  - One new installer tab, **Pre-flight**: a short checklist and one
    button, **Set up for me**.
  - A first install lands on it.
  - No wizard, no questions, no new `edvr.ini` key.
- **Decided (Sean, 2026-10-14):**
  - Q1: the installer may write Elite's graphics files.
  - Q2: the render size scales with the headset and the graphics card
    (section 4).
  - Q3: DLSS on NVIDIA, FSR on AMD, XeSS on Intel.
  - Q4: after a flight it suggests the next step and never applies one by
    itself.
  - Q5: the names "Pre-flight", "Set up for me" and "Put my old settings
    back".
  - Q6: Detect my headset is offered on SteamVR too, and says first that it
    starts SteamVR.
- **Phases:**
  - P1: the rig card, the checks and the EDVR keys. Elite's rows show as
    advice, without a checkbox, until P2.
  - P1 and P2 ship together. The upscaler alone at HMD Image Quality 1.0 runs
    DLAA, which costs more than Elite's native path.
  - P2: Elite's own graphics files.
  - P3: `edvr_logs\flight_summary.ini` from the runtime, and the suggestion
    after a flight.
  - P4: the render size estimated from headset and card, through Detect my
    headset and the card speed table. It needs P3's GPU frame time to
    calibrate.
- **New ground:**
  - The installer writes `Settings.xml` (`<PresetName>` only) and the
    Custom `.fxcfg`, and never `DisplaySettings.xml`.
  - Writes happen only with the game closed, after a backup, and with an
    undo that restores values one at a time.
  - The runtime DLL keeps its never-write rule (`vr-supersampling-gate`).
- **Open hypotheses:** H1-H12 (section 9). Nothing is ruled out yet. Section 8
  lists what was considered and left out.
- **Next flight:** none until P2 is built. Then one sitting per headset on the
  5090, Quest 3 over VDXR and Pimax over PiOpenXR (section 9).
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
  ●  Headset               Meta Quest 3 · 3072×3264 per eye  [Detect my headset]

  BEFORE YOU FLY                                    now        after
  [x] Upscaler             DLSS, sharper and faster   off     →  DLSS
  [x] Render size          Elite's HMD Image Quality  1.00    →  0.70
                           sized for this card and headset at 90 Hz
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

  That is two clicks after Install. **Detect my headset** is optional: it
  sharpens the render-size estimate (section 4).
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
- **After a flight (P3),** a "Your last flight" card shows the measured result
  and at most one suggestion (section 6).
- **Put my old settings back** undoes the last Set up for me (section 5).

## 3. The checklist

The written rows (ticked by default, under the rule above):

| Row | When it shows | After | Writes | Evidence |
|---|---|---|---|---|
| Upscaler (VR) | scope has VR | `dlss` on NVIDIA, `fsr` on AMD, `xess` on Intel (Q3) | `edvr.ini` `fix.temporal_aa` | README:139-161; ini 321-346 |
| Render size | upscaler row is on | sized for the headset and card (section 4); 0.75 when either is unknown | fxcfg `HMDRenderTargetMultiplier` | DLSS floor 0.5 (anti-aliasing.md Status); field values 0.5-0.75; #90 |
| Anti-aliasing | upscaler row is on | Off | fxcfg `AAMode` | README:153-154 |
| Terrain checkerboard | scope has VR | Off | fxcfg `TerrainCheckerboardRenderingEnabled` | every stock VR preset ships it on and it shimmers under DLSS (`terrain_checkerboard_reader.h`) |
| Upscaler (flat) | scope has flat | same vendor rule | `edvr-flat.ini` `fix.temporal_aa` | release/README.txt |
| Supersampling | flat upscaler row is on | P1-P3: 1.0 when above 1.0; P4: sized for the card and the screen (section 4) | fxcfg `SSAAMultiplier` | "DLAA requires native SS"; it is the flat render-scale control (`design-flat-temporal-aa`); VR holds 1.0 anyway (`vr-supersampling-gate`) |
| EDVR output size | P4, two cases only (Q7) | a width for this headset's key | `edvr.ini` `fix.openxr_resolution` | section 4 |

- **Which card the vendor rule reads:** the card the headset is on. That is the
  probe's adapter LUID when Detect my headset has run, and otherwise the
  high-performance adapter. A laptop's Intel iGPU never decides for its NVIDIA
  card.
- **Fallbacks are safe:**
  - On an NVIDIA card without NGX (a GTX), `dlss` falls back to EDVR's own
    TAA and logs it (ini 321-323).
  - `xess` without `libxess.dll` falls back to spatial recovery.
- **Shared settings.** Elite has one graphics configuration for both 3D modes,
  so AA Off and Supersampling 1.0 apply to flat and VR alike. Both profiles
  want those values, so there is no conflict.

The advice rows have no checkbox and write nothing:

| Row | Tone | Says | Detected by |
|---|---|---|---|
| Graphics card | info | name, VRAM, vendor | DXGI: vendor id, `Description`, `DedicatedVideoMemory`, LUID |
| VR runtime | Good, or Bad if missing | the active OpenXR runtime's name, and where to change it: the runtime's own app | `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` (64-bit view), or `XR_RUNTIME_JSON` when set; name from the manifest's `runtime.name`; Bad when unset or the manifest is missing |
| Headset | info | system name and per-eye size, or "known after your first flight" | section 4 |
| RivaTuner | Warn | "It has crashed Elite with EDVR: close it before you start Elite." | `RTSS.exe` in a process snapshot |

These are the post-flight rows (P3, read from `flight_summary.ini`, section 6):

| Row | Tone | Says |
|---|---|---|
| Startup guard | Bad | "Elite crashed while EDVR was starting, so EDVR switched its fixes off for safety. Press Save logs and ask on Discord." |
| Render size | Warn or info | "Last flight: 61 fps of 90, 31% of frames late. Lower the render size to 0.60?" or "Room to spare: sharper at 0.85?" One click. |
| CPU limit | info | "Frames were late while the graphics card had time to spare: the processor is the limit, so a smaller render size will not help." |
| Flat post chain | Warn | Only when the flat pass refused frames for the post chain. It offers Bloom, Depth of Field or AA off, whichever the stand-down named. This is the same evidence gate `flat_elite_settings.h` uses. |
| Pimax Parallel Projection | info | "Turn Parallel Projection off in Pimax Play: it cut GPU time by up to 30%." (README:50-57) |

## 4. Render size from the headset and the graphics card (P4)

**It is possible**, from three measured numbers and two fitted constants:

- W×H, the per-eye size the VR runtime asks for: what EDVR renders at HMD Image
  Quality 1.0, scaled by `openxr_resolution` when that is set;
- Hz, the headset's refresh rate;
- S, the card's speed relative to an RTX 5090;
- a and b, Elite's fixed and per-pixel GPU cost on the 5090, fitted from the
  calibration sitting (section 9).

The model gives the GPU frame time at render size q (the per-axis scale) as:

`t(q) = (a + b · 2·W·H·q²) / S`

The estimate is the largest q, in steps of 0.05 within [0.5, 1.0], for which
`t(q) ≤ 0.85 × 1000 / Hz`. The 0.85 leaves headroom for stations and
settlements. It is a constant in code and part of H8.

**Where each number comes from**, best source first:

- **W×H and the headset's name:**
  1. This PC's last EDVR flight, from `flight_summary.ini` (section 6).
     `vscreen_auto_state.h` is the precedent: the runtime already saves
     the last per-eye width in `edvr_logs\`.
  2. **Detect my headset.** The installer asks the runtime through the
     OpenXR loader it already carries (payload 105):
     - `xrCreateInstance`, `xrGetSystem`, `xrGetSystemProperties`,
       `xrEnumerateViewConfigurationViews`, and
       `xrGetD3D11GraphicsRequirementsKHR` for the adapter LUID.
     - It creates no session, so nothing appears in the headset.
     - It runs in a child process (`edvr-installer.exe --probe-headset`),
       never elevated and killed after 20 s, so a runtime that hangs or
       crashes cannot take the installer with it.
     - It needs the headset connected and its runtime ready. A failure
       says what to do, for example "Connect your headset in Virtual
       Desktop, then press again".
     - On SteamVR the click starts SteamVR, and the button says so (Q6).
  3. Neither: 0.75, which the first flight's suggestion corrects.
- **Why the runtime's number and not a table of headset models:** the same
  Quest 3 asks for 1824×1968 over Link and 3072×3264 over Virtual Desktop
  (openxr-resolution-per-headset). Streaming apps also let users pick their own
  size. The runtime's number already includes those choices.
- **Hz:** the last flight's. The runtime already reads
  `xrGetDisplayRefreshRateFB` (native_runtime_host.h:3286), but that call needs
  a session, so the probe cannot read it. With no flight on record, the
  estimate assumes 90 and says "at 90 Hz" on the row.
- **S:** a table of relative speed per card in the installer, `gpu_speed.inc`.
  - It is generated from `tools\gpu_speed.csv` by a tool with its own
    `--self-test`, added to the `build.bat` gate.
  - Entries match the DXGI adapter name, with Laptop variants listed apart
    from desktop cards.
  - The seed figures come from public relative-performance benchmarks.
  - An unknown card gets no estimate: the row uses 0.75 and the suggestion
    after a flight corrects it.
- **a and b:** fitted on the 5090 with both headsets. The upscaler's own cost
  differs (DLSS 2.9-3.6 ms, FSR 1.6-2.1 ms on the 5090, `fsr-upscaler-design`),
  so a is kept per upscaler.

**When even 0.5 is too slow,** the row says so and names the lever outside
EDVR. It computes the refresh rate that fits at 0.5, for example: "This card is
short of this headset at 90 Hz, even at 0.5. Set Virtual Desktop to 72 Hz, or
expect reprojection."

**How good the estimate is:**

- It is a starting point.
- The first flight measures the real frame time, and the suggestion corrects
  the estimate using the same model with the measured time in place of the
  table's.
- Every Save logs bundle carries the flight summary: card, headset, size, Hz, q
  and frame time. So the speed table can be checked card by card as supporters
  fly (H10).
- A wrong guess costs one flight at a sub-optimal size, not a lasting bad
  setup.
- The estimate snaps to values Elite's own menu offers, so the menu shows what
  was written (H12).

**EDVR's output size, `fix.openxr_resolution` (Q7).** There are two sizes in
play:

- **The output:** the per-eye width EDVR hands Elite as the headset's size. It
  is `openxr_resolution` when the headset has an entry, and otherwise 100% of
  the runtime's recommendation (edvr.ini:605-634).
- **The input:** that size times HMD Image Quality. It is what Elite actually
  renders before the upscaler fills in the rest.

Elite's cost follows the input. The output sets the upscaler's own cost and how
sharp the result is. The proposal is for Pre-flight to write a width for the
headset's key (`runtime/system`, the same normalisation the runtime uses; P4
shares that code) in two cases only:

- **DLSS's limit.** NGX fails above 8192 per axis, and a Pimax 8KX with
  Parallel Projection on asks for 8268 (anti-aliasing.md:2370-2391). The entry
  caps the output under the limit.
- **A card short even at 0.5.** Lowering the output comes before telling the
  user to lower the refresh rate.

It stays empty otherwise. The runtime's number already carries the user's own
choice in Virtual Desktop, Pimax Play, SteamVR or the Meta app. The entry is a
fixed pixel width, so writing one would silently stop those choices from having
an effect, within a quarter to twice the recommendation.

**Flat: Supersampling sized for the card and the screen.** In flat, Elite's
Supersampling is the render-scale control. Below 1.0 the upscaler fills the
screen; at 1.0, DLSS runs as DLAA. The same model applies with:

- W×H being Elite's resolution, read (never written) from
  `DisplaySettings.xml`;
- one view instead of two;
- a frame-rate target in place of the headset's refresh (Q8).

The flight summary's `profile` key keeps flat flights apart from VR ones.

## 5. Writing Elite's settings (P2)

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

## 6. Learning from each flight (P3)

- **The runtime writes `edvr_logs\flight_summary.ini`** atomically, every 30 s
  and at shutdown, one `key=value` per line. It holds:
  - `version`, `build`, `profile`;
  - the card: `gpu_name`, `gpu_vram_mb`;
  - the headset: `headset_key`, `eye_size`, `refresh_hz`;
  - the frames: `frames`, `late_frames`, `gpu_ms_p50`, `gpu_ms_p90`;
  - what EDVR read and ran: `hmd_quality_read`, `upscaler` (and any
    fallback reason), `terrain_checkerboard_read`;
  - `startup_guard_tripped`, `flat_standdown`.
- **Why a file and not log parsing.** The installer is C++ and `edvr_log.py` is
  Python. AGENTS.md wants a parser that drifts from its format to fail in the
  build. So the format is one versioned file with one fixture:
  - the runtime's rig writes it;
  - the installer's rig reads it;
  - `tools\edvr_log.py` gains a reader under its `--self-test`.

  The log records the adapter's LUID but not its name, so `gpu_name` is new.
- **Right build first.** A summary from another build than the one installed
  shows "Your last flight was before this update" and offers nothing. A stale
  log is not evidence.
- **If the writer never ran,** there is no file, and the card says "No flight
  with this build yet". That is distinct from a flight that went well.
- **The suggestion (Q4)** is offered and never applied by itself:
  - **Late frames with the GPU over budget:** the section 4 model, fed the
    measured `gpu_ms_p90` at the q that flew, gives the largest q in
    budget.
  - **Late frames with the GPU under budget:** the CPU-limit row, with no
    render-size change.
  - **No late frames and a step of room or more:** "sharper at …?".

  These rules are H6. Which instrument gives the whole frame's GPU time by
  default (`native_producer_gpu_summary`'s timer is the candidate) is checked
  with `edvr_log.py` before the writer is built (H11).

## 7. Implementation map

- **`detect.cpp`:**
  - `gpuAdapters()` (vendor, name, VRAM, LUID; software adapters skipped)
    replaces `nvidiaAdapterPresent`, keeping the `nvngx_dlss.dll` decision
    intact;
  - the high-performance adapter via `EnumAdapterByGpuPreference`;
  - `activeOpenXrRuntime()`;
  - `processRunning(L"RTSS.exe")`.
- **New `elite_settings.{h,cpp}`:** locate the files and read the values. Pure
  `setElement(text, tag, value) -> optional<text>`.
- **New `headset_probe.{h,cpp}`:**
  - the `--probe-headset` child, which loads the installed
    `openxr_loader.dll`, or before an install a copy extracted to a temp
    folder and deleted afterwards;
  - the parent's launch, timeout and parse.
- **New `render_size.{h,cpp}`:** the pure model, the table lookup and the
  suggestion rules. `gpu_speed.inc` is generated from `tools\gpu_speed.csv`.
- **New `preflight.{h,cpp}`:**
  - pure `recommend(survey, rig, elite, flight, scope) -> rows`;
  - `planPreflight(rows, ticked) -> Plan`;
  - `planPreflightUndo(state) -> Plan`.
- **`gui.cpp`:**
  - the third tab, whose rows reuse the status-row drawing and the
    settings list's checks;
  - Detect my headset;
  - the switch to the tab after a first install;
  - the Setup status row.
  - The checklist must fit the fixed 720x688 window without scrolling,
    about 9 rows.
- **`app.cpp`:**
  - `--preflight` prints the checklist;
  - `--preflight --apply [--dry-run]`;
  - `--preflight --undo`;
  - `--probe-headset`.
- **`state.cpp`:** the `[preflight]` section.
- **The runtime (P3):** the `flight_summary.ini` writer, plus one shared header
  with the key names.
- **Gates:**
  - a rig for the pure functions (edit rule, recommend, render size,
    plan, undo, dry-run writes nothing), added to `build.bat`;
  - the `gpu_speed` generator's self-test;
  - the summary fixture in `edvr_log.py --self-test`.
- **Docs:**
  - a section in `installer.md`;
  - a short "First flight" section in README pointing at the tab.

## 8. Considered and left out

- **A table of headset models.** The per-eye size varies about 3x for the same
  Quest between Link and Virtual Desktop. The runtime's own number is used
  instead (section 4).
- **Asking the user "smooth or sharp?".** That is one more decision for a new
  user, and the suggestion after a flight reaches the same place with evidence.
  It can be added later as a slider.
- **Probing the runtime without a click.** That would start SteamVR or wake a
  Link session unasked.
- **Creating an OpenXR session in the probe** to read the refresh rate. It
  would show something in the headset, and the first flight reads the rate
  anyway.
- **A synthetic GPU benchmark in the installer.** It would not measure Elite's
  load, which is what the flight measures.
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

## 9. Hypotheses, and the sitting that settles them

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
  `headset_key`.
- **H5 open:** the Set up for me result runs at least as many frames on time as
  stock settings.
- **H6 open:** late frames together with `gpu_ms_p90` tell a GPU limit from a
  CPU limit.
- **H7 open:** where the `Options\Graphics` folder lives on Epic, Frontier and
  Proton. This is the same question as unified-install H5.
- **H8 open:** frame time is `a + b·pixels` to within one 0.05 step for q from
  0.5 to 1.0. That must hold on both headsets with the same card and the same a
  and b, and the 0.85 headroom must keep stations and settlements on time.
- **H9 open:** the probe, with no session, returns the same system and per-eye
  size as the flight's `headset_key` and eye size. It shows nothing in the
  headset.
- **H10 open:** the speed table puts a supporter's card within one step of what
  their flight measures. This is checked card by card from Save logs bundles,
  and it is what P4's accuracy rests on.
- **H11 open:** a default-settings log already carries the whole frame's GPU
  time.
- **H12 open:** Elite's menu offers HMD Image Quality and Supersampling in
  fixed steps. A written value on one of those steps is kept and shown by the
  menu. The steps are read from the game's own menu before P2 writes anything.
- **The sitting, per headset** (Quest 3 over VDXR and Pimax over PiOpenXR, both
  on the 5090), each flight a fixed 3-minute route that passes a station:
  1. Press **Detect my headset** and note what it reports (H4, H9).
  2. Launch on stock settings, fly, exit.
  3. Press **Set up for me**, then launch, fly, exit (H1-H3, H5).
  4. Launch at 0.5, then at 1.0, set from Elite's menu, flying each. All
     four flights give `gpu_ms_p90` at four sizes, which fits a and b and
     tests H8.
  5. Press **Put my old settings back** and diff the files.

  That is four launches per headset. H6, H10 and H11 are read from the same
  logs.

## 10. Decisions for Sean

- **Q1 decided (Sean, 2026-10-14):** yes. The installer may write
  `Settings.xml` `<PresetName>` and the Custom fxcfg under the section 5 rules.
- **Q2 decided (Sean, 2026-10-14):** the render size scales with the headset
  and card combination (section 4).
- **Q3 decided (Sean, 2026-10-14):** DLSS for NVIDIA, FSR for AMD, XeSS for
  Intel.
- **Q4 decided (Sean, 2026-10-14):** after a flight, suggest the next step.
- **Q5 decided (Sean, 2026-10-14):** "Pre-flight", "Set up for me", "Put my old
  settings back".
- **Q6 decided (Sean, 2026-10-14):** allow Detect my headset on SteamVR; the
  button says first that it starts SteamVR.
- **Q7 open:** should Pre-flight write `fix.openxr_resolution` only in the two
  cases of section 4 (recommended), or always to a target size per headset?
  "Always" needs a table of panel sizes per headset. It also overrides the
  user's streaming-quality choice.
- **Q8 open:** the frame-rate target for sizing flat Supersampling.
  Recommended: 60 fps, or the monitor's refresh when that is lower.
