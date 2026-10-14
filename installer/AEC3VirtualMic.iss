; AEC3 Virtual Mic installer (Inno Setup 6).
;
; Build (from the repository root, after building bin\AEC3VirtualMic.exe):
;   pwsh -NoProfile -File installer\fetch_vbcable.ps1
;   ISCC.exe installer\AEC3VirtualMic.iss
; Output: installer\Output\AEC3VirtualMic-Setup-<version>.exe
; The version is read from src\resource.h (AEC3VM_VERSION_STR); /DAppVersion=x.y.z
; overrides it. See installer\README.md for the VB-CABLE research and sources.

#define AppName "AEC3 Virtual Mic"
#define AppExeName "AEC3VirtualMic.exe"
#define AppMutexName "AEC3VirtualMic.Running"
#define RepoRoot AddBackslash(SourcePath) + ".."
#define VbCableDir AddBackslash(SourcePath) + "vendor\vbcable"

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
#if !FileExists(VbCableDir + "\VBCABLE_Setup_x64.exe")
  #error VB-CABLE pack not found: run installer\fetch_vbcable.ps1 first.
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
; The unmodified VB-CABLE driver pack, always kept in {app}\vbcable: Setup runs
; it to install VB-CABLE, and the uninstaller to remove VB-CABLE on request.
Source: "{#VbCableDir}\*"; Excludes: ".sha256"; DestDir: "{app}\vbcable"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"

[Run]
; Fresh VB-CABLE install only: remember the default devices, install the
; driver, then apply recommended settings and restore the default devices.
Filename: "{app}\{#AppExeName}"; Parameters: "--snapshot-audio-defaults"; StatusMsg: "Saving your default audio devices..."; Check: ShouldInstallVbCable; Flags: runhidden waituntilterminated
Filename: "{app}\vbcable\VBCABLE_Setup_x64.exe"; Parameters: "-i -h"; WorkingDir: "{app}\vbcable"; StatusMsg: "Installing VB-CABLE (VB-Audio Virtual Cable)..."; Check: ShouldInstallVbCable; AfterInstall: AfterVbCableSetup; Flags: waituntilterminated
Filename: "{app}\{#AppExeName}"; Parameters: "--configure-vbcable"; StatusMsg: "Applying recommended VB-CABLE settings..."; Check: VbCableInstalledBySetup; Flags: runhidden waituntilterminated
Filename: "{app}\{#AppExeName}"; Parameters: "--enable-autostart"; StatusMsg: "Enabling start with Windows..."; Tasks: autostart; Flags: runasoriginaluser runhidden waituntilterminated
Filename: "{app}\{#AppExeName}"; Description: "Launch {#AppName} now"; Flags: postinstall nowait skipifsilent runasoriginaluser

[Code]
const
  MarkerKey = 'Software\AEC3VirtualMic';
  MarkerValue = 'InstalledVbCable';
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  RunValue = 'AEC3VirtualMic';
  UserKey = 'Software\AEC3VirtualMic';
  RenderEndpointsKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render';
  // Endpoint property cache: hardware id of the endpoint's device.
  HardwareIdProperty = '{a8b865dd-2e3d-4094-ad97-e593a70c75d6},8';
  // Hardware id of the plain VB-CABLE device (driver INF "HardwareId").
  VbCableHardwareId = 'VBAudioVACWDM';
  // Uninstall entry written by VBCABLE_Setup_x64.exe.
  VbCableUninstallKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VB:VBCABLE {87459874-1236-4469}';
  VbCableUrl = 'https://vb-cable.com';

var
  VbCablePresentAtStart: Boolean;
  VacLitePresentAtStart: Boolean;
  MarkerPresentAtStart: Boolean;
  VbCableInstalledNow: Boolean;
  VbCableRemovedNow: Boolean;
  RemoveVbCableChosen: Boolean;
  CableChoicePage: TInputOptionWizardPage;

// True when a VB-CABLE playback endpoint exists (active or disabled by the
// user) or VB-CABLE's own uninstall entry is registered.
function IsVbCableInstalled: Boolean;
var
  Endpoints: TArrayOfString;
  I: Integer;
  State: Cardinal;
  HardwareId: String;
