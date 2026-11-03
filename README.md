# AutoTune Lite  v0.2  (VST3 pitch correction with a piano-roll editor)

Real-time monophonic pitch correction for vocals with two ways of working that share one engine:

* **Live mode**: pick a key and scale and the voice snaps to the nearest allowed note as it arrives.
  Works on live input and on recorded tracks, exactly like v0.1.
* **Edit mode**: a piano roll. The plugin remembers the pitch of your vocal, turns it into notes, and you
  drag notes to other pitches, split/merge them, or switch correction off per note. Playback applies your edits.

You can sing live through it **and** edit afterwards. Nothing about Live mode was removed.

## Windows installer (64-bit, for FL Studio)

The project can produce a normal `AutoTuneLite-Setup-0.2.0.exe` that installs the plugin to
`C:\Program Files\Common Files\VST3\` (where FL Studio looks), adds an Uninstall entry, and optionally installs a
Standalone test app. Two ways to get that installer:

**Option 1: build it on your own PC (one double-click).** Install Visual Studio 2022 ("Desktop development with
C++"), CMake, Git and [Inno Setup 6.3+](https://jrsoftware.org/isdl.php), then double-click `build_installer.bat`.
The installer appears in `installer\Output\`.

**Option 2: let GitHub build it for you (no Visual Studio needed).** Create a free GitHub account and a new
repository, upload the contents of this folder (including the hidden-looking `.github` folder), open the
**Actions** tab, choose **Build Windows installer**, click **Run workflow**, and after about 10-15 minutes download
`AutoTuneLite-Windows-Installer` from the finished run. If the workflow does not show up, use *Add file > Create new
file*, name it `.github/workflows/build-windows.yml`, and paste in the file of that name from this project.

Installing: close FL Studio, run the installer (click Yes at the administrator prompt), then in FL Studio use
**Options > Manage plugins > Find installed plugins**. The installer is not code-signed, so Windows SmartScreen may
say "Windows protected your PC": click **More info > Run anyway**.

Before you publish or share it, replace `YourName` in `CMakeLists.txt` (COMPANY_NAME) and
`installer/AutoTuneLite.iss` (MyPublisher).

## Build manually (Windows, without the installer)

1. Install **Visual Studio 2022 Community** ("Desktop development with C++"), **CMake** and **Git**.
2. In a *Developer PowerShell for VS 2022*, inside this folder:

```
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

