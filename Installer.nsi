; Legacy developer convenience only.
; This script is not invoked by the HEngine 2.0 source-first release gate.
; Installer.nsi and any HEngineInstaller.exe it produces are not release
; artifacts and must not be used as build, test, package, or install evidence.

;--------------------------------
; Includes

  !include "MUI2.nsh"
  !include "logiclib.nsh"

;--------------------------------
; Custom defines
  !define NAME "HEngine"
  !define EXENAME HEngineEditor
  !define APPFILE "HEngineEditor.exe"
  !define VERSION "1.1.0"
  !define SLUG "${NAME} v${VERSION}"

;--------------------------------
; General

  Name "${NAME}"
  OutFile "${NAME}Installer.exe"
  InstallDir "$PROGRAMFILES\${NAME}"
  InstallDirRegKey HKCU "Software\${NAME}" ""
  RequestExecutionLevel admin

;--------------------------------
; UI
  
  !define MUI_ICON "HEngine\Engine\Resources\Icons\EngineIcon.ico"
  !define MUI_HEADERIMAGE
  !define MUI_WELCOMEFINISHPAGE_BITMAP "HEngine\Engine\Resources\Icons\EngineIcon.ico"
  !define MUI_HEADERIMAGE_BITMAP "HEngine\Engine\Resources\Icons\EngineIcon.ico"
  !define MUI_ABORTWARNING
  !define MUI_WELCOMEPAGE_TITLE "${SLUG} Setup"

;--------------------------------
; Pages
  
  ; Installer pages
  !insertmacro MUI_PAGE_WELCOME
  !insertmacro MUI_PAGE_LICENSE "LICENSE"
  !insertmacro MUI_PAGE_COMPONENTS
  !insertmacro MUI_PAGE_DIRECTORY
  !insertmacro MUI_PAGE_INSTFILES
  !insertmacro MUI_PAGE_FINISH

  ; Uninstaller pages
  !insertmacro MUI_UNPAGE_CONFIRM
  !insertmacro MUI_UNPAGE_INSTFILES
  
  ; Set UI language
  !insertmacro MUI_LANGUAGE "English"

;--------------------------------
; Section - Install App

  Section "-hidden Engine"
    SectionIn RO
    SetOutPath "$INSTDIR"
    File /r "HEngine\*.*" 
    ; WriteRegStr HKCU "Software\${NAME}" "" $INSTDIR
    WriteUninstaller "$INSTDIR\Uninstall.exe"
  SectionEnd

;--------------------------------
; Section - Shortcut

  Section "Desktop Shortcut" DeskShort
    CreateShortCut "$DESKTOP\${EXENAME}.lnk" "$INSTDIR\Engine\Binaries\Win64\${APPFILE}" "" "$INSTDIR\Engine\Resources\Icons\EngineIcon.ico" 0
  SectionEnd
