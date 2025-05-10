package main

import (
	"syscall"
	"unsafe"
)

// modified from https://gist.githubusercontent.com/nathan-osman/18c2e227ad00a223b61c0b3c16d452c3/raw/d2da5c08ed82aac945c0053ce60d12e8c2a9d265/win32.go

// Windows Message Constants
const (
	WM_NULL            = 0x0000
	WM_CREATE          = 0x0001
	WM_DESTROY         = 0x0002
	WM_MOVE            = 0x0003
	WM_SIZE            = 0x0005
	WM_ACTIVATE        = 0x0006
	WM_SETFOCUS        = 0x0007
	WM_KILLFOCUS       = 0x0008
	WM_ENABLE          = 0x000A
	WM_SETREDRAW       = 0x000B
	WM_SETTEXT         = 0x000C
	WM_GETTEXT         = 0x000D
	WM_GETTEXTLENGTH   = 0x000E
	WM_PAINT           = 0x000F
	WM_CLOSE           = 0x0010
	WM_SETFONT         = 0x0030
	WM_COMMAND         = 0x0111
	WM_SYSCOMMAND      = 0x0112
	WM_TIMER           = 0x0113
	WM_HSCROLL         = 0x0114
	WM_VSCROLL         = 0x0115
	WM_INITMENU        = 0x0116
	WM_INITMENUPOPUP   = 0x0117
	WM_CTLCOLORMSGBOX  = 0x0132
	WM_CTLCOLOREDIT    = 0x0133
	WM_CTLCOLORLISTBOX = 0x0134
	WM_CTLCOLORBTN     = 0x0135
	WM_CTLCOLORDLG     = 0x0136
	WM_CTLCOLORSCROLLBAR = 0x0137
	WM_CTLCOLORSTATIC  = 0x0138
	WM_USER            = 0x0400
	WM_APP             = 0x8000

	// Edit Control Messages
	EM_GETLINECOUNT  = 0x00BA
	EM_LINESCROLL    = 0x00B6
	EM_SETSEL        = 0x00B1
	EM_SCROLLCARET   = 0x00B7
	// EM_GETSEL (not used here, but for completeness if needed later: 0x00B0)
	// EM_REPLACESEL (not used here: 0x00C2)
	// EM_LINEINDEX (not used here: 0x00BB)
)

// Window Styles
const (
	WS_OVERLAPPED       = 0x00000000
	WS_POPUP            = 0x80000000
	WS_CHILD            = 0x40000000
	WS_MINIMIZE         = 0x20000000
	WS_VISIBLE          = 0x10000000
	WS_DISABLED         = 0x08000000
	WS_CLIPSIBLINGS     = 0x04000000
	WS_CLIPCHILDREN     = 0x02000000
	WS_MAXIMIZE         = 0x01000000
	WS_CAPTION          = 0x00C00000 // WS_BORDER | WS_DLGFRAME
	WS_BORDER           = 0x00800000
	WS_DLGFRAME         = 0x00400000
	WS_VSCROLL          = 0x00200000
	WS_HSCROLL          = 0x00100000
	WS_SYSMENU          = 0x00080000
	WS_THICKFRAME       = 0x00040000
	WS_GROUP            = 0x00020000
	WS_TABSTOP          = 0x00010000
	WS_MINIMIZEBOX      = 0x00020000 // Note: This is same as WS_GROUP, ensure intended use. Often WS_GROUP is for dialogs.
	WS_MAXIMIZEBOX      = 0x00010000 // Note: This is same as WS_TABSTOP
	WS_OVERLAPPEDWINDOW = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX
)

// Extended Window Styles
const (
	WS_EX_CLIENTEDGE = 0x00000200
	WS_EX_APPWINDOW  = 0x00040000
)

// Edit Control Styles
const (
	ES_MULTILINE    = 0x0004
	ES_AUTOVSCROLL  = 0x0040
	ES_AUTOHSCROLL  = 0x0080
	ES_READONLY     = 0x0800
	ES_WANTRETURN   = 0x1000
	ES_NUMBER       = 0x2000
	// ES_LEFT (already default) = 0x0000
	// ES_CENTER = 0x0001
	// ES_RIGHT = 0x0002
	// ES_PASSWORD = 0x0020
)

// Button Control Styles
const (
	BS_PUSHBUTTON    = 0x00000000
	BS_DEFPUSHBUTTON = 0x00000001
	// BS_CHECKBOX = 0x00000002
	// BS_AUTOCHECKBOX = 0x00000003
	// BS_RADIOBUTTON = 0x00000004
	// BS_GROUPBOX = 0x00000007
)

