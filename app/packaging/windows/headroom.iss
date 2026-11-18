; Headroom for Windows: Inno Setup 6 script.
;
; Input is the `cmake --install` tree (packaging/Install.cmake layout: Headroom.exe + app\{ui,presets,resources,...})
; and, optionally, the Headroom.vst3 bundle from app/plugin.
;
;   cmake --install build-win --prefix stage
;   iscc /DStage=..\..\..\stage /DVersion=0.9.0 [/DVst3=path\to\Headroom.vst3] ^
;        [/DWebView2Bootstrapper=path\to\MicrosoftEdgeWebview2Setup.exe] ^
;        [/DSign "/Sheadroomsign=signtool.exe sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 /a $f"] ^
;        app\packaging\windows\headroom.iss
;
; Licences: Inno Setup's licence allows commercial use ("for any purpose, including commercial applications");
; jrsoftware.org asks commercial users (> USD 5000/yr revenue) to buy a licence, "not strictly required".
; The WebView2 Evergreen bootstrapper may be redistributed per Microsoft's WebView2 distribution terms.
; ASIO needs no runtime component: drivers come with the user's interface.

#ifndef Version
  #define Version "0.9.0"
#endif
#ifdef Sign
  #define SignFlag "signonce"
#else
  #define SignFlag ""
#endif
#ifndef Stage
  #define Stage "..\..\..\stage"
#endif

[Setup]
AppId={{6B0E8C1E-6E7A-4E43-9A57-48C2C1A9D0F1}
AppName=Headroom
AppVersion={#Version}
AppPublisher=Headroom
DefaultDirName={autopf}\Headroom
DefaultGroupName=Headroom
DisableProgramGroupPage=yes
OutputBaseFilename=Headroom-{#Version}-Windows-x64-Setup
OutputDir=..\..\..\dist
SetupIconFile=Headroom.ico
UninstallDisplayIcon={app}\Headroom.exe
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
PrivilegesRequired=admin
WizardStyle=modern
#ifdef Sign
SignTool=headroomsign
SignedUninstaller=yes
#endif

[Types]
Name: "full"; Description: "Headroom and the VST3 plugin"
Name: "app"; Description: "Headroom only"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "app"; Description: "Headroom (standalone)"; Types: full app custom; Flags: fixed
#ifdef Vst3
Name: "vst3"; Description: "VST3 plugin (Common Files\VST3)"; Types: full custom
#endif

[Files]
Source: "{#Stage}\Headroom.exe"; DestDir: "{app}"; Components: app; Flags: ignoreversion {#SignFlag}
Source: "{#Stage}\*.dll"; DestDir: "{app}"; Components: app; Flags: ignoreversion skipifsourcedoesntexist {#SignFlag}
Source: "{#Stage}\app\*"; DestDir: "{app}\app"; Components: app; Flags: ignoreversion recursesubdirs createallsubdirs
#ifdef Vst3
; Steinberg's documented 64-bit location: C:\Program Files\Common Files\VST3\<Name>.vst3 (a bundle folder).
Source: "{#Vst3}\*"; DestDir: "{commoncf64}\VST3\Headroom.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs {#SignFlag}
#endif
#ifdef WebView2Bootstrapper
Source: "{#WebView2Bootstrapper}"; DestDir: "{tmp}"; DestName: "MicrosoftEdgeWebview2Setup.exe"; Flags: deleteafterinstall; Check: NeedsWebView2
#endif

[Icons]
Name: "{autoprograms}\Headroom"; Filename: "{app}\Headroom.exe"
Name: "{autodesktop}\Headroom"; Filename: "{app}\Headroom.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Run]
#ifdef WebView2Bootstrapper
Filename: "{tmp}\MicrosoftEdgeWebview2Setup.exe"; Parameters: "/silent /install"; StatusMsg: "Installing Microsoft Edge WebView2 Runtime..."; Check: NeedsWebView2; Flags: waituntilterminated
#endif
Filename: "{app}\Headroom.exe"; Description: "Launch Headroom"; Flags: nowait postinstall skipifsilent

[Code]
// WebView2 Evergreen runtime present? (Windows 11 ships it; Windows 10 usually has it via Edge.)
// https://learn.microsoft.com/microsoft-edge/webview2/concepts/distribution#detect-if-a-webview2-runtime-is-already-installed
function WebView2Version(Root: Integer; Key: String): String;
begin
  if not RegQueryStringValue(Root, Key, 'pv', Result) then Result := '';
end;

function NeedsWebView2(): Boolean;
var v: String;
begin
  v := WebView2Version(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}');
  if (v = '') or (v = '0.0.0.0') then
    v := WebView2Version(HKCU, 'Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}');
  Result := (v = '') or (v = '0.0.0.0');
end;
