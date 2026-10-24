#define MyAppName "Game Audio Bridge"
#define MyAppVersion "2.5 experimental"
#define MyAppPublisher "Game Audio Bridge contributors"
[Setup]
AppId={{4D8D6B67-1C79-48A8-8A4C-AB4A9C4D4D0F}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\Programs\Game Audio Bridge
DefaultGroupName=Game Audio Bridge
OutputDir=..\Output
OutputBaseFilename=GameAudioBridge-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64
PrivilegesRequired=lowest
UninstallDisplayIcon={app}\GameAudioBridge.exe
[Files]
Source: "..\dist\GameAudioBridge.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\GameAudioBridge.ini"; DestDir: "{app}"; Flags: onlyifdoesntexist
Source: "..\dist\GameAudioBridgeAsio.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\dist\x64\*"; DestDir: "{app}\x64"; Flags: ignoreversion
Source: "..\dist\x86\*"; DestDir: "{app}\x86"; Flags: ignoreversion
Source: "..\dist\README.txt"; DestDir: "{app}"; Flags: ignoreversion
[Icons]
Name: "{group}\Game Audio Bridge"; Filename: "{app}\GameAudioBridge.exe"
Name: "{group}\Game Audio Bridge instructions"; Filename: "{app}\README.txt"
Name: "{group}\Uninstall Game Audio Bridge"; Filename: "{uninstallexe}"
[Run]
Filename: "{app}\GameAudioBridge.exe"; Description: "Launch Game Audio Bridge"; Flags: postinstall nowait skipifsilent
