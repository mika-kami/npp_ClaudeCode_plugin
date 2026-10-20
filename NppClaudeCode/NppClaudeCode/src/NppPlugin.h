// Minimal subset of the Notepad++ plugin SDK (PluginInterface.h,
// Notepad_plus_msgs.h, menuCmdID.h, Scintilla.h) - just what this plugin uses.
// Values match the official headers; the binary interface is stable.
#pragma once
#include <windows.h>

struct NppData {
    HWND _nppHandle;
    HWND _scintillaMainHandle;
    HWND _scintillaSecondHandle;
};

typedef void(__cdecl* PFUNCPLUGINCMD)();

struct ShortcutKey {
    bool _isCtrl;
    bool _isAlt;
    bool _isShift;
    UCHAR _key;
};

const int menuItemSize = 64;

struct FuncItem {
    wchar_t _itemName[menuItemSize];
    PFUNCPLUGINCMD _pFunc;   // nullptr = separator
    int _cmdID;
    bool _init2Check;
    ShortcutKey* _pShKey;
};

// Only the header of SCNotification is used (same layout as NMHDR).
struct SCNotification {
    NMHDR nmhdr;
};

// ---- Notepad++ messages ----
constexpr UINT NPPMSG = WM_USER + 1000;
constexpr UINT NPPM_GETCURRENTSCINTILLA  = NPPMSG + 4;
constexpr UINT NPPM_SETSTATUSBAR         = NPPMSG + 24;
constexpr UINT NPPM_RELOADFILE           = NPPMSG + 36;
constexpr UINT NPPM_SAVECURRENTFILE      = NPPMSG + 38;
constexpr UINT NPPM_GETPLUGINSCONFIGDIR  = NPPMSG + 46;
constexpr UINT NPPM_MENUCOMMAND          = NPPMSG + 48;
constexpr UINT NPPM_GETCURRENTBUFFERID   = NPPMSG + 60;
constexpr UINT NPPM_DOOPEN               = NPPMSG + 77;

constexpr UINT RUNCOMMAND_USER = WM_USER + 3000;
constexpr UINT NPPM_GETFULLCURRENTPATH = RUNCOMMAND_USER + 1;

constexpr WPARAM STATUSBAR_DOC_TYPE = 0;

constexpr UINT NPPN_FIRST    = 1000;
constexpr UINT NPPN_READY    = NPPN_FIRST + 1;
constexpr UINT NPPN_SHUTDOWN = NPPN_FIRST + 9;

constexpr int IDM_FILE_NEW = 41001;

// ---- Scintilla messages ----
constexpr UINT SCI_GETLENGTH            = 2006;
constexpr UINT SCI_GETEOLMODE           = 2030;
constexpr UINT SCI_BEGINUNDOACTION      = 2078;
constexpr UINT SCI_ENDUNDOACTION        = 2079;
constexpr UINT SCI_GETCODEPAGE          = 2137;
constexpr UINT SCI_GETREADONLY          = 2140;
constexpr UINT SCI_GETSELECTIONSTART    = 2143;
constexpr UINT SCI_GETSELECTIONEND      = 2145;
constexpr UINT SCI_GETMODIFY            = 2159;
constexpr UINT SCI_SETSEL               = 2160;
constexpr UINT SCI_LINEFROMPOSITION     = 2166;
constexpr UINT SCI_SETTEXT              = 2181;
constexpr UINT SCI_SETTARGETSTART       = 2190;
constexpr UINT SCI_SETTARGETEND         = 2192;
constexpr UINT SCI_REPLACETARGET        = 2194;
constexpr UINT SCI_SELECTIONISRECTANGLE = 2372;
constexpr UINT SCI_GETCHARACTERPOINTER  = 2520;
constexpr UINT SCI_GETSELECTIONS        = 2570;

constexpr int SC_EOL_CRLF = 0;
constexpr int SC_EOL_CR   = 1;
constexpr int SC_EOL_LF   = 2;