begin
  Result := RegKeyExists(HKLM64, VbCableUninstallKey);
  if Result then
    Exit;
  if not RegGetSubkeyNames(HKLM64, RenderEndpointsKey, Endpoints) then
    Exit;
  for I := 0 to GetArrayLength(Endpoints) - 1 do
  begin
    if RegQueryStringValue(HKLM64, RenderEndpointsKey + '\' + Endpoints[I] + '\Properties',
        HardwareIdProperty, HardwareId) and
       (CompareText(HardwareId, VbCableHardwareId) = 0) and
       RegQueryDWordValue(HKLM64, RenderEndpointsKey + '\' + Endpoints[I], 'DeviceState', State) and
       ((State = 1) or (State = 2)) then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

function IsMarkerPresent: Boolean;
var
  Value: Cardinal;
begin
  Result := RegQueryDWordValue(HKLM64, MarkerKey, MarkerValue, Value) and (Value = 1);
end;

function ShouldInstallVbCable: Boolean;
begin
  Result := not VbCablePresentAtStart;
  if Result and VacLitePresentAtStart then
    Result := CableChoicePage.Values[1];
end;

function VbCableInstalledBySetup: Boolean;
begin
  Result := VbCableInstalledNow;
end;

procedure AfterVbCableSetup;
begin
  if IsVbCableInstalled then
  begin
    VbCableInstalledNow := True;
    if not RegWriteDWordValue(HKLM64, MarkerKey, MarkerValue, 1) then
      Log('Could not write the InstalledVbCable marker.');
    Log('VB-CABLE installed by Setup.');
  end
  else
  begin
    Log('VB-CABLE was not detected after running its setup.');
    SuppressibleMsgBox('VB-CABLE could not be installed.' + #13#10#13#10 +
      'AEC3 Virtual Mic needs a virtual audio cable. Please install VB-CABLE from ' +
      VbCableUrl + ' (run VBCABLE_Setup_x64.exe as administrator), then restart your computer.',
      mbError, MB_OK, IDOK);
  end;
end;

function InitializeSetup: Boolean;
begin
  VbCablePresentAtStart := IsVbCableInstalled;
  MarkerPresentAtStart := IsMarkerPresent;
  Log(Format('VB-CABLE present: %d, installed by us earlier: %d', [Ord(VbCablePresentAtStart), Ord(MarkerPresentAtStart)]));
  Result := True;
end;

procedure OpenVbCableSite(Sender: TObject);
var
  ErrorCode: Integer;
begin
  ShellExecAsOriginalUser('open', VbCableUrl, '', '', SW_SHOWNORMAL, ewNoWait, ErrorCode);
end;

// Asks the app (same detection as at runtime) whether VAC Lite is active.
function DetectVacLite: Boolean;
var
  ResultCode: Integer;
begin
  Result := False;
  ExtractTemporaryFile('{#AppExeName}');
  if Exec(ExpandConstant('{tmp}\{#AppExeName}'), '--detect-cables', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode) then
    Result := (ResultCode and 2) <> 0
  else
    Log(Format('Cable detection failed to run: %d', [ResultCode]));
  Log(Format('VAC Lite present: %d', [Ord(Result)]));
end;

procedure AddVbCableNotice(Page: TWizardPage; const Text: String);
var
  Body: TNewStaticText;
  Link: TNewStaticText;
begin
  Body := TNewStaticText.Create(Page);
  Body.Parent := Page.Surface;
  Body.Left := 0;
  Body.Top := 0;
  Body.Width := Page.SurfaceWidth;
  Body.AutoSize := False;
  Body.WordWrap := True;
  Body.Caption := Text;
  Body.AdjustHeight;

  Link := TNewStaticText.Create(Page);
  Link.Parent := Page.Surface;
  Link.Left := 0;
  Link.Top := Body.Top + Body.Height + ScaleY(4);
  Link.Caption := 'www.vb-cable.com';
  Link.Cursor := crHand;
  Link.Font.Style := Link.Font.Style + [fsUnderline];
  Link.Font.Color := clHotLight;
  Link.OnClick := @OpenVbCableSite;
end;

procedure InitializeWizard;
var
  Text: String;
begin
  if VbCablePresentAtStart then
  begin
    Text :=
      'AEC3 Virtual Mic plays your processed voice into a virtual audio cable that apps ' +
      'such as Discord, Zoom or OBS use as their microphone ("CABLE Output").' + #13#10#13#10 +
      'VB-CABLE (VB-Audio Virtual Cable) is already installed on this computer. Setup uses it ' +
      'as it is: the existing VB-CABLE and its settings are left untouched.' + #13#10#13#10 +
      'VB-CABLE is VB-Audio donationware; all participations are welcome:';
    AddVbCableNotice(CreateCustomPage(wpSelectDir, 'Virtual audio cable',
      'VB-CABLE is already installed.'), Text);
  end
  else
  begin
    VacLitePresentAtStart := DetectVacLite;
    if VacLitePresentAtStart then
    begin
      CableChoicePage := CreateInputOptionPage(wpSelectDir, 'Virtual audio cable',
        'Choose the virtual audio cable AEC3 Virtual Mic plays your voice into.', '',
        True, False);
      CableChoicePage.Add('Use the installed VAC Lite');
      CableChoicePage.Add('Install VB-CABLE (VB-Audio donationware)');
      CableChoicePage.Values[0] := True;
    end
    else
    begin
      Text :=
        'AEC3 Virtual Mic plays your processed voice into a virtual audio cable that apps ' +
        'such as Discord, Zoom or OBS use as their microphone ("CABLE Output").' + #13#10#13#10 +
        'No virtual audio cable was found, so Setup will install VB-CABLE by VB-Audio ' +
        'Software. Windows may ask you to confirm the VB-Audio driver.' + #13#10#13#10 +
        'VB-CABLE is donationware: free to use, and all participations are welcome. ' +
        'If you find it useful, please support VB-Audio at:';
      AddVbCableNotice(CreateCustomPage(wpSelectDir, 'Virtual audio cable',
        'Setup will install VB-CABLE, the VB-Audio Virtual Cable.'), Text);
    end;
  end;

  // The Ready memo carries longer explanatory lines.
  WizardForm.ReadyMemo.WordWrap := True;
  WizardForm.ReadyMemo.ScrollBars := ssVertical;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssInstall) and VacLitePresentAtStart and not VbCablePresentAtStart then
  begin
    if ShouldInstallVbCable then
      Log('Cable choice: Install VB-CABLE')
    else
      Log('Cable choice: Use the installed VAC Lite');
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo;
  if MemoTasksInfo <> '' then
    Result := Result + NewLine + NewLine + MemoTasksInfo;
  Result := Result + NewLine + NewLine + 'Virtual audio cable:' + NewLine + Space;
  if VbCablePresentAtStart then
    Result := Result + 'Use the installed VB-CABLE (left untouched)'
  else if not ShouldInstallVbCable then
    Result := Result + 'Use the installed VAC Lite'
  else
    Result := Result + 'Install VB-CABLE by VB-Audio (donationware, www.vb-cable.com)';
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

