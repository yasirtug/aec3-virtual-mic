; AEC3 Virtual Mic installer (Inno Setup 6).
;
; Build (from the repository root, after building bin\AEC3VirtualMic.exe):
;   powershell -NoProfile -ExecutionPolicy Bypass -File installer\fetch_vaclite.ps1
;   ISCC.exe installer\AEC3VirtualMic.iss
; Output: installer\Output\AEC3VirtualMic-Setup-<version>.exe
; The version is read from src\resource.h (AEC3VM_VERSION_STR); /DAppVersion=x.y.z
; overrides it. See installer\README.md for the virtual cable research and sources.

#define AppName "AEC3 Virtual Mic"
#define AppExeName "AEC3VirtualMic.exe"
#define AppMutexName "AEC3VirtualMic.Running"
#define RepoRoot AddBackslash(SourcePath) + ".."
#define VacDir AddBackslash(SourcePath) + "vendor\vaclite"

#ifndef AppVersion
  #define VersionPrefix '#define AEC3VM_VERSION_STR "'
  #define VersionFile FileOpen(RepoRoot + "\src\resource.h")
  #if !VersionFile
    #error Cannot open src\resource.h to read AEC3VM_VERSION_STR.
  #endif
  #sub ReadVersionLine
    #define VersionLine Trim(FileRead(VersionFile))
    #if Pos(VersionPrefix, VersionLine) == 1
      #define public AppVersion Copy(VersionLine, Len(VersionPrefix) + 1, Len(VersionLine) - Len(VersionPrefix) - 1)
    #endif
  #endsub
  #for {0; !FileEof(VersionFile) && !Defined(AppVersion); 0} ReadVersionLine
  #expr FileClose(VersionFile)
  #ifndef AppVersion
    #error AEC3VM_VERSION_STR not found in src\resource.h.
  #endif
#endif

#if !FileExists(RepoRoot + "\bin\" + AppExeName)
  #error bin\AEC3VirtualMic.exe not found: build the app first.
#endif
#if !FileExists(VacDir + "\setup64.exe")
  #error VAC Lite package not found: run installer\fetch_vaclite.ps1 first.
#endif

