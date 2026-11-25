; T-MU Plugins installer for Windows (Inno Setup)
#ifndef BuildNum
  #define BuildNum "0"
#endif
[Setup]
AppName=T-MU Plugins
AppVersion=1.0.{#BuildNum}
AppPublisher=T-MU
DefaultDirName={commonpf64}\T-MU
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=T-MU-Plugins-Windows-Setup
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
PrivilegesRequired=admin
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Types]
Name: "full"; Description: "VST3 and AAX"
Name: "custom"; Description: "Choose"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 (FL Studio, Ableton, Studio One, Reaper...)"; Types: full custom
Name: "aax"; Description: "AAX (Pro Tools)"; Types: full

[Files]
Source: "..\build\MeltChannel_artefacts\Release\VST3\Melt Channel.vst3\*"; DestDir: "{commoncf64}\VST3\Melt Channel.vst3"; Components: vst3; Flags: recursesubdirs ignoreversion
Source: "..\build\MeltStutter_artefacts\Release\VST3\Melt Stutter.vst3\*"; DestDir: "{commoncf64}\VST3\Melt Stutter.vst3"; Components: vst3; Flags: recursesubdirs ignoreversion
Source: "..\build\MeltDeck_artefacts\Release\VST3\Melt Deck.vst3\*"; DestDir: "{commoncf64}\VST3\Melt Deck.vst3"; Components: vst3; Flags: recursesubdirs ignoreversion
Source: "..\build\MeltChannel_artefacts\Release\AAX\Melt Channel.aaxplugin\*"; DestDir: "{commoncf64}\Avid\Audio\Plug-Ins\Melt Channel.aaxplugin"; Components: aax; Flags: recursesubdirs ignoreversion
Source: "..\build\MeltStutter_artefacts\Release\AAX\Melt Stutter.aaxplugin\*"; DestDir: "{commoncf64}\Avid\Audio\Plug-Ins\Melt Stutter.aaxplugin"; Components: aax; Flags: recursesubdirs ignoreversion
Source: "..\build\MeltDeck_artefacts\Release\AAX\Melt Deck.aaxplugin\*"; DestDir: "{commoncf64}\Avid\Audio\Plug-Ins\Melt Deck.aaxplugin"; Components: aax; Flags: recursesubdirs ignoreversion
