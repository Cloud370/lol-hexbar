; 海克斯大乱斗战绩条(OBS 插件)NSIS 安装器
;
; 行为:
;   1. 自动检测 OBS Studio 安装目录(OBS 官方安装程序在注册表卸载项里留有
;      DisplayIcon / UninstallString,主流 OBS 插件安装器均以此定位):
;      找到 → 默认装进 OBS 目录经典布局  <OBS>\obs-plugins\64bit\lol-hexbar.dll
;                                              <OBS>\data\obs-plugins\lol-hexbar\...
;      未找到 → 默认装官方 per-plugin 根  <ProgramData>\obs-studio\plugins\lol-hexbar\...
;      (两种位置 OBS 30+ 都会扫描;向导里手动改目录时按目录内容自适应布局)
;   2. 升级 = 重新运行安装器,直接覆盖同名文件
;   3. 写入标准卸载注册表项与自带卸载程序
;
; 构建(需 NSIS 3.x):
;   makensis /DRELEASE_DIR=<安装树目录> /DVERSION=<版本> installer.nsi
;   RELEASE_DIR 指向 cmake --install 产物,即包含 lol-hexbar\bin\64bit\ 与 lol-hexbar\data\ 的目录

!ifndef RELEASE_DIR
  !error "RELEASE_DIR 未定义:应指向包含 lol-hexbar\ 安装树的目录"
!endif
!ifndef VERSION
  !define VERSION "0.0.0"
!endif

Unicode true
ManifestDPIAware true

; TEST_NOADMIN:本地无提权环境的自动化测试开关(user 级 + HKCU + 测试键名),CI/正式构建不定义
!ifdef TEST_NOADMIN
RequestExecutionLevel user
!define REG_HK HKCU
!define REG_UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\lol-hexbar-test"
!define REG_STATEKEY "Software\lol-hexbar-test"
!else
RequestExecutionLevel admin
!define REG_HK HKLM
!define REG_UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\lol-hexbar"
!define REG_STATEKEY "Software\lol-hexbar"
!endif
!define PROG_NAME "海克斯大乱斗战绩条"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

!ifndef OUT_DIR
  !define OUT_DIR "."
!endif

Name "海克斯大乱斗战绩条 (lol-hexbar)"
OutFile "${OUT_DIR}\lol-hexbar-${VERSION}-windows-x64-Installer.exe"
InstallDir "" ; 由 .onInit 自动探测填充
BrandingText "lol-hexbar ${VERSION}"

; ---------------------------------------------------------------- 界面

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_LICENSE "LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------- OBS 目录探测

; 从 DisplayIcon(<OBS>\bin\64bit\obs64.exe)提取 OBS 根目录,写入 $0(失败为空)
Function ObsDirFromIcon
	StrCpy $0 ""
	${If} $1 != ""
		${GetFileName} "$1" $2
		${If} $2 == "obs64.exe"
			${GetParent} "$1" $3
			${GetParent} "$3" $3
			${GetParent} "$3" $3
			${If} ${FileExists} "$3\bin\64bit\obs64.exe"
				StrCpy $0 "$3"
			${EndIf}
		${EndIf}
	${EndIf}
FunctionEnd

; 从 UninstallString("<OBS>\uninstall.exe" [参数])提取 OBS 根目录,写入 $0(失败为空)
Function ObsDirFromUninst
	StrCpy $0 ""
	${If} $1 != ""
		; 取首个引号对内内容;无引号则取到首个空格
		StrCpy $2 ""
		StrCpy $5 0
		cleanLoop:
			IntOp $5 $5 + 1
			StrCpy $4 "$1" 1 $5
			${If} $4 == '"'
				IntOp $5 $5 - 1
				${If} $5 > 0
					StrCpy $2 "$1" $5 1
				${EndIf}
				Goto cleanDone
			${EndIf}
			${If} $4 == " "
				IntOp $5 $5 - 1
				${If} $5 > 0
					StrCpy $2 "$1" $5 1
				${EndIf}
				Goto cleanDone
			${EndIf}
			${If} $4 == ""
				StrCpy $2 "$1"
				Goto cleanDone
			${EndIf}
			Goto cleanLoop
		cleanDone:
		${If} $2 != ""
			${GetFileName} "$2" $3
			${If} $3 == "uninstall.exe"
				${GetParent} "$2" $3
				${If} ${FileExists} "$3\bin\64bit\obs64.exe"
					StrCpy $0 "$3"
				${EndIf}
			${EndIf}
		${EndIf}
	${EndIf}
FunctionEnd

; $1 = 注册表值内容 → 触发两种提取;命中后 $0 = OBS 目录
Function ObsDirFromEntry
	Call ObsDirFromIcon
	${If} $0 == ""
		Call ObsDirFromUninst
	${EndIf}
FunctionEnd

; 读一个"OBS Studio"卸载项并尝试定位;ROOT 为 HKCU/HKLM
!macro TryObsUninstallEntry ROOT KEY
	ReadRegStr $1 ${ROOT} "${KEY}" "DisplayIcon"
	Call ObsDirFromEntry
	${If} $0 == ""
		ReadRegStr $1 ${ROOT} "${KEY}" "UninstallString"
		Call ObsDirFromEntry
	${EndIf}
!macroend