[Setup]
AppId={{6E1C5A57-3D0B-4F8E-9C2A-AEC3B1C0DE01}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=AEC3VirtualMic contributors
AppCopyright=Copyright (c) 2026 AEC3VirtualMic contributors
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} Setup
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
AppMutex={#AppMutexName}
SetupMutex=AEC3VirtualMicSetup
CloseApplications=yes
RestartApplications=no
UsePreviousTasks=no
SetupIconFile={#RepoRoot}\assets\app.ico
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
OutputDir=Output
OutputBaseFilename=AEC3VirtualMic-Setup-{#AppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "autostart"; Description: "Start {#AppName} with Windows"; Flags: unchecked

[Files]
Source: "{#RepoRoot}\bin\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "{#RepoRoot}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
; The unmodified VAC Lite package, unpacked to Setup's temp folder only when no
; virtual cable is installed. VAC's own setup installs and registers its own
; uninstaller, so nothing of the package is kept.
Source: "{#VacDir}\*"; DestDir: "{tmp}\vaclite"; Check: ShouldInstallVacLite; Flags: ignoreversion recursesubdirs createallsubdirs deleteafterinstall

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"

[Registry]
; Fresh VAC Lite install only: enable the cable's volume control (documented
; per-cable "Volume control" DWORD, VAC manual "Configuration data in the
; Registry"). [Registry] runs before [Run], so the driver reads it on its first
; start. At 100% the signal is unchanged; it gives "Line 1" a working volume
; slider in Windows. An existing VAC's settings are never touched.
Root: HKLM64; Subkey: "SOFTWARE\EuMus Design\Virtual Audio Cable\4\Cable 1"; ValueType: dword; ValueName: "Volume control"; ValueData: 1; Check: ShouldInstallVacLite

[Run]
; No virtual cable yet: remember the default devices, run VAC Lite's own
; setup, then restore the default devices if Windows switched them to VAC.
Filename: "{app}\{#AppExeName}"; Parameters: "--snapshot-audio-defaults"; StatusMsg: "Saving your default audio devices..."; Check: ShouldInstallVacLite; Flags: runhidden waituntilterminated
Filename: "{code:VacSetupExe}"; WorkingDir: "{tmp}\vaclite"; StatusMsg: "Installing Virtual Audio Cable Lite (follow its setup window)..."; Check: ShouldInstallVacLite; AfterInstall: AfterVacLiteSetup; Flags: waituntilterminated
Filename: "{app}\{#AppExeName}"; Parameters: "--restore-audio-defaults"; StatusMsg: "Restoring your default audio devices..."; Check: VacLiteInstalledBySetup; Flags: runhidden waituntilterminated
Filename: "{app}\{#AppExeName}"; Parameters: "--enable-autostart"; StatusMsg: "Enabling start with Windows..."; Tasks: autostart; Flags: runasoriginaluser runhidden waituntilterminated
Filename: "{app}\{#AppExeName}"; Description: "Launch {#AppName} now"; Flags: postinstall nowait skipifsilent runasoriginaluser

[Code]
const
  MarkerKey = 'Software\AEC3VirtualMic';
  MarkerValue = 'InstalledVacLite';
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  RunValue = 'AEC3VirtualMic';
  UserKey = 'Software\AEC3VirtualMic';
  // Uninstall entry VAC Lite's setup registers (AppGUID in its setup.ini).
  VacLiteUninstallKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{83ed7f0e-2028-4956-b0b4-39c76fdaef1d}';
  VacUrl = 'https://vac.muzychenko.net/en/';
  // Exit-code bits of "AEC3VirtualMic.exe --detect-cables".
  DetectedVbCable = 1;
  DetectedVacLite = 2;

var
  CablesAtStart: Integer;
  MarkerPresentAtStart: Boolean;
  VacLiteInstalledNow: Boolean;
  RemoveVacLiteChosen: Boolean;

function IsMarkerPresent: Boolean;
var
  Value: Cardinal;
begin
  Result := RegQueryDWordValue(HKLM64, MarkerKey, MarkerValue, Value) and (Value = 1);
end;

function IsVacLiteInstalled: Boolean;
begin
  Result := RegKeyExists(HKLM64, VacLiteUninstallKey);
end;

function ShouldInstallVacLite: Boolean;
begin
  Result := CablesAtStart = 0;
end;

function VacLiteInstalledBySetup: Boolean;
begin
  Result := VacLiteInstalledNow;
end;

// VAC's manual: setup64a.exe on ARM64, otherwise its x64 setup.
function VacSetupExe(Param: String): String;
begin
  if IsArm64 then
    Result := ExpandConstant('{tmp}\vaclite\setup64a.exe')
  else
    Result := ExpandConstant('{tmp}\vaclite\setup64.exe');
end;

procedure AfterVacLiteSetup;
begin
  if IsVacLiteInstalled then
  begin
    VacLiteInstalledNow := True;
    if not RegWriteDWordValue(HKLM64, MarkerKey, MarkerValue, 1) then
      Log('Could not write the InstalledVacLite marker.');
    Log('VAC Lite installed by Setup.');
  end
  else
  begin
    Log('VAC Lite was not detected after running its setup.');
    SuppressibleMsgBox('Virtual Audio Cable Lite was not installed.' + #13#10#13#10 +
      'AEC3 Virtual Mic needs a virtual audio cable. You can install Virtual Audio Cable ' +
      'from ' + VacUrl + ' and then start AEC3 Virtual Mic.', mbError, MB_OK, IDOK);
  end;
end;

// Asks the app (same detection as at runtime) which virtual cables are active.
function DetectCables: Integer;
var
  ResultCode: Integer;
begin
  Result := 0;
  ExtractTemporaryFile('{#AppExeName}');
  if Exec(ExpandConstant('{tmp}\{#AppExeName}'), '--detect-cables', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode) then
    Result := ResultCode and (DetectedVbCable or DetectedVacLite)
  else
    Log(Format('Cable detection failed to run: %d', [ResultCode]));
  Log(Format('Virtual cables at start: VAC Lite %d, VB-CABLE %d', [Ord((Result and DetectedVacLite) <> 0), Ord((Result and DetectedVbCable) <> 0)]));
end;

function InitializeSetup: Boolean;
begin
  MarkerPresentAtStart := IsMarkerPresent;
  Result := True;
end;

procedure OpenVacSite(Sender: TObject);
var
  ErrorCode: Integer;
begin
  ShellExecAsOriginalUser('open', VacUrl, '', '', SW_SHOWNORMAL, ewNoWait, ErrorCode);
end;

procedure InitializeWizard;
var
  Page: TWizardPage;
  Body: TNewStaticText;
  Link: TNewStaticText;
begin
  CablesAtStart := DetectCables;
  if CablesAtStart = 0 then
  begin
    Page := CreateCustomPage(wpSelectDir, 'Virtual audio cable',
      'Setup will install Virtual Audio Cable Lite.');

    Body := TNewStaticText.Create(Page);
    Body.Parent := Page.Surface;
    Body.Left := 0;
    Body.Top := 0;
    Body.Width := Page.SurfaceWidth;
    Body.AutoSize := False;
    Body.WordWrap := True;
    Body.Caption :=
      'AEC3 Virtual Mic plays your processed voice into a virtual audio cable that apps ' +
      'such as Discord, Zoom or OBS use as their microphone ("Line 1 (Virtual Audio ' +
      'Cable)").' + #13#10#13#10 +
      'No virtual audio cable was found, so Setup will run the setup of Virtual Audio Cable ' +
      '(VAC) Lite by Eugene Muzychenko. Follow its window; Windows may ask you to confirm ' +
      'the driver.' + #13#10#13#10 +
      'VAC Lite is free for use in a private home environment not associated with income ' +
      'generation. For other use, see:';
    Body.AdjustHeight;

    Link := TNewStaticText.Create(Page);
    Link.Parent := Page.Surface;
    Link.Left := 0;
    Link.Top := Body.Top + Body.Height + ScaleY(4);
    Link.Caption := 'vac.muzychenko.net';
    Link.Cursor := crHand;
    Link.Font.Style := Link.Font.Style + [fsUnderline];
    Link.Font.Color := clHotLight;
    Link.OnClick := @OpenVacSite;
  end;

  // The Ready memo carries longer explanatory lines.
  WizardForm.ReadyMemo.WordWrap := True;
  WizardForm.ReadyMemo.ScrollBars := ssVertical;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo;
  if MemoTasksInfo <> '' then
    Result := Result + NewLine + NewLine + MemoTasksInfo;
  Result := Result + NewLine + NewLine + 'Virtual audio cable:' + NewLine + Space;
  if ShouldInstallVacLite then
    Result := Result + 'Install Virtual Audio Cable Lite (by Eugene Muzychenko)'
  else
    Result := Result + 'Use the installed virtual audio cable';
  Result := Result + NewLine + NewLine + 'Neural echo model (optional):' + NewLine + Space +
    'The app can use Google''s unreleased ML residual echo estimator that Chrome downloads ' +
    'for its own echo cancellation. It only copies that file from your local Chrome profile ' +
    '(no download, nothing uploaded); the file is not covered by this app''s MIT license and ' +
    'may be absent. Without it the app uses classic WebRTC AEC3.';
end;


// ---------------------------------------------------------------- Uninstall

procedure CloseRunningApp;
var
  ResultCode: Integer;
  Waited: Integer;
begin
  if not CheckForMutexes('{#AppMutexName}') then
    Exit;
  Log('AEC3 Virtual Mic is running; closing it.');
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#AppExeName}', '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode);
  Waited := 0;
  while CheckForMutexes('{#AppMutexName}') and (Waited < 5000) do
  begin
    Sleep(250);
    Waited := Waited + 250;
  end;
end;

// Removes autostart and settings of every loaded user profile (the uninstaller
// may run under an administrator account other than the app's user).
procedure RemoveUserSettings;
var
  Users: TArrayOfString;
  I: Integer;
begin
  RegDeleteValue(HKCU, RunKey, RunValue);
  RegDeleteKeyIncludingSubkeys(HKCU, UserKey);
  if RegGetSubkeyNames(HKU, '', Users) then
    for I := 0 to GetArrayLength(Users) - 1 do
      if Pos('_Classes', Users[I]) = 0 then
      begin
        RegDeleteValue(HKU, Users[I] + '\' + RunKey, RunValue);
        RegDeleteKeyIncludingSubkeys(HKU, Users[I] + '\' + UserKey);
      end;
  DelTree(ExpandConstant('{localappdata}\AEC3VirtualMic'), True, True, True);
end;

// Asks whether to remove VAC Lite too: checked by default only when this
// installer installed it.
function AskRemoveVacLite(Default: Boolean): Boolean;
var
  Form: TSetupForm;
  Check: TNewCheckBox;
  OkButton: TNewButton;
begin
  Result := Default;
  Form := CreateCustomForm(ScaleX(320), ScaleY(92), False, False);
  try
    Form.Caption := '{#AppName} Uninstall';

    Check := TNewCheckBox.Create(Form);
    Check.Parent := Form;
    Check.Left := ScaleX(16);
    Check.Top := ScaleY(18);
    Check.Width := Form.ClientWidth - ScaleX(32);
    Check.Height := ScaleY(17);
    Check.Caption := 'Also uninstall Virtual Audio Cable Lite';
    Check.Checked := Default;

    OkButton := TNewButton.Create(Form);
    OkButton.Parent := Form;
    OkButton.Width := ScaleX(75);
    OkButton.Height := ScaleY(23);
    OkButton.Left := Form.ClientWidth - OkButton.Width - ScaleX(16);
    OkButton.Top := Form.ClientHeight - OkButton.Height - ScaleY(14);
    OkButton.Caption := 'OK';
    OkButton.ModalResult := mrOk;
    OkButton.Default := True;

    Form.ActiveControl := OkButton;
    if Form.ShowModal = mrOk then
      Result := Check.Checked;
  finally
    Form.Free;
  end;
end;

// Runs VAC Lite's own (interactive) uninstaller from its registered command line,
// e.g. "C:\Program Files\Virtual Audio Cable\setup64.exe" -u.
procedure RemoveVacLite;
var
  Command: String;
  Exe: String;
  Params: String;
  Closing: Integer;
  ResultCode: Integer;
begin
  if not RemoveVacLiteChosen then
  begin
    Log('Leaving VAC Lite installed.');
    Exit;
  end;
  if not RegQueryStringValue(HKLM64, VacLiteUninstallKey, 'UninstallString', Command) then
  begin
    Log('VAC Lite uninstall command not found.');
    Exit;
  end;
  Command := Trim(Command);
  if (Length(Command) > 0) and (Command[1] = '"') then
  begin
    Delete(Command, 1, 1);
    Closing := Pos('"', Command);
    Exe := Copy(Command, 1, Closing - 1);
    Params := Trim(Copy(Command, Closing + 1, Length(Command)));
  end
  else
  begin
    Exe := Command;
    Params := '';
  end;
  UninstallProgressForm.StatusLabel.Caption := 'Removing Virtual Audio Cable Lite (follow its window)...';
  // VAC's setup64.exe hands the work to another process and exits at once;
  // Start-Process -Wait also waits for that child, so wait through PowerShell.
  if Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
      '-NoProfile -NonInteractive -Command "Start-Process -FilePath ''' + Exe +
      ''' -ArgumentList ''' + Params + ''' -Wait"', ExtractFileDir(Exe), SW_HIDE,
      ewWaitUntilTerminated, ResultCode) then
    Log(Format('VAC Lite uninstall finished (exit code %d), VAC Lite still installed: %d', [ResultCode, Ord(IsVacLiteInstalled)]))
  else
    Log(Format('Could not run the VAC Lite uninstaller: %d', [ResultCode]));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  case CurUninstallStep of
    usAppMutexCheck:
      begin
        CloseRunningApp;
        // VAC has no silent uninstall, so silent uninstalls leave it installed.
        RemoveVacLiteChosen := False;
        if IsVacLiteInstalled and not UninstallSilent then
          RemoveVacLiteChosen := AskRemoveVacLite(IsMarkerPresent);
        Log(Format('Remove VAC Lite: %d (installed by us: %d)', [Ord(RemoveVacLiteChosen), Ord(IsMarkerPresent)]));
      end;
    usUninstall:
      begin
        RemoveUserSettings;
        RemoveVacLite;
        RegDeleteKeyIncludingSubkeys(HKLM64, MarkerKey);
      end;
  end;
end;
