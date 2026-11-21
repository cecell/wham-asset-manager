; Inno Setup (6.6 or newer) script for the WHAM shell extension.
; Built by CMake (target "installer"); can also be compiled by hand:
;   ISCC.exe /DAppVersion=0.90 /DBinDir=..\build\bin /DOutDir=..\build installer\PluginShellExtension.iss
;
; Installs per user (no admin prompt) into %LOCALAPPDATA%\Programs\Plugin Shell Extension
; and registers the context-menu handler under HKCU\Software\Classes. The
; optional "columns" task additionally registers the property handler and its
; schema machine-wide, which Windows only accepts from an elevated process: it
; runs "regsvr32 /n /i:columns" through a UAC prompt. Everything else calls the
; installed DLL's DllInstall from Setup itself (a 64-bit setup).

#ifndef AppVersion
  #define AppVersion "0.90"
#endif
#ifndef BinDir
  #define BinDir "..\build\bin"
#endif
#ifndef OutDir
  #define OutDir "..\build"
#endif

#define AppName "Windows Hyper Asset Manager - WHAM"
#define Clsid "{{1D4348E0-3A45-489E-8022-6A2A883E9D94}"
#define ClsidKey "Software\Classes\CLSID\" + Clsid
#define Sfa "Software\Classes\SystemFileAssociations"
#define PropHandlerClsid "{E0BA20B2-0391-45F5-80C3-07E306E945E9}"
#define HardlinkClsid "{{9FB91679-FC8C-4040-9C35-61E735E81908}"
#define HardlinkClsidKey "Software\Classes\CLSID\" + HardlinkClsid
#define HardlinkMenuKey "Software\Classes\*\shellex\ContextMenuHandlers\PluginShellExtensionHardlink"
#define P "Cecell.PluginShellExtension."
; Details pane (Alt+Shift+P) and hover tooltip contents.
#define PreviewDetails "prop:" + P + "HeaderVersion;" + P + "FormVersion;" + P + "FormCount;" + P + "RecordCount;" + P + "Flags;" + P + "Light;" + P + "MasterCount;" + P + "MasterStatus;" + P + "DependentCount;" + P + "DeletedCount;" + P + "DeletedNavmeshes;" + P + "ItmCount;" + P + "Dirty;" + P + "ESLable;" + P + "ESLNote;" + P + "Masters;*System.DateModified;*System.Size;*System.DateCreated"
#define InfoTip "prop:System.ItemTypeText;System.Size;System.DateModified;" + P + "FormCount;" + P + "TipStatus;" + P + "Masters;" + P + "ESLNote"
#define TgaDetails "prop:System.ItemTypeText;System.Size;System.DateModified;System.Image.Dimensions;System.Image.BitDepth"
#define FuzDetails "prop:System.ItemTypeText;System.Size;System.DateModified;System.Media.Duration;System.Audio.EncodingBitrate;" + P + "SampleRate;" + P + "Channels;" + P + "AudioFormat;" + P + "FuzVersion;" + P + "HasLip;" + P + "LipVersion;" + P + "LipUnits"

