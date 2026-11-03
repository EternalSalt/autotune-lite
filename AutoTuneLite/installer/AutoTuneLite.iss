; Inno Setup script for AutoTune Lite (64-bit Windows, VST3 for FL Studio and other hosts).
; Build it with:  ISCC.exe installer\AutoTuneLite.iss     (needs Inno Setup 6.3 or newer)
; The plugin must already be built:  cmake --build build --config Release

#ifndef MyAppVersion
  #define MyAppVersion "0.2.0"
#endif
#define MyAppName      "AutoTune Lite"
#define MyPublisher    "YourName"
#ifndef BuildDir
  #define BuildDir     "..\build\AutoTuneLite_artefacts\Release"
#endif

[Setup]
AppId={{AE60340E-1D37-4492-9EAC-E61A67E35C76}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyPublisher}
DefaultDirName={autopf}\{#MyAppName}
DisableProgramGroupPage=yes
DisableDirPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=Output
OutputBaseFilename=AutoTuneLite-Setup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={uninstallexe}

[Messages]
FinishedLabel=AutoTune Lite is installed.%n%nNext step in FL Studio: Options > Manage plugins > Find installed plugins. Then click an empty effect slot on a mixer track and choose AutoTune Lite.%n%nIf FL Studio was open, close it and reopen it first.
FinishedLabelNoIcons=AutoTune Lite is installed.%n%nNext step in FL Studio: Options > Manage plugins > Find installed plugins. Then click an empty effect slot on a mixer track and choose AutoTune Lite.%n%nIf FL Studio was open, close it and reopen it first.

[Tasks]
Name: "standalone"; Description: "Also install the Standalone test app (microphone test, no piano roll capture)"; Flags: unchecked

[Files]
; The VST3 is a folder bundle. This is where FL Studio and other hosts look for VST3 plugins.
Source: "{#BuildDir}\VST3\AutoTune Lite.vst3\*"; DestDir: "{commoncf64}\VST3\AutoTune Lite.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildDir}\Standalone\AutoTune Lite.exe"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist; Tasks: standalone
Source: "..\README.md"; DestDir: "{app}"; DestName: "README.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\AutoTune Lite (Standalone)"; Filename: "{app}\AutoTune Lite.exe"; Tasks: standalone
Name: "{autoprograms}\AutoTune Lite README"; Filename: "{app}\README.txt"

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\AutoTune Lite.vst3"
