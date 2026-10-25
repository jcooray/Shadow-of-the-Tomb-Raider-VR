; Shadow of the Tomb Raider VR -- installer (Inno Setup 7). Build with tools/make_installer.py, which stages the files
; into installer\stage and runs ISCC. The installer:
;   - finds the game (Steam libraries, Epic and GOG install records; otherwise Browse) and checks SOTTR.exe is there;
;   - moves a dxgi.dll / version.dll from another mod aside (restored on uninstall);
;   - installs the mod (dxgi.dll) and, if chosen, CheekyFoveatedDLSS foveated rendering (version.dll loader + its folder),
;     fixed or eye tracked -- vrmod_settings.ini [foveation] mode, and the sharp region's size in Cheeky's ini -- and with
;     eye tracking optionally DLSS 5: NVIDIA's nvngx_dlssnr.dll is never shipped; Setup copies one the PC already has
;     (from another game) into the game folder, and removes only that copy again;
;   - uninstalls cleanly: our files and logs, optionally the settings, the moved files back, and the game's saved
;     fullscreen resolution reset to the desktop's (the mod sets it to the headset's size).

#define AppName "Shadow of the Tomb Raider VR"
#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#define AppUrl "https://github.com/jcooray/Shadow-of-the-Tomb-Raider-VR"

[Setup]
AppId={{6E4C2B1F-8D3A-4F5E-9B27-4C1A9E6D3F80}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=jcooray
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
; The game folder is the install folder; the wizard shows the detected one and lets the user browse.
DefaultDirName={code:DetectedGameDir}
DirExistsWarning=no
AppendDefaultDirName=no
DisableProgramGroupPage=yes
DisableDirPage=no
UsePreviousAppDir=yes
; Steam's default library is under Program Files.
PrivilegesRequired=admin
; /CURRENTUSER on the command line installs without admin rights (a game folder outside Program Files; tests).
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=stage\LICENSE.txt
InfoBeforeFile=stage\before_install.txt
OutputDir=output
OutputBaseFilename=Shadow-of-the-Tomb-Raider-VR-{#AppVersion}-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Room for the foveated rendering page's descriptions.
WizardSizePercent=130
UninstallDisplayName={#AppName}
UninstallFilesDir={app}\SOTTR-VR\uninstall
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} installer

[Files]
Source: "stage\dxgi.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "stage\README.txt"; DestDir: "{app}\SOTTR-VR"; Flags: ignoreversion isreadme
Source: "stage\SOTTR-VR-restart.exe"; DestDir: "{app}\SOTTR-VR"; Flags: ignoreversion
Source: "stage\LICENSE.txt"; DestDir: "{app}\SOTTR-VR"; Flags: ignoreversion
Source: "stage\THIRD_PARTY_NOTICES.md"; DestDir: "{app}\SOTTR-VR"; Flags: ignoreversion
Source: "stage\cheeky\version.dll"; DestDir: "{app}"; Check: FoveationOn; Flags: ignoreversion
Source: "stage\cheeky\CheekyFoveatedDLSSHost.dll"; DestDir: "{app}\CheekyFoveatedDLSS"; Check: FoveationOn; Flags: ignoreversion
Source: "stage\cheeky\CheekyFoveatedDLSSRuntime.dll"; DestDir: "{app}\CheekyFoveatedDLSS"; Check: FoveationOn; Flags: ignoreversion
Source: "stage\cheeky\CheekyFoveatedDLSS.ini"; DestDir: "{app}\CheekyFoveatedDLSS"; Check: FoveationOn; Flags: onlyifdoesntexist uninsneveruninstall
Source: "stage\cheeky\CheekyOverlay.ini"; DestDir: "{app}\CheekyFoveatedDLSS"; Check: FoveationOn; Flags: onlyifdoesntexist uninsneveruninstall
Source: "stage\cheeky\CHEEKY_LICENSE.txt"; DestDir: "{app}\SOTTR-VR"; Check: FoveationOn; Flags: ignoreversion

[INI]
; Foveated rendering for the mod (mode off / fixed / eye, dlss5 0 / 1); the mod writes the rest of vrmod_settings.ini at
; its first start. Cheeky's ini: on, the sharp region's size for the mode (as the mod's overlay writes it: gpu_tier.cpp
; FoveaSize), DLSS 5.
Filename: "{app}\vrmod_settings.ini"; Section: "foveation"; Key: "mode"; String: "{code:FoveationMode}"
Filename: "{app}\vrmod_settings.ini"; Section: "foveation"; Key: "dlss5"; String: "{code:Dlss5Value}"
Filename: "{app}\CheekyFoveatedDLSS\CheekyFoveatedDLSS.ini"; Section: "CheekyFoveatedDLSS"; Key: "Enabled"; String: "1"; Check: FoveationOn
Filename: "{app}\CheekyFoveatedDLSS\CheekyFoveatedDLSS.ini"; Section: "CheekyFoveatedDLSS"; Key: "Width"; String: "{code:FoveaSize|w}"; Check: FoveationOn
Filename: "{app}\CheekyFoveatedDLSS\CheekyFoveatedDLSS.ini"; Section: "CheekyFoveatedDLSS"; Key: "Height"; String: "{code:FoveaSize|h}"; Check: FoveationOn
Filename: "{app}\CheekyFoveatedDLSS\CheekyFoveatedDLSS.ini"; Section: "CheekyFoveatedDLSS"; Key: "NrEnabled"; String: "{code:Dlss5Value}"; Check: FoveationOn

[UninstallDelete]
Type: files; Name: "{app}\vrmod.log"
Type: files; Name: "{app}\vrmod.prev1.log"
Type: files; Name: "{app}\vrmod.prev2.log"
Type: files; Name: "{app}\vrmod.prev3.log"
Type: files; Name: "{app}\vrmod_settings.ini.tmp"
Type: files; Name: "{app}\CheekyFoveatedDLSS\*.log"
Type: dirifempty; Name: "{app}\CheekyFoveatedDLSS"
Type: dirifempty; Name: "{app}\SOTTR-VR"

[Messages]
SelectDirLabel3=Setup found Shadow of the Tomb Raider in the folder below. If that's not where you play it from, click Browse and choose the folder that contains SOTTR.exe.
FinishedLabel=Shadow of the Tomb Raider VR is installed.%n%nStart your headset's VR software (e.g. SteamVR), then the game as usual. The first start works out the right resolution for your headset and graphics card, then asks once a level has loaded to restart the game to apply it (press A, then confirm the game's quit message).%n%nIn the headset, hold Guide (the Xbox button) and press Menu for the mod's settings.

[Code]
const
  GameExe = 'SOTTR.exe';
  BackupSuffix = '.before-sottrvr';
  GraphicsKey = 'Software\Eidos Montreal\Shadow of the Tomb Raider\Graphics';

  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6E4C2B1F-8D3A-4F5E-9B27-4C1A9E6D3F80}_is1';

