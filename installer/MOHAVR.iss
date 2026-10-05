; MOHAVR setup (Inno Setup 6). Built by tools\package.ps1, which passes the version:
;   ISCC.exe /DAppVersion=0.8.0 installer\MOHAVR.iss
; It installs MOHAVR's three files next to MOHA.exe (the game's UnrealEngine3\Binaries) and changes nothing of the
; game's: the same rules as release\install.ps1 -- the game found through Steam, a dinput8.dll that isn't MOHAVR's never
; overwritten, the game not running, a changed shipped MOHAVR.ini saved aside; the player's own settings
; (%LOCALAPPDATA%\MOHAVR) never touched, on install or uninstall.

#ifndef AppVersion
  #error Pass /DAppVersion=x.y.z (tools\package.ps1 does)
#endif

[Setup]
AppId={{546DE66D-2985-4665-AC46-5B89E0AB9D47}
AppName=MOHAVR
AppVersion={#AppVersion}
AppVerName=MOHAVR {#AppVersion}
AppPublisher=MOHAVR
AppComments=VR for Medal of Honor: Airborne (Steam)
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

// The game's folder: Steam's own uninstall entry for app 24840, then every Steam library; else the usual place.
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
end;

function GameDir(Param: String): String;
begin
  Result := FindGame();
  if Result = '' then Result := ExpandConstant('{commonpf32}\Steam\') + GameSub;
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

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Dll: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then begin
    if not HasGame(WizardDirValue) then begin
      MsgBox('This folder isn''t Medal of Honor: Airborne: there is no ' + ExeSub + ' in it.' + #13#10#13#10 +
             'Choose the game''s own folder (in Steam: right-click the game > Manage > Browse local files).', mbError, MB_OK);
      Result := False;
      exit;
    end;
    Dll := AddBackslash(WizardDirValue) + 'UnrealEngine3\Binaries\dinput8.dll';
    if FileExists(Dll) and not IsOurs(Dll) then begin
      MsgBox('There is already a dinput8.dll in the game''s Binaries folder that isn''t MOHAVR''s (another mod?).' + #13#10#13#10 +
             'Remove or rename it first: MOHAVR will not overwrite it. Nothing was changed.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if GameRunning() then Result := 'Medal of Honor: Airborne is running. Quit the game, then run setup again.';
end;

// An update: the shipped MOHAVR.ini changed -> the old copy is kept for the player (their own settings are elsewhere).
procedure CurStepChanged(CurStep: TSetupStep);
var
  Old, Keep: String;
begin
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
    MsgBox('Medal of Honor: Airborne is running. Quit the game, then uninstall MOHAVR.', mbError, MB_OK);
    Result := False;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Dll: String;
begin
  if CurUninstallStep = usUninstall then begin
    Dll := ExpandConstant('{app}\UnrealEngine3\Binaries\dinput8.dll');
    if FileExists(Dll) then begin
      if IsOurs(Dll) then DeleteFile(Dll)
      else Log('dinput8.dll is not MOHAVR''s any more -- left alone');
    end;
  end;
end;
