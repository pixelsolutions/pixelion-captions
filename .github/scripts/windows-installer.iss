; Inno Setup 6 script for the Pixelion Captions OBS plugin (Windows x64).
;
; Required iscc defines:
;   /DAppVersion=<version>   plugin version (from buildspec.json)
;   /DSourceDir=<path>       install tree root, i.e. release/<Config>/pixelion-captions
;                            (contains bin\64bit and data)
;   /DOutputDir=<path>       directory for the generated installer
;   /DLicenseFile=<path>     optional license text shown in the wizard
;   /DOutputName=<name>      installer file name without extension
;
; Layout mirrors the CMake install tree / zip:
;   <SourceDir>\bin\64bit\*  ->  <OBS>\obs-plugins\64bit
;   <SourceDir>\data\*       ->  <OBS>\data\obs-plugins\pixelion-captions

#ifndef AppVersion
  #error "AppVersion define is required (iscc /DAppVersion=x.y.z)"
#endif
#ifndef SourceDir
  #error "SourceDir define is required (iscc /DSourceDir=path)"
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
#ifndef OutputName
  #define OutputName "pixelion-captions-" + AppVersion + "-windows-x64-installer"
#endif

#define AppName "Pixelion Captions"
#define PluginName "pixelion-captions"

[Setup]
; Fixed GUID: do not change, it lets newer installers upgrade in place.
AppId={{DEDBD78D-410C-4108-A391-FB5F214D703E}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Pixel Solutions
AppPublisherURL=https://pixelion.ai
DefaultDirName={code:GetObsDir}
DisableProgramGroupPage=yes
DisableDirPage=no
AppendDefaultDirName=no
DirExistsWarning=no
UsePreviousAppDir=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
; Keep the uninstaller out of the OBS root directory.
UninstallFilesDir={app}\data\obs-plugins\{#PluginName}
UninstallDisplayName={#AppName} (OBS plugin)
#ifdef LicenseFile
LicenseFile={#LicenseFile}
#endif
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; OBS locks the DLL while running; we check explicitly in code instead of
; relying on Restart Manager.
CloseApplications=no
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; Plugin DLL (and PDB when the build produced one).
Source: "{#SourceDir}\bin\64bit\*"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
; Plugin data (locale files etc.).
Source: "{#SourceDir}\data\*"; DestDir: "{app}\data\obs-plugins\{#PluginName}"; Flags: ignoreversion recursesubdirs createallsubdirs

; Plugin config in %APPDATA%\obs-studio\plugin_config\pixelion-captions is
; user data and is intentionally never touched, including on uninstall.

[Code]
// Default install dir: OBS Studio's HKLM\SOFTWARE\OBS Studio default value,
// read from the 64-bit registry view first and the 32-bit (WOW6432Node) view
// second. Falls back to commonpf64\obs-studio. The user can change it on the
// Select Destination Location page.
function GetObsDir(Param: String): String;
var
  Dir: String;
begin
  Result := '';
  if RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', Dir) and (Dir <> '') then
    Result := Dir
  else if RegQueryStringValue(HKLM32, 'SOFTWARE\OBS Studio', '', Dir) and (Dir <> '') then
    Result := Dir;
  if Result = '' then
    Result := ExpandConstant('{commonpf64}\obs-studio');
end;

function IsObsRunning(): Boolean;
var
  ResultCode: Integer;
begin
  Result := False;
  if Exec(ExpandConstant('{sys}\cmd.exe'),
          '/C tasklist /FI "IMAGENAME eq obs64.exe" /NH | find /I "obs64.exe"',
          '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Result := (ResultCode = 0);
end;

{ Block until OBS is closed, or return False if the user cancels. }
function EnsureObsClosed(): Boolean;
begin
  Result := True;
  while IsObsRunning() do
  begin
    if MsgBox('OBS Studio is running and locks the plugin files.' + #13#10 +
              'Please close OBS Studio, then click Retry. Click Cancel to abort.',
              mbError, MB_RETRYCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

function InitializeSetup(): Boolean;
begin
  Result := EnsureObsClosed();
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  ObsDir: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    ObsDir := WizardForm.DirEdit.Text;
    if not FileExists(AddBackslash(ObsDir) + 'bin\64bit\obs64.exe') then
      Result := MsgBox('obs64.exe was not found in:' + #13#10 + ObsDir + #13#10#13#10 +
                       'This does not look like an OBS Studio installation. Install here anyway?',
                       mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
    if Result then
      Result := EnsureObsClosed();
  end;
end;

{ OBS may have been started while the wizard was open. Re-check right before
  files are copied, because the copy would otherwise fail on the locked DLL. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not EnsureObsClosed() then
    Result := 'Installation cancelled because OBS Studio is still running.';
end;

function InitializeUninstall(): Boolean;
begin
  Result := EnsureObsClosed();
end;