var
  FoveationPage: TWizardPage;
  FovNone, FovFixed, FovEye: TNewRadioButton;
  Dlss5Check: TNewCheckBox;
  Dlss5Status: TNewStaticText;
  FoveationShown: Boolean;
  Dlss5Wanted, Dlss5Updating: Boolean;   { the player's DLSS 5 choice, kept while the box is unavailable }
  Dlss5Source: String;             { the nvngx_dlssnr.dll DLSS 5 would use; '' = none found }
  Dlss5MS, Dlss5LS: Cardinal;      { its version }
  CachedGameDir: String;
  KeepSettings: Boolean;

{ ---- Already installed: update or uninstall ---- }

{ An earlier install's uninstaller and folder, from its Installed apps entry (an admin install's in HKLM, a /CURRENTUSER
  one's in HKCU). }
function FindInstalled(var Uninstaller, Folder: String): Boolean;
var
  Roots: array of Integer;
  I: Integer;
begin
  Result := False;
  SetArrayLength(Roots, 3);
  Roots[0] := HKLM64;
  Roots[1] := HKLM32;
  Roots[2] := HKCU;
  for I := 0 to GetArrayLength(Roots) - 1 do
    if RegQueryStringValue(Roots[I], UninstallKey, 'UninstallString', Uninstaller) then begin
      Uninstaller := RemoveQuotes(Uninstaller);
      if not RegQueryStringValue(Roots[I], UninstallKey, 'InstallLocation', Folder) then Folder := '';
      Result := FileExists(Uninstaller);
      if Result then Exit;
    end;
end;

{ Running Setup again on an installed mod: ask whether to update / change it (the wizard, in place, settings kept) or to
  uninstall it (hands over to the uninstaller, which asks about the settings). Silent installs just update. }
function InitializeSetup: Boolean;
var
  Uninstaller, Folder: String;
  Code: Integer;
begin
  Result := True;
  if WizardSilent or not FindInstalled(Uninstaller, Folder) then Exit;
  case TaskDialogMsgBox('{#AppName} is already installed.',
         'In: ' + RemoveBackslashUnlessRoot(Folder) + #13#10#13#10 +
         'Update it to {#AppVersion} or change what is installed (your settings are kept), or uninstall it?',
         mbConfirmation, MB_YESNO, ['Update or change', 'Uninstall'], 0) of
    IDYES: Exit;
    IDNO: begin
      if not Exec(Uninstaller, '', '', SW_SHOWNORMAL, ewNoWait, Code) then
        MsgBox('Could not start the uninstaller: ' + SysErrorMessage(Code) + #13#10#13#10 +
               'Uninstall from Windows Settings > Apps > Installed apps instead.', mbError, MB_OK);
      Result := False;
    end;
  else
    Result := False;
  end;
end;

{ ---- Game detection ---- }

function Unescape(const S: String): String;
begin
  Result := S;
  StringChangeEx(Result, '\\', '\', True);
end;

{ The value of "key": "..." (JSON) or "key"  "..." (Valve KeyValues) on a line; empty when the line has no such key. }
function QuotedValue(const Line, Key: String): String;
var
  P, Q: Integer;
  Rest: String;
begin
  Result := '';
  P := Pos('"' + Key + '"', Line);
  if P = 0 then Exit;
  Rest := Copy(Line, P + Length(Key) + 2, MaxInt);
  P := Pos('"', Rest);
  if P = 0 then Exit;
  Rest := Copy(Rest, P + 1, MaxInt);
  Q := Pos('"', Rest);
  if Q = 0 then Exit;
  Result := Unescape(Copy(Rest, 1, Q - 1));
end;

function IsGameDir(const Dir: String): Boolean;
begin
  Result := (Dir <> '') and FileExists(AddBackslash(Dir) + GameExe);
end;

function FindSteam: String;
var
  SteamPath, Lib, Dir: String;
  Lines: TArrayOfString;
  I: Integer;
begin
  Result := '';
  if not RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', SteamPath) then Exit;
  StringChangeEx(SteamPath, '/', '\', True);
  { the Steam folder itself, then every library in libraryfolders.vdf }
  Dir := AddBackslash(SteamPath) + 'steamapps\common\Shadow of the Tomb Raider';
  if IsGameDir(Dir) then begin Result := Dir; Exit; end;
  if LoadStringsFromFile(AddBackslash(SteamPath) + 'steamapps\libraryfolders.vdf', Lines) then
    for I := 0 to GetArrayLength(Lines) - 1 do begin
      Lib := QuotedValue(Lines[I], 'path');
      if Lib <> '' then begin
        Dir := AddBackslash(Lib) + 'steamapps\common\Shadow of the Tomb Raider';
        if IsGameDir(Dir) then begin Result := Dir; Exit; end;
      end;
    end;
end;

function FindEpic: String;
var
  FR: TFindRec;
  ManifestDir, Name, Loc: String;
  Lines: TArrayOfString;
  I: Integer;
begin
  Result := '';
  ManifestDir := ExpandConstant('{commonappdata}') + '\Epic\EpicGamesLauncher\Data\Manifests\';
  if FindFirst(ManifestDir + '*.item', FR) then
  try
    repeat
      if LoadStringsFromFile(ManifestDir + FR.Name, Lines) then begin
        Name := '';
        Loc := '';
        for I := 0 to GetArrayLength(Lines) - 1 do begin
          if Name = '' then Name := QuotedValue(Lines[I], 'DisplayName');
          if Loc = '' then Loc := QuotedValue(Lines[I], 'InstallLocation');
        end;
        if (Pos('Shadow of the Tomb Raider', Name) > 0) and IsGameDir(Loc) then begin Result := Loc; Exit; end;
      end;
    until not FindNext(FR);
  finally
    FindClose(FR);
  end;
end;

function FindGog: String;
var
  Keys: TArrayOfString;
  I: Integer;
  Name, Dir: String;
begin
  Result := '';
  if RegGetSubkeyNames(HKLM32, 'SOFTWARE\GOG.com\Games', Keys) then
    for I := 0 to GetArrayLength(Keys) - 1 do
      if RegQueryStringValue(HKLM32, 'SOFTWARE\GOG.com\Games\' + Keys[I], 'gameName', Name) and
         (Pos('Shadow of the Tomb Raider', Name) > 0) and
         RegQueryStringValue(HKLM32, 'SOFTWARE\GOG.com\Games\' + Keys[I], 'path', Dir) and IsGameDir(Dir) then begin
        Result := Dir;
        Exit;
      end;
end;

function DetectedGameDir(Param: String): String;
begin
  if CachedGameDir = '' then begin
    CachedGameDir := FindSteam;
    if CachedGameDir = '' then CachedGameDir := FindEpic;
    if CachedGameDir = '' then CachedGameDir := FindGog;
    if CachedGameDir = '' then CachedGameDir := ExpandConstant('{commonpf32}') + '\Steam\steamapps\common\Shadow of the Tomb Raider';
  end;
  Result := CachedGameDir;
end;

{ ---- Wizard ---- }

{ ---- DLSS 5: an nvngx_dlssnr.dll already on the PC ---- }

{ The newest nvngx_dlssnr.dll in Dir or up to Depth folder levels below it. }
procedure SearchDlssnr(const Dir: String; Depth: Integer);
var
  FR: TFindRec;
  MS, LS: Cardinal;
begin
  if FileExists(Dir + '\nvngx_dlssnr.dll') and GetVersionNumbers(Dir + '\nvngx_dlssnr.dll', MS, LS) then
    if (Dlss5Source = '') or (MS > Dlss5MS) or ((MS = Dlss5MS) and (LS > Dlss5LS)) then begin
      Dlss5Source := Dir + '\nvngx_dlssnr.dll';
      Dlss5MS := MS;
      Dlss5LS := LS;
    end;
  if (Depth > 0) and FindFirst(Dir + '\*', FR) then
  try
    repeat
      if ((FR.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0) and (FR.Name <> '.') and (FR.Name <> '..') then
        SearchDlssnr(Dir + '\' + FR.Name, Depth - 1);
    until not FindNext(FR);
  finally
    FindClose(FR);
  end;
end;

{ One beside the game is used as it is; otherwise the newest in the other games of the Steam libraries, Epic, GOG and
  the Xbox app (games that ship DLSS 5, e.g. Crimson Desert or No Man's Sky). }
procedure FindDlss5;
var
  SteamPath, Lib, ManifestDir, Loc, Dir: String;
  Lines, Keys: TArrayOfString;
  FR: TFindRec;
  I, Drive: Integer;
begin
  Dlss5Source := '';
  Dir := ExpandConstant('{app}');
  if FileExists(Dir + '\nvngx_dlssnr.dll') then begin
    Dlss5Source := Dir + '\nvngx_dlssnr.dll';
    if not GetVersionNumbers(Dlss5Source, Dlss5MS, Dlss5LS) then begin Dlss5MS := 0; Dlss5LS := 0; end;
    Exit;
  end;
  if RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', SteamPath) then begin
    StringChangeEx(SteamPath, '/', '\', True);
    SearchDlssnr(AddBackslash(SteamPath) + 'steamapps\common', 3);
    if LoadStringsFromFile(AddBackslash(SteamPath) + 'steamapps\libraryfolders.vdf', Lines) then
      for I := 0 to GetArrayLength(Lines) - 1 do begin
        Lib := QuotedValue(Lines[I], 'path');
        if (Lib <> '') and (CompareText(RemoveBackslashUnlessRoot(Lib), RemoveBackslashUnlessRoot(SteamPath)) <> 0) then
          SearchDlssnr(AddBackslash(Lib) + 'steamapps\common', 3);
      end;
  end;
  ManifestDir := ExpandConstant('{commonappdata}') + '\Epic\EpicGamesLauncher\Data\Manifests\';
  if FindFirst(ManifestDir + '*.item', FR) then
  try
    repeat
      if LoadStringsFromFile(ManifestDir + FR.Name, Lines) then
        for I := 0 to GetArrayLength(Lines) - 1 do begin
          Loc := QuotedValue(Lines[I], 'InstallLocation');
          if Loc <> '' then SearchDlssnr(RemoveBackslashUnlessRoot(Loc), 2);
        end;
    until not FindNext(FR);
  finally
    FindClose(FR);
  end;
  if RegGetSubkeyNames(HKLM32, 'SOFTWARE\GOG.com\Games', Keys) then
    for I := 0 to GetArrayLength(Keys) - 1 do
      if RegQueryStringValue(HKLM32, 'SOFTWARE\GOG.com\Games\' + Keys[I], 'path', Dir) then
        SearchDlssnr(RemoveBackslashUnlessRoot(Dir), 2);
  for Drive := Ord('C') to Ord('Z') do
    if DirExists(Chr(Drive) + ':\XboxGames') then SearchDlssnr(Chr(Drive) + ':\XboxGames', 3);
end;

function Dlss5Version: String;
begin
  Result := Format('%d.%d.%d', [Dlss5MS shr 16, Dlss5MS and $FFFF, Dlss5LS shr 16]);
end;

{ ---- Wizard ---- }

function AddText(const Caption: String; Left, Top: Integer): TNewStaticText;
begin
  Result := TNewStaticText.Create(FoveationPage);
  Result.Parent := FoveationPage.Surface;
  Result.AutoSize := False;
  Result.WordWrap := True;
  Result.Left := Left;
  Result.Top := Top;
  Result.Width := FoveationPage.SurfaceWidth - Left;
  Result.Caption := Caption;
  Result.AdjustHeight;
end;

function AddRadio(const Caption: String; Top: Integer): TNewRadioButton;
begin
  Result := TNewRadioButton.Create(FoveationPage);
  Result.Parent := FoveationPage.Surface;
  Result.Top := Top;
  Result.Width := FoveationPage.SurfaceWidth;
  Result.Height := ScaleY(17);
  Result.Caption := Caption;
  Result.Font.Style := [fsBold];
end;

{ DLSS 5 runs with eye tracked foveated rendering only, and needs the runtime: unavailable, its box is greyed and
  empty; the player's choice comes back with eye tracked. }
procedure UpdateDlss5(Sender: TObject);
begin
  Dlss5Updating := True;
  Dlss5Check.Enabled := FovEye.Checked and (Dlss5Source <> '');
  Dlss5Check.Checked := Dlss5Check.Enabled and Dlss5Wanted;
  Dlss5Updating := False;
end;

procedure Dlss5Clicked(Sender: TObject);
begin
  if not Dlss5Updating then Dlss5Wanted := Dlss5Check.Checked;
end;

procedure InitializeWizard;
var
  Indent: Integer;
  T: TNewStaticText;
begin
  FoveationPage := CreateCustomPage(wpSelectDir, 'Foveated rendering',
    'How much of your view is drawn at full quality. You can change it later in the mod''s overlay (Rendering page).');
  Indent := ScaleX(18);
  FovNone := AddRadio('No foveated rendering', 0);
  T := AddText('Your whole view is drawn at full quality. Recommended without eye tracking, where foveated rendering ' +
    'means turning your head, not just your eyes, to see sharply.', Indent, FovNone.Top + FovNone.Height + ScaleY(2));
  FovFixed := AddRadio('Fixed foveated rendering', T.Top + T.Height + ScaleY(10));
  T := AddText('The middle of your view stays at full quality and the edges are slightly softer. It saves very little ' +
    'GPU time, so the automatic resolution stays about the same.', Indent, FovFixed.Top + FovFixed.Height + ScaleY(2));
  FovEye := AddRadio('Eye tracked foveated rendering', T.Top + T.Height + ScaleY(10));
  T := AddText('Wherever you look stays at full quality and the rest is slightly softer, freeing about 15 % of GPU time, ' +
    'so the automatic resolution is about 30 % higher. Needs eye tracking that reaches SteamVR or OpenXR.',
    Indent, FovEye.Top + FovEye.Height + ScaleY(2));
  Dlss5Check := TNewCheckBox.Create(FoveationPage);
  Dlss5Check.Parent := FoveationPage.Surface;
  Dlss5Check.Left := Indent;
  Dlss5Check.Top := T.Top + T.Height + ScaleY(8);
  Dlss5Check.Width := FoveationPage.SurfaceWidth - Indent;
  Dlss5Check.Height := ScaleY(17);
  Dlss5Check.Caption := 'DLSS 5 neural rendering (experimental)';
  T := AddText('A more lifelike image. It uses about as much GPU time as eye tracking saves, so the automatic ' +
    'resolution stays as without foveated rendering.', Indent * 2, Dlss5Check.Top + Dlss5Check.Height + ScaleY(2));
  Dlss5Status := AddText('', Indent * 2, T.Top + T.Height + ScaleY(4));
  FovNone.Checked := True;
  FovNone.OnClick := @UpdateDlss5;
  FovFixed.OnClick := @UpdateDlss5;
  FovEye.OnClick := @UpdateDlss5;
  Dlss5Check.OnClick := @Dlss5Clicked;
end;

{ Once the game folder is known (the page's first showing, or before installing when silent): the earlier install's
  choice, and the DLSS 5 runtime. }
procedure InitFoveationChoice;
var
  Ini, Mode: String;
begin
  if FoveationShown then Exit;
  FoveationShown := True;
  Ini := ExpandConstant('{app}\vrmod_settings.ini');
  Mode := GetIniString('foveation', 'mode', '', Ini);
  if FileExists(ExpandConstant('{app}\CheekyFoveatedDLSS\CheekyFoveatedDLSSRuntime.dll')) then begin
    if (Mode = 'eye') or (Mode = '2') then FovEye.Checked := True
    else if (Mode = 'fixed') or (Mode = '1') then FovFixed.Checked := True;
  end;
  Dlss5Wanted := GetIniString('foveation', 'dlss5', '0', Ini) = '1';
  { Command line (silent installs, tests): /FOVEATION=none|fixed|eye /DLSS5=0|1 }
  Mode := ExpandConstant('{param:FOVEATION|}');
  if Mode = 'none' then FovNone.Checked := True
  else if Mode = 'fixed' then FovFixed.Checked := True
  else if Mode = 'eye' then FovEye.Checked := True;
  Mode := ExpandConstant('{param:DLSS5|}');
  if Mode <> '' then Dlss5Wanted := Mode = '1';
  Dlss5Status.Caption := 'Looking for NVIDIA''s DLSS 5 file on this PC...';
  Dlss5Status.AdjustHeight;
  WizardForm.Refresh;
  FindDlss5;
  if Dlss5Source = '' then
    Dlss5Status.Caption := 'Needs NVIDIA''s nvngx_dlssnr.dll, which comes with some recent games (e.g. Crimson Desert, ' +
      'No Man''s Sky); none was found on this PC.'
  else if CompareText(Dlss5Source, ExpandConstant('{app}\nvngx_dlssnr.dll')) = 0 then
    Dlss5Status.Caption := 'Uses the nvngx_dlssnr.dll ' + Dlss5Version + ' already in the game folder.'
  else
    Dlss5Status.Caption := 'Copies nvngx_dlssnr.dll ' + Dlss5Version + ' from ' + ExtractFileDir(Dlss5Source) +
      ' into the game folder. It has to suit your graphics card (NVIDIA''s current build: RTX 50 series).';
  Dlss5Status.AdjustHeight;
  UpdateDlss5(nil);
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = FoveationPage.ID then InitFoveationChoice;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectDir then
    if not IsGameDir(WizardDirValue) then begin
      SuppressibleMsgBox('SOTTR.exe isn''t in this folder:' + #13#10 + WizardDirValue + #13#10#13#10 +
             'Choose the folder Shadow of the Tomb Raider is installed in (the one that contains SOTTR.exe).', mbError, MB_OK, IDOK);
      Result := False;
    end;
end;

function FoveationMode(Param: String): String;
begin
  if FovEye.Checked then Result := 'eye'
  else if FovFixed.Checked then Result := 'fixed'
  else Result := 'off';
end;

function FoveationOn: Boolean;
begin
  Result := not FovNone.Checked;
end;

function Dlss5On: Boolean;
begin
  Result := FovEye.Checked and (Dlss5Source <> '') and Dlss5Check.Checked;
end;

function Dlss5Value(Param: String): String;
begin
  if Dlss5On then Result := '1' else Result := '0';
end;

{ Cheeky's sharp region (Width / Height): large when fixed, small when it follows the eyes (the mod's FoveaSize). }
function FoveaSize(Param: String): String;
begin
  if FovFixed.Checked then begin
    if Param = 'w' then Result := '0.55' else Result := '0.45';
  end else
    Result := '0.20';
end;

{ The nvngx_dlssnr.dll Setup copied into the game folder (a marker file next to the readme names its source). }
procedure RemoveCopiedDlss5;
var
  Dir: String;
begin
  Dir := ExpandConstant('{app}\');
  if FileExists(Dir + 'SOTTR-VR\nvngx_dlssnr.copied') then begin
    if DeleteFile(Dir + 'nvngx_dlssnr.dll') then Log('Removed the nvngx_dlssnr.dll Setup had copied');
    DeleteFile(Dir + 'SOTTR-VR\nvngx_dlssnr.copied');
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Dir: String;
begin
  if CurStep <> ssPostInstall then Exit;
  Dir := ExpandConstant('{app}\');
  if not Dlss5On then
    RemoveCopiedDlss5
  else if CompareText(Dlss5Source, Dir + 'nvngx_dlssnr.dll') <> 0 then begin
    if FileCopy(Dlss5Source, Dir + 'nvngx_dlssnr.dll', False) then begin
      SaveStringToFile(Dir + 'SOTTR-VR\nvngx_dlssnr.copied', Dlss5Source, False);
      Log('Copied nvngx_dlssnr.dll from ' + Dlss5Source);
    end else
      SuppressibleMsgBox('Could not copy nvngx_dlssnr.dll from ' + Dlss5Source + '. DLSS 5 stays off until it is in the game folder.',
        mbError, MB_OK, IDOK);
  end;
end;

{ A dxgi.dll / version.dll that isn't ours (another mod: ReShade, OptiScaler, Special K, ...) is moved aside, and put back
  when the mod is uninstalled. }
{ Ours: the file contains a name only our builds have -- the mod's settings file name vrmod_settings.ini (in every build; its
  Cheeky gaze export names are shared with other mods), or
  the export CheekyBootstrap_Status in Cheeky's loader (stored as plain text; the folder name in it is UTF-16). }
function IsOurs(const Path, Marker: String): Boolean;
var
  S: AnsiString;
begin
  Result := LoadStringFromFile(Path, S) and (Pos(AnsiString(Marker), S) > 0);
end;

procedure MoveAside(const Name, OurProduct: String);
var
  Path: String;
begin
  Path := ExpandConstant('{app}\') + Name;
  if FileExists(Path) and not IsOurs(Path, OurProduct) and not FileExists(Path + BackupSuffix) then begin
    if RenameFile(Path, Path + BackupSuffix) then
      Log('Moved another ' + Name + ' aside: ' + Path + BackupSuffix)
    else
      SuppressibleMsgBox('Could not move the existing ' + Name + ' aside. Close the game and any mod tools, then run Setup again.', mbError, MB_OK, IDOK);
  end;
end;

procedure RestoreAside(const Name: String);
var
  Path: String;
begin
  Path := ExpandConstant('{app}\') + Name;
  if FileExists(Path + BackupSuffix) and not FileExists(Path) then RenameFile(Path + BackupSuffix, Path);
end;

{ Reinstalling over an earlier install upgrades it in place (same AppId). Foveated rendering left out this time: remove
  the earlier install's Cheeky files, which would otherwise keep loading, and put back a version.dll that Setup moved
  aside for it. Its settings (the two ini files) stay for a later reinstall. }
procedure RemoveFoveation;
var
  Dir: String;
begin
  Dir := ExpandConstant('{app}\');
  if FileExists(Dir + 'version.dll') and IsOurs(Dir + 'version.dll', 'CheekyBootstrap_Status') then begin
    if DeleteFile(Dir + 'version.dll') then Log('Foveated rendering not selected: removed version.dll (CheekyFoveatedDLSS)');
    DeleteFile(Dir + 'CheekyFoveatedDLSS\CheekyFoveatedDLSSHost.dll');
    DeleteFile(Dir + 'CheekyFoveatedDLSS\CheekyFoveatedDLSSRuntime.dll');
    DeleteFile(Dir + 'SOTTR-VR\CHEEKY_LICENSE.txt');
  end;
  RestoreAside('version.dll');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Other: String;
begin
  Result := '';
  Other := '';
  InitFoveationChoice;   { a silent install never shows the page }
  if FileExists(ExpandConstant('{app}\dxgi.dll')) and not IsOurs(ExpandConstant('{app}\dxgi.dll'), 'vrmod_settings.ini') then
    Other := 'dxgi.dll';
  if FoveationOn and FileExists(ExpandConstant('{app}\version.dll')) and
     not IsOurs(ExpandConstant('{app}\version.dll'), 'CheekyBootstrap_Status') then
    if Other = '' then Other := 'version.dll' else Other := Other + ' and version.dll';
  if Other <> '' then
    if SuppressibleMsgBox('The game folder already has a ' + Other + ' from something else (another mod such as ReShade, OptiScaler ' +
              'or Special K, or an earlier manual install of this mod).' + #13#10#13#10 +
              'Setup will rename it to ' + Other + BackupSuffix + ' so the VR mod can use the name, and put it back ' +
              'when you uninstall. The other mod won''t run while the VR mod is installed.' + #13#10#13#10 + 'Continue?',
              mbConfirmation, MB_YESNO, IDYES) = IDNO then begin
      Result := 'Installation cancelled: nothing was changed.';
      Exit;
    end;
  MoveAside('dxgi.dll', 'vrmod_settings.ini');
  if FoveationOn then MoveAside('version.dll', 'CheekyBootstrap_Status') else RemoveFoveation;
end;

{ ---- Uninstall ---- }

function GetSystemMetrics(nIndex: Integer): Integer;
  external 'GetSystemMetrics@user32.dll stdcall';
function SetProcessDPIAware: Boolean;
  external 'SetProcessDPIAware@user32.dll stdcall';

function InitializeUninstall: Boolean;
begin
  KeepSettings := SuppressibleMsgBox('Keep your VR mod settings (vrmod_settings.ini) for a later reinstall?', mbConfirmation, MB_YESNO, IDYES) = IDYES;
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then RemoveCopiedDlss5;   { before the SOTTR-VR folder is removed }
  if CurUninstallStep = usPostUninstall then begin
    if not KeepSettings then begin
      DeleteFile(ExpandConstant('{app}\vrmod_settings.ini'));
      DeleteFile(ExpandConstant('{app}\CheekyFoveatedDLSS\CheekyFoveatedDLSS.ini'));
      DeleteFile(ExpandConstant('{app}\CheekyFoveatedDLSS\CheekyOverlay.ini'));
      RemoveDir(ExpandConstant('{app}\CheekyFoveatedDLSS'));
    end;
    RestoreAside('dxgi.dll');
    RestoreAside('version.dll');
    { The mod set the game's fullscreen resolution to the headset's size: back to the desktop's, so the game starts
      normally on the monitor. }
    SetProcessDPIAware;   { the real desktop size, not the one Windows scaling reports }
    RegWriteDWordValue(HKCU, GraphicsKey, 'FullscreenWidth', GetSystemMetrics(0));
    RegWriteDWordValue(HKCU, GraphicsKey, 'FullscreenHeight', GetSystemMetrics(1));
  end;
end;