// Static Control Styles
const (
	SS_LEFT = 0x00000000
	// SS_CENTER = 0x00000001
	// SS_RIGHT = 0x00000002
	// SS_NOPREFIX = 0x00000080 // Prevents interpretation of "&"
)

// ShowWindow Commands
const (
	SW_HIDE            = 0
	SW_SHOWNORMAL      = 1
	SW_SHOWMINIMIZED   = 2
	SW_SHOWMAXIMIZED   = 3
	SW_MAXIMIZE        = 3 // Alias for SW_SHOWMAXIMIZED
	SW_SHOWNOACTIVATE  = 4
	SW_SHOW            = 5
	SW_MINIMIZE        = 6
	SW_SHOWMINNOACTIVE = 7
	SW_SHOWNA          = 8 // Alias for SW_SHOWNOACTIVATE
	SW_RESTORE         = 9
	SW_SHOWDEFAULT     = 10
	SW_FORCEMINIMIZE   = 11
)

// Common Controls - For LoadCursor
const (
	IDC_ARROW = 32512
	// IDC_IBEAM = 32513
	// IDC_WAIT = 32514
	// IDC_CROSS = 32515
	// IDC_UPARROW = 32516
	// IDC_SIZENWSE = 32642
	// IDC_SIZENESW = 32643
	// IDC_SIZEWE = 32644
	// IDC_SIZENS = 32645
	// IDC_SIZEALL = 32646
	// IDC_NO = 32648
	// IDC_HAND = 32649
	// IDC_APPSTARTING = 32650
	// IDC_HELP = 32651
)

// For GetSystemMetrics
const (
	SM_CXSCREEN = 0
	SM_CYSCREEN = 1
)

// Scroll Bar Commands (for WM_VSCROLL, WM_HSCROLL messages)
const (
	SB_LINEUP        = 0
	SB_LINEDOWN      = 1
	SB_PAGEUP        = 2
	SB_PAGEDOWN      = 3
	SB_THUMBPOSITION = 4
	SB_THUMBTRACK    = 5
	SB_TOP           = 6
	SB_BOTTOM        = 7
	SB_ENDSCROLL     = 8
)


var (
	kernel32         = syscall.NewLazyDLL("kernel32.dll")
	user32           = syscall.NewLazyDLL("user32.dll")
	gdi32            = syscall.NewLazyDLL("gdi32.dll")

	// Kernel32
	pGetModuleHandleW = kernel32.NewProc("GetModuleHandleW")

	// User32
	pRegisterClassExW    = user32.NewProc("RegisterClassExW")
	pCreateWindowExW     = user32.NewProc("CreateWindowExW")
	pDestroyWindow       = user32.NewProc("DestroyWindow")
	pShowWindow          = user32.NewProc("ShowWindow")
	pUpdateWindow        = user32.NewProc("UpdateWindow")
	pGetMessageW         = user32.NewProc("GetMessageW")
	pTranslateMessage    = user32.NewProc("TranslateMessage")
	pDispatchMessageW    = user32.NewProc("DispatchMessageW")
	pPostQuitMessage     = user32.NewProc("PostQuitMessage")
	pDefWindowProcW      = user32.NewProc("DefWindowProcW")
	pLoadCursorW         = user32.NewProc("LoadCursorW")
	pSendMessageW        = user32.NewProc("SendMessageW")
	pPostMessageW        = user32.NewProc("PostMessageW")
	pGetDlgItem          = user32.NewProc("GetDlgItem")
	pSetWindowTextW      = user32.NewProc("SetWindowTextW")
	pGetWindowTextW      = user32.NewProc("GetWindowTextW")
	pGetWindowTextLengthW= user32.NewProc("GetWindowTextLengthW")
	pGetClientRect       = user32.NewProc("GetClientRect")
	pMoveWindow          = user32.NewProc("MoveWindow")
	pSetFocus            = user32.NewProc("SetFocus")
    pGetSystemMetrics    = user32.NewProc("GetSystemMetrics")
	// pIsDialogMessageW    = user32.NewProc("IsDialogMessageW") // If needed for advanced TAB key handling

	// Gdi32
	pCreateFontIndirectW = gdi32.NewProc("CreateFontIndirectW")
	pDeleteObject        = gdi32.NewProc("DeleteObject")
)