3. Copy `build\AutoTuneLite_artefacts\Release\VST3\AutoTune Lite.vst3` to
   `C:\Program Files\Common Files\VST3\` (needs admin).
4. FL Studio: **Options > Manage plugins > Find installed plugins**, then load **AutoTune Lite** on a mixer insert.

## Workflow

**A. You already have a recorded vocal**
1. Route the vocal to a mixer track with the plugin. Mode = **Live**, Key/Scale = your song.
2. Click **Capture** (it turns red), then play the project from before the vocal to after it.
   The plugin is tuning live while it listens.
3. Stop. Notes appear in the piano roll automatically ("auto-detect" runs when the pass ends).
4. Switch **Mode to Edit**, adjust notes, press play. Your edits are what you hear.

**B. You are recording a new vocal**
Mode = Live, **Capture** on, record as usual: you hear the live tuning, and the pitch is captured at the same
time. Afterwards switch to Edit mode to polish.

**C. Just want live tuning**
Leave Mode on Live. Ignore the piano roll (the red dot on its keyboard shows the pitch you are singing right now).

Edits are tied to the **host timeline**. If you move the audio clip in the playlist afterwards, the notes no
longer line up: re-capture (Mode = Live, Capture on, play it through again). Capture is intentionally paused
while Mode = Edit so playing back can never overwrite your edits.

## The piano roll

| Action | How |
|---|---|
| Move a note's pitch | Drag it up/down. Pick *Drag snaps: Semitone / Scale / Free (cents)* in the top bar. You hear it while dragging. |
| Fine nudge | Select, then Up/Down arrows (1 semitone), Shift+Up/Down (10 cents) |
| Select | Click; Shift/Ctrl+click to add; drag on empty space for a box; Ctrl+A for all |
| Correction on/off for a note | Double-click it, or Delete. Grey notes are left exactly as sung. |
| Split / merge / snap / reset | Right-click a note |
| Undo / redo | Ctrl+Z, Ctrl+Y (or the buttons). A whole drag is one step. |
| Zoom / scroll | Wheel = scroll time, Ctrl+wheel = zoom time, Shift+wheel = scroll pitch, Alt+wheel = zoom pitch, **Fit** button |
| Playhead | Red line; **Follow playhead** keeps it in view |

How to read a note: the solid block sits at the pitch it **will** be (target). A faint outline marks where it was
actually sung. The thin blue line is the captured pitch curve.

Buttons: **Detect Notes** rebuilds all notes from the captured pitch (throws away your edits; Undo brings them
back). **Snap All To Scale** moves every target to the nearest note of the Key/Scale.

## Controls

| Control | What it does |
|---|---|
| **Mode** | Live (scale snapping) or Edit (your notes). In Edit mode, anywhere without a note, or when the transport is stopped, the plugin falls back to Live tuning. |
| **Key / Scale** | Notes allowed in Live mode, and the starting targets when notes are detected. |
| **Retune** | Speed of pitch changes. `0 ms` = instant robotic effect, `15-40 ms` = pop, `100+` = subtle. In Edit mode it smooths the jumps between notes. |
| **Amount** | 100 % full correction, 0 % none. |
| **Drift corr.** | Edit mode: 100 % flattens wobble inside a note (clean), 0 % keeps your natural drift and vibrato and only moves the note as a whole. |
| **Transpose** | Extra shift in semitones (+-12). |

## Limits

* Monophonic: one voice per instance.
* Latency about 35 ms (reported to FL Studio so recorded tracks line up). For live singing use direct monitoring;
  to reduce it raise `kMinPitchHz` at the top of `Source/AutoTuneCore.h`.
* Tracks about 85 Hz - 1 kHz. Capture holds up to 20 minutes per instance.
* Needs the host to report its playback position (FL Studio does for mixer effects). In the Standalone app there
  is no host timeline, so Capture/Edit are unavailable there. The top bar says so when it happens.
* Edits are saved inside the FL project (notes and the captured pitch), and restored when you reopen it.
  The Capture button is deliberately not saved: it always starts off.
* Corrections larger than about 7 semitones start to sound artificial. No formant control.

## What was and wasn't verified

Verified by compiling and running tests (`tests/`, no JUCE needed):
* Live engine: pitch accuracy ~0.1 cent, vibrato flattening, scales, loudness, no clicks, noise passthrough,
  any block size, 44.1/48/96 kHz (`offline_test.cpp`).
* Piano roll model and edit playback (`edit_test.cpp`): note detection (legato splits, dropouts bridged,
  blips ignored), split/merge, re-detecting only a changed region while keeping edits elsewhere, notes keyed to a
  host timeline that does not start at zero, dragging a note moves the output to the new pitch (within 1 cent),
  bypassed notes are untouched, drift correction 100 % vs 0 %, fallbacks to live tuning, block-size independence.
* Processor logic (undo/redo, auto-detect, sample-rate change) was exercised against a small stand-in for JUCE.

**Not verified:** the JUCE wrapper, the piano-roll drawing/mouse code and the CMake file have only been
type-checked against that stand-in. They have never been compiled against real JUCE, and nothing has been loaded in FL
Studio. Expect that a first build might need a small fix, and that mouse feel/layout could need tuning. Send me any
compiler error or behaviour that feels wrong and I'll fix it.

Run the engine tests any time:
```
g++ -O2 -std=c++17 -ISource tests/offline_test.cpp -o offline_test && ./offline_test
g++ -O2 -std=c++17 -ISource tests/edit_test.cpp    -o edit_test    && ./edit_test
```