; $0 = 探测到的 OBS 目录,未找到为空
Function DetectOBS
	StrCpy $0 ""

	; 当前用户安装的 OBS(HKCU)
	!insertmacro TryObsUninstallEntry HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio"

	; 全机安装的 OBS(HKLM,Inno 通常写在 WOW6432Node 视图)
	${If} $0 == ""
		!insertmacro TryObsUninstallEntry HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio"
	${EndIf}
	${If} $0 == ""
		SetRegView 32
		!insertmacro TryObsUninstallEntry HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio"
		SetRegView 64
	${EndIf}

	; 常见安装路径兜底
	${If} $0 == ""
		${If} ${FileExists} "$PROGRAMFILES64\obs-studio\bin\64bit\obs64.exe"
			StrCpy $0 "$PROGRAMFILES64\obs-studio"
		${ElseIf} ${FileExists} "$PROGRAMFILES32\obs-studio\bin\64bit\obs64.exe"
			StrCpy $0 "$PROGRAMFILES32\obs-studio"
		${EndIf}
	${EndIf}
FunctionEnd

; ---------------------------------------------------------------- 安装

Section "install"
	; 按最终目录内容决定布局:OBS 根目录(有 obs64.exe 或 obs-plugins\64bit)
	; → 经典布局;其余(含 per-plugin 根)→ lol-hexbar\ 独立目录树
	StrCpy $4 "pluginroot"
	${If} ${FileExists} "$INSTDIR\bin\64bit\obs64.exe"
	${OrIf} ${FileExists} "$INSTDIR\obs-plugins\64bit\*.*"
		StrCpy $4 "obsroot"
	${EndIf}

	${If} $4 == "obsroot"
		SetOutPath "$INSTDIR\obs-plugins\64bit"
		File "${RELEASE_DIR}\lol-hexbar\bin\64bit\lol-hexbar.dll"
		SetOutPath "$INSTDIR\data\obs-plugins\lol-hexbar"
		File /r "${RELEASE_DIR}\lol-hexbar\data\web"
		WriteUninstaller "$INSTDIR\lol-hexbar-uninstall.exe"
	${Else}
		SetOutPath "$INSTDIR\lol-hexbar\bin\64bit"
		File "${RELEASE_DIR}\lol-hexbar\bin\64bit\lol-hexbar.dll"
		SetOutPath "$INSTDIR\lol-hexbar\data"
		File /r "${RELEASE_DIR}\lol-hexbar\data\web"
		WriteUninstaller "$INSTDIR\lol-hexbar\uninstall.exe"
	${EndIf}

	; 记录安装位置/布局供卸载使用;标准卸载项供"应用列表"显示
	WriteRegStr ${REG_HK} "${REG_STATEKEY}" "InstDir" "$INSTDIR"
	WriteRegStr ${REG_HK} "${REG_STATEKEY}" "Layout" "$4"
	WriteRegStr ${REG_HK} "${REG_STATEKEY}" "Version" "${VERSION}"
	${If} $4 == "obsroot"
		WriteRegStr ${REG_HK} "${REG_UNINSTKEY}" "UninstallString" '"$INSTDIR\lol-hexbar-uninstall.exe"'
		WriteRegStr ${REG_HK} "${REG_UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\lol-hexbar-uninstall.exe" /S'
	${Else}
		WriteRegStr ${REG_HK} "${REG_UNINSTKEY}" "UninstallString" '"$INSTDIR\lol-hexbar\uninstall.exe"'
		WriteRegStr ${REG_HK} "${REG_UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\lol-hexbar\uninstall.exe" /S'
	${EndIf}
	WriteRegStr ${REG_HK} "${REG_UNINSTKEY}" "DisplayName" "${PROG_NAME} (lol-hexbar)"
	WriteRegStr ${REG_HK} "${REG_UNINSTKEY}" "DisplayVersion" "${VERSION}"
	WriteRegStr ${REG_HK} "${REG_UNINSTKEY}" "DisplayIcon" "$INSTDIR\lol-hexbar\bin\64bit\lol-hexbar.dll,0"
	WriteRegStr ${REG_HK} "${REG_UNINSTKEY}" "Publisher" "Cloud370"
	WriteRegStr ${REG_HK} "${REG_UNINSTKEY}" "URLInfoAbout" "https://github.com/Cloud370/lol-hexbar"
	WriteRegStr ${REG_HK} "${REG_UNINSTKEY}" "InstallLocation" "$INSTDIR"
	SetOutPath "$INSTDIR"
SectionEnd

; ---------------------------------------------------------------- 卸载

Section "-un.install"
	ReadRegStr $5 ${REG_HK} "${REG_STATEKEY}" "InstDir"
	ReadRegStr $6 ${REG_HK} "${REG_STATEKEY}" "Layout"
	${If} $5 == ""
		Abort
	${EndIf}

	${If} $6 == "obsroot"
		Delete "$5\obs-plugins\64bit\lol-hexbar.dll"
		RMDir /r "$5\data\obs-plugins\lol-hexbar"
		Delete "$5\lol-hexbar-uninstall.exe"
		RMDir "$5\data\obs-plugins" ; 空则删,非空自动跳过
	${Else}
		RMDir /r "$5\lol-hexbar"
	${EndIf}

	DeleteRegKey ${REG_HK} "${REG_UNINSTKEY}"
	DeleteRegKey ${REG_HK} "${REG_STATEKEY}"
	SetAutoClose true
SectionEnd

; ---------------------------------------------------------------- 初始化

Function .onInit
	SetShellVarContext all
	; /D= 显式指定时 $INSTDIR 已非空,不得覆盖;其次用上次安装位置;
	; 再其次自动检测 OBS;最后回落官方 per-plugin 根
	${If} $INSTDIR == ""
		ReadRegStr $0 ${REG_HK} "${REG_STATEKEY}" "InstDir"
		${If} $0 == ""
			Call DetectOBS
		${EndIf}
		${If} $0 != ""
			StrCpy $INSTDIR "$0"
		${Else}
			StrCpy $INSTDIR "$COMMONPROGRAMDATA\obs-studio\plugins"
		${EndIf}
	${EndIf}
FunctionEnd

Function un.onInit
	SetShellVarContext all
FunctionEnd