// Structures
type POINT struct {
	X, Y int32
}

type MSG struct {
	Hwnd    syscall.Handle
	Message uint32
	WParam  uintptr
	LParam  uintptr
	Time    uint32
	Pt      POINT
}

type WNDCLASSEXW struct {
	Size       uint32
	Style      uint32
	WndProc    uintptr
	ClsExtra   int32
	WndExtra   int32
	Instance   syscall.Handle
	Icon       syscall.Handle
	Cursor     syscall.Handle
	Background syscall.Handle
	MenuName   *uint16
	ClassName  *uint16
	IconSm     syscall.Handle
}

type RECT struct {
	Left, Top, Right, Bottom int32
}

type LOGFONTW struct {
	Height         int32
	Width          int32
	Escapement     int32
	Orientation    int32
	Weight         int32
	Italic         byte
	Underline      byte
	StrikeOut      byte
	CharSet        byte
	OutPrecision   byte
	ClipPrecision  byte
	Quality        byte
	PitchAndFamily byte
	FaceName       [32]uint16 // LF_FACESIZE
}


// Functions
func GetModuleHandle() (syscall.Handle, error) {
	ret, _, err := pGetModuleHandleW.Call(0)
	if ret == 0 {
		return 0, err
	}
	return syscall.Handle(ret), nil
}

func RegisterClassEx(wcex *WNDCLASSEXW) (uint16, error) {
	ret, _, err := pRegisterClassExW.Call(uintptr(unsafe.Pointer(wcex)))
	if ret == 0 {
		return 0, err
	}
	return uint16(ret), nil
}

func CreateWindowEx(exStyle uint32, className, windowName *uint16, style uint32, x, y, width, height int32, parent, menu, instance syscall.Handle, param unsafe.Pointer) (syscall.Handle, error) {
	ret, _, err := pCreateWindowExW.Call(
		uintptr(exStyle),
		uintptr(unsafe.Pointer(className)),
		uintptr(unsafe.Pointer(windowName)),
		uintptr(style),
		uintptr(x),
		uintptr(y),
		uintptr(width),
		uintptr(height),
		uintptr(parent),
		uintptr(menu),
		uintptr(instance),
		uintptr(param),
	)
	if ret == 0 {
		return 0, err
	}
	return syscall.Handle(ret), nil
}

func DestroyWindow(hwnd syscall.Handle) bool {
	ret, _, _ := pDestroyWindow.Call(uintptr(hwnd))
	return ret != 0
}

func ShowWindow(hwnd syscall.Handle, cmdShow int32) bool {
	ret, _, _ := pShowWindow.Call(uintptr(hwnd), uintptr(cmdShow))
	return ret != 0
}

func UpdateWindow(hwnd syscall.Handle) bool {
	ret, _, _ := pUpdateWindow.Call(uintptr(hwnd))
	return ret != 0
}

func GetMessage(msg *MSG, hwnd syscall.Handle, msgFilterMin, msgFilterMax uint32) (int32, error) {
	ret, _, err := pGetMessageW.Call(
		uintptr(unsafe.Pointer(msg)),
		uintptr(hwnd),
		uintptr(msgFilterMin),
		uintptr(msgFilterMax),
	)
	if int32(ret) == -1 {
		return int32(ret), err
	}
	return int32(ret), nil
}


func TranslateMessage(msg *MSG) bool {
	ret, _, _ := pTranslateMessage.Call(uintptr(unsafe.Pointer(msg)))
	return ret != 0
}

func DispatchMessage(msg *MSG) uintptr {
	ret, _, _ := pDispatchMessageW.Call(uintptr(unsafe.Pointer(msg)))
	return ret
}

func PostQuitMessage(exitCode int32) {
	pPostQuitMessage.Call(uintptr(exitCode))
}

func DefWindowProc(hwnd syscall.Handle, msg uint32, wParam, lParam uintptr) uintptr {
	ret, _, _ := pDefWindowProcW.Call(uintptr(hwnd), uintptr(msg), wParam, lParam)
	return ret
}

func LoadCursor(instance syscall.Handle, cursorName uintptr) (syscall.Handle, error) {
	ret, _, err := pLoadCursorW.Call(uintptr(instance), cursorName)
	if ret == 0 {
		return 0, err
	}
	return syscall.Handle(ret), nil
}