[Setup]
AppId={{F78B36DB-E7CD-4FA3-B923-5643F4FD3ADB}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Cecell
AppComments=Explorer columns, previews and tools for Skyrim plugins, textures, audio, scripts and archives.
VersionInfoVersion={#AppVersion}
VersionInfoDescription={#AppName} Setup
PrivilegesRequired=lowest
DefaultDirName={autopf}\WHAM
DisableProgramGroupPage=yes
; The licence is the first page.
DisableWelcomePage=yes
LicenseFile=..\LICENSE
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; A 64-bit setup, so it can call the (64-bit) DLL's DllInstall itself for the
; per-user registrations instead of starting regsvr32 for each.
SetupArchitecture=x64
MinVersion=10.0
; Follows the Windows light/dark app mode (Inno Setup 6.6 or newer).
WizardStyle=modern dynamic
Compression=lzma2/max
SolidCompression=yes
ChangesAssociations=yes
; Explorer keeps the DLL loaded, and loading the same path again gives back
; that old copy, so a new version only runs after Explorer restarts.
; Installing doesn't need Explorer closed: the [Code] section moves the loaded
; DLL aside. Explorer is restarted afterwards instead, and only Explorer (see
; RestartExplorer below): Restart Manager here would also try to close every
; other program that loaded the DLL, such as Everything, and fail.
CloseApplications=no
OutputDir={#OutDir}
OutputBaseFilename=WHAM-{#AppVersion}-setup
UninstallDisplayName={#AppName}
SetupIconFile=icon\PluginShellExtension.ico
UninstallDisplayIcon={app}\PluginShellExtension.ico

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
FinishedLabelNoIcons=Setup has finished installing [name] on your computer.%n%nRight-click a .esp, .esm or .esl file and open "Skyrim SE plugins". On Windows 11 it is under "Show more options" (or Shift+right-click).

[Tasks]
Name: "columns"; Description: "  Add columns to Explorer, the details pane and files tooltip (requires admin rights)"
Name: "bsa"; Description: "  Browse .bsa archives in Explorer and add the BSA manager menu (read-only)"
Name: "hashes"; Description: "  Add ""Hashes"" to the right-click menu of every file (SHA-256, CRC-32 and MD5)"
Name: "papyrus"; Description: "  Add ""Decompile"" to .pex and ""Compile"" to .psc files in the right-click menu"
Name: "folderviews"; Description: "  Add ""Folder views"" to the folder background menu (save layouts and apply them to other folders)"
Name: "hardlink"; Description: "  Add ""Hardlink"" to the right-click menu of every file (copy or open the path of its other link)"

[Files]
Source: "{#BinDir}\PluginShellExtension.dll"; DestDir: "{app}"; Flags: ignoreversion
; The DLL's symbols, so a crash dump from Explorer can be read.
Source: "{#BinDir}\PluginShellExtension.pdb"; DestDir: "{app}"; Flags: ignoreversion
Source: "PluginShellExtension.propdesc"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\screenshot.png"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSES\MPL-2.0.txt"; DestDir: "{app}\LICENSES"; Flags: ignoreversion
Source: "icon\PluginShellExtension.ico"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
Type: files; Name: "{app}\*.old"

[UninstallDelete]
Type: files; Name: "{app}\*.old"
; The saved column results (src/disk_cache.cpp)
Type: filesandordirs; Name: "{localappdata}\Plugin Shell Extension"

[Registry]
; COM server
Root: HKA; Subkey: "{#ClsidKey}"; ValueType: string; ValueData: "WHAM context menu"; Flags: uninsdeletekey
Root: HKA; Subkey: "{#ClsidKey}\InprocServer32"; ValueType: string; ValueData: "{app}\PluginShellExtension.dll"
Root: HKA; Subkey: "{#ClsidKey}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"
; Context-menu handler for each plugin extension. The parent keys are removed
; on uninstall only if nothing else lives in them.
Root: HKA; Subkey: "{#Sfa}\.esp"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "{#Sfa}\.esp\shellex"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "{#Sfa}\.esp\shellex\ContextMenuHandlers"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "{#Sfa}\.esp\shellex\ContextMenuHandlers\PluginShellExtension"; ValueType: string; ValueData: "{#Clsid}"; Flags: uninsdeletekey
Root: HKA; Subkey: "{#Sfa}\.esm"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "{#Sfa}\.esm\shellex"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "{#Sfa}\.esm\shellex\ContextMenuHandlers"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "{#Sfa}\.esm\shellex\ContextMenuHandlers\PluginShellExtension"; ValueType: string; ValueData: "{#Clsid}"; Flags: uninsdeletekey
Root: HKA; Subkey: "{#Sfa}\.esl"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "{#Sfa}\.esl\shellex"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "{#Sfa}\.esl\shellex\ContextMenuHandlers"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "{#Sfa}\.esl\shellex\ContextMenuHandlers\PluginShellExtension"; ValueType: string; ValueData: "{#Clsid}"; Flags: uninsdeletekey
; Optional "Hardlink" entry on every file's context menu (src/hardlink_menu.cpp).
Root: HKA; Subkey: "{#HardlinkClsidKey}"; ValueType: string; ValueData: "WHAM hardlink menu"; Flags: uninsdeletekey; Tasks: hardlink
Root: HKA; Subkey: "{#HardlinkClsidKey}\InprocServer32"; ValueType: string; ValueData: "{app}\PluginShellExtension.dll"; Tasks: hardlink
Root: HKA; Subkey: "{#HardlinkClsidKey}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"; Tasks: hardlink
Root: HKA; Subkey: "{#HardlinkMenuKey}"; ValueType: string; ValueData: "{#HardlinkClsid}"; Flags: uninsdeletekey; Tasks: hardlink
; Details pane and tooltip fields (per user; the properties themselves come
; from the machine-wide handler the columns task registers).
Root: HKA; Subkey: "{#Sfa}\.esp"; ValueType: string; ValueName: "PreviewDetails"; ValueData: "{#PreviewDetails}"; Flags: uninsdeletevalue; Tasks: columns
Root: HKA; Subkey: "{#Sfa}\.esp"; ValueType: string; ValueName: "InfoTip"; ValueData: "{#InfoTip}"; Flags: uninsdeletevalue; Tasks: columns
Root: HKA; Subkey: "{#Sfa}\.esm"; ValueType: string; ValueName: "PreviewDetails"; ValueData: "{#PreviewDetails}"; Flags: uninsdeletevalue; Tasks: columns
Root: HKA; Subkey: "{#Sfa}\.esm"; ValueType: string; ValueName: "InfoTip"; ValueData: "{#InfoTip}"; Flags: uninsdeletevalue; Tasks: columns
Root: HKA; Subkey: "{#Sfa}\.esl"; ValueType: string; ValueName: "PreviewDetails"; ValueData: "{#PreviewDetails}"; Flags: uninsdeletevalue; Tasks: columns
Root: HKA; Subkey: "{#Sfa}\.esl"; ValueType: string; ValueName: "InfoTip"; ValueData: "{#InfoTip}"; Flags: uninsdeletevalue; Tasks: columns
Root: HKA; Subkey: "{#Sfa}\.tga"; Flags: uninsdeletekeyifempty; Tasks: columns
Root: HKA; Subkey: "{#Sfa}\.tga"; ValueType: string; ValueName: "PreviewDetails"; ValueData: "{#TgaDetails}"; Flags: uninsdeletevalue; Tasks: columns
Root: HKA; Subkey: "{#Sfa}\.tga"; ValueType: string; ValueName: "InfoTip"; ValueData: "{#TgaDetails}"; Flags: uninsdeletevalue; Tasks: columns
Root: HKA; Subkey: "{#Sfa}\.tga"; ValueType: string; ValueName: "FullDetails"; ValueData: "{#TgaDetails}"; Flags: uninsdeletevalue; Tasks: columns
Root: HKA; Subkey: "{#Sfa}\.fuz"; Flags: uninsdeletekeyifempty; Tasks: columns
Root: HKA; Subkey: "{#Sfa}\.fuz"; ValueType: string; ValueName: "PreviewDetails"; ValueData: "{#FuzDetails}"; Flags: uninsdeletevalue; Tasks: columns
Root: HKA; Subkey: "{#Sfa}\.fuz"; ValueType: string; ValueName: "InfoTip"; ValueData: "{#FuzDetails}"; Flags: uninsdeletevalue; Tasks: columns
Root: HKA; Subkey: "{#Sfa}\.fuz"; ValueType: string; ValueName: "FullDetails"; ValueData: "{#FuzDetails}"; Flags: uninsdeletevalue; Tasks: columns

[Run]
Filename: "{app}\README.md"; Description: "Open the README"; Flags: postinstall shellexec skipifsilent nowait unchecked

[Code]
// Before 0.90 this extension shipped under another name and AppId, using the
// same COM and registry keys. Installing over it would let its uninstaller
// later remove keys this version needs, so it has to be uninstalled first.
const
  OldUninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{27A531A2-EEF9-4E30-BDE9-3295D03F4D3E}_is1';

var
  LicenseCheck: TNewCheckBox;
  AiNotice, InnoCredit: TNewStaticText;

// --- Licence page: a checkbox instead of Inno's accept / decline radios ---

procedure SyncLicense;
begin
  WizardForm.LicenseAcceptedRadio.Checked := LicenseCheck.Checked;
  WizardForm.LicenseNotAcceptedRadio.Checked := not LicenseCheck.Checked;
  WizardForm.NextButton.Enabled := LicenseCheck.Checked;
end;

procedure LicenseCheckClick(Sender: TObject);
begin
  SyncLicense;
end;

// --- Tasks page: a notice under the task list and the Inno Setup credit at
// the bottom of the page ---

function AddPageText(const Caption: String): TNewStaticText;
begin
  Result := TNewStaticText.Create(WizardForm);
  Result.Parent := WizardForm.SelectTasksPage;
  Result.AutoSize := False;
  Result.WordWrap := True;
  Result.Left := WizardForm.TasksList.Left;
  Result.Width := WizardForm.TasksList.Width;
  Result.Caption := Caption;
  Result.AdjustHeight;
end;

procedure AddTasksPageText;
begin
  WizardForm.TasksList.Height := ScaleY(150);
  AiNotice := AddPageText('WHAM was written with the assistance of Claude by Anthropic. ' +
    'Many have negative feelings towards AI but it has been a life saver for people like me who struggle ' +
    'with cognitive disabilities. I hope this software is helpful.');
  AiNotice.Top := WizardForm.TasksList.Top + WizardForm.TasksList.Height + ScaleY(16);
  InnoCredit := AddPageText('Uses Inno Setup, Copyright (C) Jordan Russell, https://jrsoftware.org/');
  InnoCredit.Top := WizardForm.SelectTasksPage.Height - InnoCredit.Height;
end;

function InitializeSetup: Boolean;
var
  OldName: String;
begin
  Result := True;
  if RegQueryStringValue(HKCU, OldUninstallKey, 'DisplayName', OldName) or
     RegQueryStringValue(HKLM, OldUninstallKey, 'DisplayName', OldName) then begin
    SuppressibleMsgBox('An earlier version of this extension is installed as "' + OldName + '".' + #13#10#13#10 +
      'Uninstall it from Settings > Apps > Installed apps, restart Explorer, then run this setup again.',
      mbError, MB_OK, IDOK);
    Result := False;
  end;
end;

procedure InitializeWizard;
begin
  WizardForm.LicenseAcceptedRadio.Visible := False;
  WizardForm.LicenseNotAcceptedRadio.Visible := False;
  LicenseCheck := TNewCheckBox.Create(WizardForm);
  LicenseCheck.Parent := WizardForm.LicensePage;
  LicenseCheck.Left := WizardForm.LicenseAcceptedRadio.Left;
  LicenseCheck.Top := WizardForm.LicenseAcceptedRadio.Top;
  LicenseCheck.Width := WizardForm.LicenseMemo.Width;
  LicenseCheck.Height := WizardForm.LicenseAcceptedRadio.Height;
  LicenseCheck.Caption := 'I agree to the terms of the GNU General Public License v3.0';
  LicenseCheck.OnClick := @LicenseCheckClick;
  AddTasksPageText;
end;

// An update restarts Explorer when it is done: say so before anything is installed.
function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := '';
  if FileExists(ExpandConstant('{app}\PluginShellExtension.dll')) then
    Result := 'All File Explorer windows will be closed when you click Finish:' + NewLine +
      Space + 'Explorer restarts so the new version is the one it runs.' + NewLine +
      Space + 'Your desktop and taskbar come back by themselves.' + NewLine + NewLine;
  if MemoDirInfo <> '' then Result := Result + MemoDirInfo + NewLine + NewLine;
  if MemoGroupInfo <> '' then Result := Result + MemoGroupInfo + NewLine + NewLine;
  if MemoTasksInfo <> '' then Result := Result + MemoTasksInfo + NewLine + NewLine;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpLicense then
    SyncLicense;
end;

// --- Moving a loaded DLL aside ---
// A DLL that Explorer has loaded cannot be overwritten or deleted, but it can
// be renamed. Move it aside so the new copy (or the uninstall) can proceed;
// the leftover is removed by the next install or when it is no longer loaded.

// DLLs earlier installs moved aside: deleted once nothing has them loaded
// (after Restart Manager closed Explorer, that is now). One still in use stays.
var
  WasUpdate: Boolean;

// The installed DLL's DllInstall, called in this process: what "regsvr32 /n
// /i:<what>" (or /u /n /i:<what>) would call, without starting regsvr32.
// Setup can't name {app} here (it isn't known when Setup starts), so it loads
// its own temporary copy and passes the installed DLL's path after a ";".
function DllInstallSetup(Install: Integer; CmdLine: String): Integer;
  external 'DllInstall@files:PluginShellExtension.dll stdcall delayload setuponly';
function DllInstallUninst(Install: Integer; CmdLine: String): Integer;
  external 'DllInstall@{app}\PluginShellExtension.dll stdcall delayload uninstallonly';

function CallDllInstall(Install: Boolean; const What: String): Integer;
begin
  try
    if IsUninstaller then
      Result := DllInstallUninst(Ord(Install), What)
    else
      Result := DllInstallSetup(Ord(Install), What + ';' + ExpandConstant('{app}') + '\PluginShellExtension.dll');
  except
    Result := -1;
    Log('DllInstall ' + What + ' failed: ' + GetExceptionMessage);
  end;
  Log(Format('DllInstall(%d, %s) -> 0x%.8x', [Ord(Install), What, Result]));
end;

// After an update, Explorer still runs the old DLL: the new DLL restarts
// Explorer (its processes only) once the user clicks Finish, closing every
// File Explorer window; the Ready page says so beforehand. The shell comes back
// with no folder window (see restart_explorer.cpp). Run as the user, not
// elevated, like Explorer itself.
procedure RestartExplorer;
begin
  CallDllInstall(True, 'restartexplorer');
end;

procedure DeleteOldCopies;
var
  Found: TFindRec;
begin
  if FindFirst(ExpandConstant('{app}\PluginShellExtension.*.old'), Found) then begin
    try
      repeat
        DeleteFile(ExpandConstant('{app}\') + Found.Name);
      until not FindNext(Found);
    finally
      FindClose(Found);
    end;
  end;
end;

procedure MoveAside(const Path, Dir: String);
var
  Target: String;
begin
  if not FileExists(Path) then
    Exit;
  if DeleteFile(Path) then
    Exit;
  Target := AddBackslash(Dir) + 'PluginShellExtension.' + GetDateTimeString('yyyymmddhhnnsszzz', #0, #0) + '.old';
  if not RenameFile(Path, Target) then
    Log('Could not move aside ' + Path);
end;

// --- Machine-wide property handler (columns task) ---

function ColumnsRegistered: Boolean;
var
  Value: String;
begin
  Result := RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers\.esp', '', Value)
    and (CompareText(Value, '{#PropHandlerClsid}') = 0);
end;

// Runs regsvr32 /n /i:columns (or /u ...) on the installed DLL through a UAC
// prompt. Returns False if the prompt was declined or registration failed.
function RunColumnsRegistration(Install: Boolean): Boolean;
var
  Params: String;
  Code: Integer;
begin
  Params := '/s /n /i:columns "' + ExpandConstant('{app}\PluginShellExtension.dll') + '"';
  if not Install then
    Params := '/u ' + Params;
  Result := ShellExec('runas', ExpandConstant('{sys}\regsvr32.exe'), Params, '', SW_HIDE, ewWaitUntilTerminated, Code)
    and (Code = 0);
  Log(Format('regsvr32 %s -> ok=%d code=%d', [Params, Ord(Result), Code]));
end;

function HashMenuRegistered: Boolean;
begin
  Result := RegKeyExists(HKCU, 'Software\Classes\*\shellex\ContextMenuHandlers\PluginShellExtensionHashes');
end;

// Adds (or removes) it for this user: DllInstall with "hash".
procedure RunHashRegistration(Install: Boolean);
begin
  CallDllInstall(Install, 'hash');
end;

function FolderViewsRegistered: Boolean;
begin
  Result := RegKeyExists(HKCU, 'Software\Classes\Directory\Background\shellex\ContextMenuHandlers\PluginShellExtensionFolderViews');
end;

// Adds (or removes) it for this user: DllInstall with "views".
procedure RunFolderViewsRegistration(Install: Boolean);
begin
  CallDllInstall(Install, 'views');
end;

function PapyrusMenuRegistered: Boolean;
begin
  Result := RegKeyExists(HKCU, 'Software\Classes\SystemFileAssociations\.pex\shellex\ContextMenuHandlers\PluginShellExtensionPapyrus');
end;

// Adds (or removes) it for this user: DllInstall with "papyrus".
procedure RunPapyrusRegistration(Install: Boolean);
begin
  CallDllInstall(Install, 'papyrus');
end;

function BsaBrowseRegistered: Boolean;
begin
  Result := RegKeyExists(HKCU, 'Software\Classes\PluginShellExtension.BSA');
end;

// Adds (or removes) it for this user: DllInstall with "bsa".
procedure RunBsaRegistration(Install: Boolean);
begin
  CallDllInstall(Install, 'bsa');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then begin
    WasUpdate := FileExists(ExpandConstant('{app}\PluginShellExtension.dll'));
    DeleteOldCopies;
    MoveAside(ExpandConstant('{app}\PluginShellExtension.dll'), ExpandConstant('{app}'));
  end;
  if CurStep = ssPostInstall then begin
    if WizardIsTaskSelected('columns') then begin
      if not RunColumnsRegistration(True) then
        SuppressibleMsgBox('The Explorer columns could not be registered (administrator rights were declined or registration failed).' + #13#10#13#10 +
          'The right-click "Plugin properties" window is installed and works without them. Run setup again to retry.',
          mbInformation, MB_OK, IDOK);
    end else if ColumnsRegistered then
      RunColumnsRegistration(False);  // the task was unticked on an update
    if WizardIsTaskSelected('hashes') then
      RunHashRegistration(True)
    else if HashMenuRegistered then
      RunHashRegistration(False);
    if WizardIsTaskSelected('folderviews') then
      RunFolderViewsRegistration(True)
    else if FolderViewsRegistered then
      RunFolderViewsRegistration(False);
    if WizardIsTaskSelected('papyrus') then
      RunPapyrusRegistration(True)
    else if PapyrusMenuRegistered then
      RunPapyrusRegistration(False);
    if WizardIsTaskSelected('bsa') then
      RunBsaRegistration(True)
    else if BsaBrowseRegistered then
      RunBsaRegistration(False);
    if not WizardIsTaskSelected('hardlink') then begin  // unticked on an update: remove it
      RegDeleteKeyIncludingSubkeys(HKA, '{#HardlinkMenuKey}');
      RegDeleteKeyIncludingSubkeys(HKA, '{#HardlinkClsidKey}');
    end;
  end;
  // After Finish, when no setup window is left for anything to cover. The README
  // (when ticked) opens before this, in its own program.
  if (CurStep = ssDone) and WasUpdate then begin
    RestartExplorer;
    DeleteOldCopies;  // Explorer has let go of the old DLL now
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then begin
    // Unregister while the DLL and schema are still in place.
    if HashMenuRegistered then
      RunHashRegistration(False);
    if FolderViewsRegistered then
      RunFolderViewsRegistration(False);
    if PapyrusMenuRegistered then
      RunPapyrusRegistration(False);
    if BsaBrowseRegistered then
      RunBsaRegistration(False);
    if ColumnsRegistered then
      RunColumnsRegistration(False);
    // Moved to %TEMP% (same volume in the usual case) so the program folder
    // can be removed even while Explorer still holds the DLL.
    UnloadDLL(ExpandConstant('{app}\PluginShellExtension.dll'));
    MoveAside(ExpandConstant('{app}\PluginShellExtension.dll'), GetTempDir);
  end;
end;