// Asks whether to remove VB-CABLE too: checked by default only when this
// installer installed it. Silent uninstalls use that default.
function AskRemoveVbCable(Default: Boolean): Boolean;
var
  Form: TSetupForm;
  Check: TNewCheckBox;
  OkButton: TNewButton;
begin
  Result := Default;
  if UninstallSilent then
    Exit;
  Form := CreateCustomForm(ScaleX(320), ScaleY(92), False, False);
  try
    Form.Caption := '{#AppName} Uninstall';

    Check := TNewCheckBox.Create(Form);
    Check.Parent := Form;
    Check.Left := ScaleX(16);
    Check.Top := ScaleY(18);
    Check.Width := Form.ClientWidth - ScaleX(32);
    Check.Height := ScaleY(17);
    Check.Caption := 'Also uninstall VB-CABLE';
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

procedure RemoveVbCable;
var
  SetupExe: String;
  ResultCode: Integer;
begin
  if not RemoveVbCableChosen then
  begin
    Log('Leaving VB-CABLE installed.');
    Exit;
  end;
  SetupExe := ExpandConstant('{app}\vbcable\VBCABLE_Setup_x64.exe');
  if FileExists(SetupExe) then
  begin
    UninstallProgressForm.StatusLabel.Caption := 'Removing VB-CABLE (VB-Audio Virtual Cable)...';
    if Exec(SetupExe, '-u -h', ExtractFileDir(SetupExe), SW_SHOWNORMAL,
        ewWaitUntilTerminated, ResultCode) then
    begin
      Log(Format('VB-CABLE uninstall exit code: %d', [ResultCode]));
      VbCableRemovedNow := True;
    end
    else
      Log(Format('Could not run the VB-CABLE uninstaller: %d', [ResultCode]));
  end
  else
    SuppressibleMsgBox('The VB-CABLE setup program is missing, so VB-CABLE could not be ' +
      'removed automatically. You can remove it with VBCABLE_Setup_x64.exe from ' +
      VbCableUrl + '.', mbInformation, MB_OK, IDOK);
  RegDeleteValue(HKLM64, MarkerKey, MarkerValue);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  case CurUninstallStep of
    usAppMutexCheck:
      begin
        CloseRunningApp;
        RemoveVbCableChosen := False;
        if IsVbCableInstalled then
          RemoveVbCableChosen := AskRemoveVbCable(IsMarkerPresent);
        Log(Format('Remove VB-CABLE: %d (installed by us: %d)', [Ord(RemoveVbCableChosen), Ord(IsMarkerPresent)]));
      end;
    usUninstall:
      begin
        RemoveUserSettings;
        RemoveVbCable;
        RegDeleteKeyIncludingSubkeys(HKLM64, MarkerKey);
      end;
  end;
end;

function UninstallNeedRestart: Boolean;
begin
  Result := VbCableRemovedNow;
end;