func SendMessage(hwnd syscall.Handle, msg uint32, wParam, lParam uintptr) uintptr {
	ret, _, _ := pSendMessageW.Call(uintptr(hwnd), uintptr(msg), wParam, lParam)
	return ret
}

func PostMessage(hwnd syscall.Handle, msg uint32, wParam, lParam uintptr) bool {
	ret, _, _ := pPostMessageW.Call(uintptr(hwnd), uintptr(msg), wParam, lParam)
	return ret != 0
}

func GetDlgItem(hwnd syscall.Handle, id int32) (syscall.Handle, error) {
	ret, _, err := pGetDlgItem.Call(uintptr(hwnd), uintptr(id))
	if ret == 0 {
		return 0, err
	}
	return syscall.Handle(ret), nil
}

func SetWindowText(hwnd syscall.Handle, text string) bool {
	ret, _, _ := pSetWindowTextW.Call(uintptr(hwnd), uintptr(unsafe.Pointer(syscall.StringToUTF16Ptr(text))))
	return ret != 0
}

func GetWindowTextLength(hwnd syscall.Handle) int32 {
    ret, _, _ := pGetWindowTextLengthW.Call(uintptr(hwnd))
    return int32(ret)
}

func GetWindowText(hwnd syscall.Handle, buffer *uint16, maxChars int32) (int32, error) {
    ret, _, err := pGetWindowTextW.Call(
        uintptr(hwnd),
        uintptr(unsafe.Pointer(buffer)),
        uintptr(maxChars),
    )
    if ret == 0 {
		// Check if GetLastError indicates an error, otherwise it might be an empty string.
		// syscall.GetLastError() is not directly available here this way.
		// The err return from .Call() should be used.
		// If err is not nil and it's a syscall.Errno, check if it's non-zero.
		if errno, ok := err.(syscall.Errno); ok && errno != 0 {
			return 0, err
		}
		// If ret is 0 and no error, it means empty text.
	}
    return int32(ret), nil
}


func GetClientRect(hwnd syscall.Handle, rect *RECT) bool {
	ret, _, _ := pGetClientRect.Call(uintptr(hwnd), uintptr(unsafe.Pointer(rect)))
	return ret != 0
}

func MoveWindow(hwnd syscall.Handle, x, y, width, height int32, repaint bool) bool {
	var bRepaint uintptr
	if repaint {
		bRepaint = 1
	}
	ret, _, _ := pMoveWindow.Call(
		uintptr(hwnd),
		uintptr(x),
		uintptr(y),
		uintptr(width),
		uintptr(height),
		bRepaint,
	)
	return ret != 0
}

func SetFocus(hwnd syscall.Handle) (syscall.Handle, error) {
	ret, _, err := pSetFocus.Call(uintptr(hwnd))
	// SetFocus returns the handle of the window that previously had focus if successful.
	// It returns NULL if there is an error (e.g. hwnd is invalid).
	// The error from .Call() (syscall.GetLastError()) is more reliable for failure.
	if ret == 0 {
		if errno, ok := err.(syscall.Errno); ok && errno != 0 {
			return 0, err
		}
		// If ret is 0 and no OS error, it might mean no window previously had focus,
		// or the call itself failed in a way not captured by GetLastError (less common for SetFocus).
		// For simplicity, we can treat ret == 0 as a potential issue if an error is also set.
	}
	return syscall.Handle(ret), nil // Return previous focus handle or 0
}

func CreateFontIndirect(logFont *LOGFONTW) (syscall.Handle, error) {
    ret, _, err := pCreateFontIndirectW.Call(uintptr(unsafe.Pointer(logFont)))
    if ret == 0 {
        return 0, err
    }
    return syscall.Handle(ret), nil
}

func DeleteObject(hObject syscall.Handle) bool {
    ret, _, _ := pDeleteObject.Call(uintptr(hObject))
    return ret != 0
}

func GetSystemMetrics(nIndex int32) int32 {
    ret, _, _ := pGetSystemMetrics.Call(uintptr(nIndex))
    return int32(ret)
}

// Helper to convert string to *[32]uint16 for LOGFONT
func strToLfFaceName(s string) [32]uint16 {
    var arr [32]uint16
    src := syscall.StringToUTF16(s)
    // LF_FACESIZE is 32. We need to copy up to 31 characters and ensure null termination.
    limit := len(src)
    if limit > 31 {
        limit = 31
    }
    for i := 0; i < limit; i++ {
        arr[i] = src[i]
    }
    arr[limit] = 0 // Null terminate
    return arr
}
