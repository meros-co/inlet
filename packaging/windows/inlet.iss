; SPDX-License-Identifier: AGPL-3.0-only
; Copyright (c) 2026 Meros Inc.
;
; Inlet's Windows installer (Inno Setup 6). build-package.ps1 compiles it from
; the files it stages:
;
;   ISCC /DVersion=0.2.0 /DStage=<staged files> /DOutDir=<dir> /DIcon=<app icon .ico>
;        [/DSign /Sinlet="<command signing $f>"] inlet.iss
;
; It installs over an earlier Inlet (this installer's or the old install.cmd's),
; keeping the settings in C:\ProgramData\Inlet. Unattended:
;   Inlet-<version>-windows-x64-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
; and to remove: "C:\Program Files\Inlet\unins000.exe" /VERYSILENT

#ifndef Version
  #error Pass /DVersion=MAJOR.MINOR.PATCH
#endif
#ifndef Stage
  #error Pass /DStage=<the staged files>
#endif
#ifndef OutDir
  #define OutDir "."
#endif

[Setup]
; Never change the AppId: Windows knows an installed Inlet by it.
AppId={{746C569C-2F24-405C-96FE-F479F86ADA86}
AppName=Inlet
AppVersion={#Version}
AppVerName=Inlet {#Version}
AppPublisher=Meros Inc.
AppPublisherURL=https://meros.co
AppSupportURL=https://github.com/meros-co/inlet/issues
AppUpdatesURL=https://github.com/meros-co/inlet/releases
AppCopyright=Copyright (c) 2026 Meros Inc.
VersionInfoVersion={#Version}.0
VersionInfoProductName=Inlet
VersionInfoDescription=Inlet installer
DefaultDirName={autopf}\Inlet
DisableDirPage=auto
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 22H2 or later.
MinVersion=10.0.19045
OutputDir={#OutDir}
OutputBaseFilename=Inlet-{#Version}-windows-x64-setup
#ifdef Icon
SetupIconFile={#Icon}
#endif
UninstallDisplayIcon={app}\Inlet.exe
UninstallDisplayName=Inlet
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; Inlet's own steps stop what runs from it; Windows' Restart Manager would not
; know about the service.
CloseApplications=no
RestartApplications=no
#ifdef Sign
SignTool=inlet
SignedUninstaller=yes
#endif

[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "setup-steps.ps1"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
; What the zip's install.cmd left in place.
Type: files; Name: "{app}\install.cmd"
Type: files; Name: "{app}\install.ps1"
Type: files; Name: "{app}\uninstall.cmd"
Type: files; Name: "{app}\uninstall.ps1"

[Icons]
Name: "{autoprograms}\Inlet"; Filename: "{app}\Inlet.exe"; WorkingDir: "{app}"

[Registry]
; The app in the tray at every sign-in; it starts the service if it was stopped.
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Inlet"; \
    ValueData: """{app}\Inlet.exe"" --tray"; Flags: uninsdeletevalue

[Run]
; As the signed-in user, not elevated.
Filename: "{app}\Inlet.exe"; Description: "Open Inlet"; Flags: nowait postinstall skipifsilent runasoriginaluser

[Code]
var
  ConfigureFailed: Boolean;

function RunStep(const Script, Step, AppDir: String; var Reason: String): Boolean;
var
  Code: Integer;
  Log: String;
  Text: AnsiString;
begin
  Log := ExpandConstant('{tmp}\inlet-' + Step + '.log');
  DeleteFile(Log);
  Result := Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
                 '-NoProfile -ExecutionPolicy Bypass -File "' + Script + '" -Step ' + Step +
                 ' -AppDir "' + AppDir + '" -Log "' + Log + '"',
                 '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
  if not Result then
  begin
    if LoadStringFromFile(Log, Text) then
      Reason := Trim(String(Text))
    else
      Reason := 'PowerShell could not run (' + IntToStr(Code) + ').';
  end;
end;

// Before any file is replaced: an earlier Inlet stops.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Reason: String;
begin
  Result := '';
  ExtractTemporaryFile('setup-steps.ps1');
  if not RunStep(ExpandConstant('{tmp}\setup-steps.ps1'), 'Stop', ExpandConstant('{app}'), Reason) then
    Result := 'The Inlet already installed could not be stopped: ' + Reason;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Reason: String;
begin
  if CurStep = ssPostInstall then
    if not RunStep(ExpandConstant('{app}\setup-steps.ps1'), 'Configure', ExpandConstant('{app}'), Reason) then
    begin
      ConfigureFailed := True;
      Log('Configure failed: ' + Reason);
      SuppressibleMsgBox('Inlet is installed, but setting it up did not finish: ' + Reason + #13#10#13#10 +
                         'Run the installer again; if this repeats, see C:\ProgramData\Inlet\inletd.log.',
                         mbError, MB_OK, IDOK);
    end;
end;

// An unattended install that could not set Inlet up says so in its exit code.
function GetCustomSetupExitCode: Integer;
begin
  if ConfigureFailed then
    Result := 8
  else
    Result := 0;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Reason: String;
begin
  if CurUninstallStep = usUninstall then
    if not RunStep(ExpandConstant('{app}\setup-steps.ps1'), 'Remove', ExpandConstant('{app}'), Reason) then
      SuppressibleMsgBox('Inlet could not be stopped completely: ' + Reason, mbError, MB_OK, IDOK);
end;
