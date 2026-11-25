# T-MU Plugins

**Melt Channel**, **Melt Stutter** and **Melt Deck** for computers: VST3 and AAX on
Windows; VST3, Audio Unit and AAX on Mac (Apple chip and Intel). These are the same
engines and screens as the MPC versions.

| Plugin | Type | What it is |
|---|---|---|
| **Melt Channel** | effect | Tube preamp → EQ → opto leveler → master with VU meters |
| **Melt Stutter** | effect | Capture a vocal word and stutter it in time, with chorus, phone and backverb. **Drag the SAVE WAV button straight into your DAW.** |
| **Melt Deck** | instrument | 1 or 2 DJ decks: sync, loops, hot cues, scratch jog, FX pads, browser for your MP3/WAV files |

## Download

Open **[Releases](../../releases/latest)** and download:

- **Mac:** `T-MU-Plugins-Mac.pkg`
- **Windows:** `T-MU-Plugins-Windows-Setup.exe`

### Mac, the first time
The installer isn't signed by Apple yet, so macOS blocks it the first time you open it.
1. Double-click the `.pkg`. When macOS says it can't be opened, click **Done**.
2. Open **System Settings → Privacy & Security**, scroll down and click **Open Anyway**.
3. Install. The plugins go to the standard plug-in folders. Restart your DAW. In Logic, the
   Audio Units are listed under **T-MU**.

### Windows, the first time
Windows may show "Windows protected your PC". Click **More info → Run anyway**.
The plugins go to `C:\Program Files\Common Files\VST3` (and `Avid\Audio\Plug-Ins` for AAX).

## Pro Tools (AAX)
Pro Tools only loads AAX plugins that are signed through Avid and PACE. Until these are
signed, they load in **Pro Tools Developer** only. To sign them, email Avid with the subject
**"PACE AAX Code Signing Tools Request"** (the address is on PACE's page *Getting started with
AAX code signing for Pro Tools plugins*). Signing is free for registered developers and needs an iLok.

## Where things are kept
Each plugin has a folder in **Music → T-MU**:
- **Melt Stutter** saves `Stutter 001.wav`… in `Music/T-MU/Melt Stutter`.
- **Melt Deck** finds a folder called **Melt Deck Music** in your Music, Desktop, Documents
  or Downloads folder, or at the top of any drive. Its **BROWSE** page can also open any folder.

## Building it yourself
Push to `main` and GitHub Actions builds everything (`.github/workflows/build.yml`).
To build locally you need CMake 3.22+ and Xcode (Mac) or Visual Studio 2022 (Windows):

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The plugin engines in `plugins/` are the same files as the MPC versions (MPC Plugin SDK).
`shim/` lets them run under JUCE, and `shared/` draws each plugin's MPC touchscreen skin.

## License
AGPL-3.0 (see `LICENSE`), because these plugins are built with JUCE under its open-source
licence. `shim/` contains code from the MPC Plugin SDK (MIT, `shim/LICENSE-mpc-sdk.txt`), and the
interface font is Inter (SIL OFL, `shared/fonts/Inter-LICENSE.txt`).
