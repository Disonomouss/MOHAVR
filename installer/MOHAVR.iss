; MOHAVR setup (Inno Setup 6). Built by tools\package.ps1, which passes the version:
;   ISCC.exe /DAppVersion=0.8.0 installer\MOHAVR.iss
; It installs MOHAVR's three files next to MOHA.exe (the game's UnrealEngine3\Binaries) and changes nothing of the
; game's: the same rules as release\install.ps1 -- the game found through Steam, a dinput8.dll that isn't MOHAVR's never
; overwritten, the game not running, a changed shipped MOHAVR.ini saved aside; the player's own settings
; (%LOCALAPPDATA%\MOHAVR) never touched, on install or uninstall.
; The one exception, the player's choice (D57; the task "laa", ticked by default): MOHA.exe's large-address-aware flag, so
; the 32-bit game may use 4 GB instead of 2 GB (the first mission ran out in VR). One bit of the exe's header; the original
; exe kept in {autoappdata}\MOHAVR first; cleared again on uninstall or when the task is unticked on an update -- only a
; flag this setup set.

#ifndef AppVersion
  #error Pass /DAppVersion=x.y.z (tools\package.ps1 does)
#endif

[Setup]
AppId={{546DE66D-2985-4665-AC46-5B89E0AB9D47}
AppName=MOHAVR
AppVersion={#AppVersion}
AppVerName=MOHAVR {#AppVersion}
AppPublisher=MOHAVR
AppComments=VR for Medal of Honor: Airborne (Steam or the EA app)
DefaultDirName={code:GameDir}
AppendDefaultDirName=no
DirExistsWarning=no
UsePreviousAppDir=yes
DefaultGroupName=MOHAVR
DisableProgramGroupPage=yes
#ifdef TestBuild
; (tools' automated tests only: no UAC prompt; the Steam folder is writable by the player, as tools\deploy.ps1 shows)
PrivilegesRequired=lowest
OutputBaseFilename=MOHAVR-{#AppVersion}-Setup-test
#else
PrivilegesRequired=admin
OutputBaseFilename=MOHAVR-{#AppVersion}-Setup
#endif
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=MOHAVR {#AppVersion} (VR for Medal of Honor: Airborne)
UninstallDisplayIcon={app}\UnrealEngine3\Binaries\MOHA.exe
InfoBeforeFile=setup-info.txt
SetupLogging=yes
; The uninstaller beside the player's data, not in the game's folder.
UninstallFilesDir={autoappdata}\MOHAVR\uninstall

[Messages]
SelectDirLabel3=Setup will add MOHAVR to Medal of Honor: Airborne in the following folder (the game's own folder, the one that contains UnrealEngine3\Binaries\MOHA.exe).
SelectDirBrowseLabel=To continue, click Next. If this isn't your game's folder, click Browse.

[Tasks]
Name: "laa"; Description: "Let the game use up to 4 GB of memory (recommended: without it the first mission runs out of memory in VR). Sets one flag in MOHA.exe's header; the original is kept and put back on uninstall."; GroupDescription: "Memory:"

[Files]
; dinput8.dll is removed by the uninstall code only while it is still MOHAVR's (another mod may have replaced it).
Source: "..\build\x86\dinput8.dll"; DestDir: "{app}\UnrealEngine3\Binaries"; Flags: ignoreversion uninsneveruninstall
Source: "..\build\x64\MOHAVR-host.exe"; DestDir: "{app}\UnrealEngine3\Binaries"; Flags: ignoreversion
Source: "..\config\MOHAVR.ini"; DestDir: "{app}\UnrealEngine3\Binaries"; Flags: ignoreversion
Source: "..\release\README.md"; DestDir: "{app}\UnrealEngine3\Binaries"; DestName: "MOHAVR-README.md"; Flags: ignoreversion

[Icons]
Name: "{group}\MOHAVR README"; Filename: "{app}\UnrealEngine3\Binaries\MOHAVR-README.md"
Name: "{group}\MOHAVR settings (MOHAVR.ini)"; Filename: "{app}\UnrealEngine3\Binaries\MOHAVR.ini"
Name: "{group}\Uninstall MOHAVR"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\UnrealEngine3\Binaries\MOHAVR-README.md"; Description: "Read the README (controls and features)"; Flags: postinstall shellexec skipifsilent unchecked

[UninstallDelete]
Type: files; Name: "{app}\UnrealEngine3\Binaries\MOHAVR.log"
Type: files; Name: "{app}\UnrealEngine3\Binaries\MOHAVR.prev.log"
Type: files; Name: "{app}\UnrealEngine3\Binaries\MOHAVR-host.log"
Type: files; Name: "{app}\UnrealEngine3\Binaries\MOHAVR-host.prev.log"

[Code]
const
  GameSub = 'steamapps\common\Medal of Honor Airborne';
  ExeSub = 'UnrealEngine3\Binaries\MOHA.exe';
  EAGameKey = '{25F28E39-FDBB-11DB-8314-0800200C9A66}';  // the EA app's uninstall entry for the game (D58)
  Marker = 'MOHAVR-host.exe';  // MOHAVR's dinput8.dll carries this string (any version)

function HasGame(const Dir: String): Boolean;
begin
  Result := (Dir <> '') and FileExists(AddBackslash(Dir) + ExeSub);
end;

// The Steam libraries' "path" entries in steamapps\libraryfolders.vdf (backslashes doubled in the file).
procedure AddLibraries(const SteamDir: String; var Libs: TArrayOfString);
var
  Lines: TArrayOfString;
  I, P, N: Integer;
  S: String;
begin
  N := GetArrayLength(Libs);
  SetArrayLength(Libs, N + 1);
  Libs[N] := SteamDir;
  if not LoadStringsFromFile(AddBackslash(SteamDir) + 'steamapps\libraryfolders.vdf', Lines) then exit;
  for I := 0 to GetArrayLength(Lines) - 1 do begin
    S := Trim(Lines[I]);
    if Pos('"path"', S) = 1 then begin
      Delete(S, 1, 6);
      S := Trim(S);
      P := Pos('"', S);
      if P = 1 then begin
        Delete(S, 1, 1);
        P := Pos('"', S);
        if P > 0 then begin
          S := Copy(S, 1, P - 1);
          StringChangeEx(S, '\\', '\', True);
          N := GetArrayLength(Libs);
          SetArrayLength(Libs, N + 1);
          Libs[N] := S;
        end;
      end;
    end;
  end;
end;

// The game's folder: Steam's own uninstall entry for app 24840, then every Steam library, then the EA app's copy (D58: its
// own key and its uninstall entry); else the usual place.
function FindGame(): String;
var
  S: String;
  Libs: TArrayOfString;
  I: Integer;
begin
  Result := '';
  if RegQueryStringValue(HKLM32, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 24840', 'InstallLocation', S) and HasGame(S) then begin
    Result := S; exit;
  end;
  if RegQueryStringValue(HKLM64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 24840', 'InstallLocation', S) and HasGame(S) then begin
    Result := S; exit;
  end;
  SetArrayLength(Libs, 0);
  if RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', S) then begin
    StringChangeEx(S, '/', '\', True);
    AddLibraries(S, Libs);
  end;
  if RegQueryStringValue(HKLM32, 'SOFTWARE\Valve\Steam', 'InstallPath', S) then AddLibraries(S, Libs);
  for I := 0 to GetArrayLength(Libs) - 1 do
    if HasGame(AddBackslash(Libs[I]) + GameSub) then begin
      Result := AddBackslash(Libs[I]) + GameSub; exit;
    end;
  if RegQueryStringValue(HKLM32, 'SOFTWARE\Electronic Arts\Medal of Honor Airborne', 'Install Dir', S) and HasGame(S) then begin
    Result := RemoveBackslashUnlessRoot(S); exit;
  end;
  if RegQueryStringValue(HKLM32, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\' + EAGameKey, 'InstallLocation', S) and HasGame(S) then begin
    Result := RemoveBackslashUnlessRoot(S); exit;
  end;
end;

function GameDir(Param: String): String;
begin
  Result := FindGame();
  if Result = '' then Result := ExpandConstant('{commonpf32}\Steam\') + GameSub;
end;

// True if the command line names the folder (/DIR=...): that one is never replaced.
function DirOnCommandLine(): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(Copy(ParamStr(I), 1, 5), '/DIR=') = 0 then Result := True;
end;

// UsePreviousAppDir: the remembered folder of an earlier MOHAVR that no longer holds the game (the game moved, or Steam's
// copy was swapped for the EA app's) is replaced by the one found now. Only that folder: never one given with /DIR (a
// wrong one must still be refused on the folder page), and the player can still browse.
procedure InitializeWizard();
var
  S: String;
begin
  if (WizardForm.PrevAppDir <> '') and not DirOnCommandLine() and
     (CompareText(WizardForm.DirEdit.Text, WizardForm.PrevAppDir) = 0) and not HasGame(WizardForm.DirEdit.Text) then begin
    S := FindGame();
    if S <> '' then begin
      Log('The previous install''s folder ' + WizardForm.PrevAppDir + ' no longer holds the game -- ' + S + ' instead');
      WizardForm.DirEdit.Text := S;
    end;
  end;
end;

function IsOurs(const Dll: String): Boolean;
var
  Data: AnsiString;
begin
  Result := LoadStringFromFile(Dll, Data) and (Pos(Marker, Data) > 0);
end;

function GameRunning(): Boolean;
var
  Code: Integer;
begin
  Result := Exec(ExpandConstant('{cmd}'), '/C tasklist /FI "IMAGENAME eq MOHA.exe" /NH | find /I "MOHA.exe" >NUL', '', SW_HIDE,
                 ewWaitUntilTerminated, Code) and (Code = 0);
end;

// D57: MOHA.exe's large-address-aware flag (0x20 in the COFF header's Characteristics, at e_lfanew + 22).
function LaaOffset(S: TFileStream): Integer;
var
  B: AnsiString;
  Pe: Integer;
begin
  Result := -1;
  SetLength(B, 4);
  S.Seek($3C, soFromBeginning);
  S.ReadBuffer(B, 4);
  Pe := Ord(B[1]) + Ord(B[2]) * $100 + Ord(B[3]) * $10000 + Ord(B[4]) * $1000000;
  if (Pe <= 0) or (Pe > $10000) then exit;
  S.Seek(Pe, soFromBeginning);
  S.ReadBuffer(B, 4);
  if B <> 'PE' + #0 + #0 then exit;
  Result := Pe + 22;
end;

// The flag now (-1 unreadable, 0 off, 1 on); with Want >= 0 it is set to that first. True if it now matches Want.
function LaaFlag(const Exe: String; Want: Integer; var Now: Integer): Boolean;
var
  S: TFileStream;
  B: AnsiString;
  O, Ch: Integer;
begin
  Result := False;
  Now := -1;
  try
    if Want >= 0 then S := TFileStream.Create(Exe, fmOpenReadWrite or fmShareDenyWrite)
    else S := TFileStream.Create(Exe, fmOpenRead or fmShareDenyNone);
    try
      O := LaaOffset(S);
      if O < 0 then exit;
      SetLength(B, 2);
      S.Seek(O, soFromBeginning);
      S.ReadBuffer(B, 2);
      Ch := Ord(B[1]) + Ord(B[2]) * $100;
      if (Want >= 0) and (((Ch and $20) <> 0) <> (Want = 1)) then begin
        if Want = 1 then Ch := Ch or $20 else Ch := Ch and not $20;
        B[1] := Chr(Ch and $FF);
        B[2] := Chr((Ch shr 8) and $FF);
        S.Seek(O, soFromBeginning);
        S.WriteBuffer(B, 2);
      end;
      if (Ch and $20) <> 0 then Now := 1 else Now := 0;
      Result := (Want < 0) or (Now = Want);
    finally
      S.Free;
    end;
  except
    Log('MOHA.exe''s header: ' + GetExceptionMessage);
  end;
end;

function LaaMarker(): String;
begin
  Result := ExpandConstant('{autoappdata}\MOHAVR\laa-set.txt');
end;

// Applies the task: on -> the flag set (the original exe kept first, once; a marker that this setup set it), off -> a flag
// this setup set cleared again.
procedure ApplyLaa(const Exe: String; On: Boolean);
var
  Now: Integer;
  Keep: String;
begin
  LaaFlag(Exe, -1, Now);
  if On then begin
    if Now = 1 then begin Log('MOHA.exe is already large address aware'); exit; end;
    if Now < 0 then begin Log('MOHA.exe''s header is not readable -- the 4 GB option is skipped'); exit; end;
    Keep := ExpandConstant('{autoappdata}\MOHAVR');
    ForceDirectories(Keep);
    if not FileExists(Keep + '\MOHA.exe.original') then FileCopy(Exe, Keep + '\MOHA.exe.original', False);
    if LaaFlag(Exe, 1, Now) then begin
      SaveStringToFile(LaaMarker(), Exe, False);
      Log('MOHA.exe: large address aware set (4 GB); the original kept as ' + Keep + '\MOHA.exe.original');
    end else
      SuppressibleMsgBox('Setup could not change MOHA.exe (is the game running, or the file read-only?). MOHAVR is installed, but the ' +
             'game keeps its 2 GB of memory: the first mission may run out in VR. Run setup again to retry.', mbInformation, MB_OK, IDOK);
  end else if FileExists(LaaMarker()) then begin
    if LaaFlag(Exe, 0, Now) then Log('MOHA.exe: large address aware cleared (the option was unticked)');
    DeleteFile(LaaMarker());
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Dll: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then begin
    if not HasGame(WizardDirValue) then begin
      SuppressibleMsgBox('This folder isn''t Medal of Honor: Airborne: there is no ' + ExeSub + ' in it.' + #13#10#13#10 +
             'Choose the game''s own folder (in Steam: right-click the game > Manage > Browse local files; in the EA app: ' +
             'the game''s ... menu > View properties > Browse).', mbError, MB_OK, IDOK);
      Result := False;
      exit;
    end;
    Dll := AddBackslash(WizardDirValue) + 'UnrealEngine3\Binaries\dinput8.dll';
    if FileExists(Dll) and not IsOurs(Dll) then begin
      SuppressibleMsgBox('There is already a dinput8.dll in the game''s Binaries folder that isn''t MOHAVR''s (another mod?).' + #13#10#13#10 +
             'Remove or rename it first: MOHAVR will not overwrite it. Nothing was changed.', mbError, MB_OK, IDOK);
      Result := False;
    end;
  end;
end;

// Runs on every install, also when the folder page was skipped (an update: DisableDirPage=auto), so the folder checks are
// repeated here.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Dll: String;
begin
  Result := '';
  Dll := AddBackslash(WizardDirValue) + 'UnrealEngine3\Binaries\dinput8.dll';
  if not HasGame(WizardDirValue) then
    Result := WizardDirValue + ' isn''t Medal of Honor: Airborne: there is no ' + ExeSub + ' in it. Nothing was changed.'
  else if FileExists(Dll) and not IsOurs(Dll) then
    Result := 'There is already a dinput8.dll in the game''s Binaries folder that isn''t MOHAVR''s (another mod?). ' +
              'Remove or rename it first: MOHAVR will not overwrite it. Nothing was changed.'
  else if GameRunning() then Result := 'Medal of Honor: Airborne is running. Quit the game, then run setup again.';
end;

// An update: the shipped MOHAVR.ini changed -> the old copy is kept for the player (their own settings are elsewhere).
procedure CurStepChanged(CurStep: TSetupStep);
var
  Old, Keep: String;
begin
  if CurStep = ssPostInstall then
    ApplyLaa(AddBackslash(WizardDirValue) + 'UnrealEngine3\Binaries\MOHA.exe', WizardIsTaskSelected('laa'));
  if CurStep = ssInstall then begin
    Old := AddBackslash(WizardDirValue) + 'UnrealEngine3\Binaries\MOHAVR.ini';
    if FileExists(Old) then begin
      ExtractTemporaryFile('MOHAVR.ini');
      if GetSHA256OfFile(Old) <> GetSHA256OfFile(ExpandConstant('{tmp}\MOHAVR.ini')) then begin
        Keep := ExpandConstant('{localappdata}\MOHAVR');
        ForceDirectories(Keep);
        if FileCopy(Old, Keep + '\MOHAVR.ini.previous', False) then
          Log('The previous MOHAVR.ini was saved to ' + Keep + '\MOHAVR.ini.previous');
      end;
    end;
  end;
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;
  if GameRunning() then begin
    SuppressibleMsgBox('Medal of Honor: Airborne is running. Quit the game, then uninstall MOHAVR.', mbError, MB_OK, IDOK);
    Result := False;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Dll: String;
  Now: Integer;
begin
  if CurUninstallStep = usUninstall then begin
    // D57: a large-address-aware flag this setup set is cleared; the kept original goes too.
    if FileExists(LaaMarker()) then begin
      if LaaFlag(ExpandConstant('{app}\UnrealEngine3\Binaries\MOHA.exe'), 0, Now) then Log('MOHA.exe: large address aware cleared');
      DeleteFile(LaaMarker());
    end;
    DeleteFile(ExpandConstant('{autoappdata}\MOHAVR\MOHA.exe.original'));
    Dll := ExpandConstant('{app}\UnrealEngine3\Binaries\dinput8.dll');
    if FileExists(Dll) then begin
      if IsOurs(Dll) then DeleteFile(Dll)
      else Log('dinput8.dll is not MOHAVR''s any more -- left alone');
    end;
  end;
end;
