#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h> // For wcscat_s, wcscpy_s, etc. _wcsdup
#include <tchar.h> // For _TCHAR, _tcscpy, etc. (though direct W functions are used)
#include <commctrl.h> // For some common controls if ever needed, not strictly for this set.

// --- Configuration ---
#define MAX_LOG_LINES_IN_EDIT_CONTROL 200
#define DEFAULT_CMD_PREFIX L"yt-dlp --js-runtimes quickjs --cookies cookies.txt -f 140 -N 12"
#define WINDOW_CLASS_NAME L"CmdQueueGUIWindowClass"
#define MAX_QUEUE_SIZE 100
#define PIPE_BUFFER_SIZE 4096

// --- Control IDs ---
#define IDC_STATIC_PREFIX_LABEL    100
#define IDC_EDIT_PREFIX            101
#define IDC_STATIC_DASHBOARD_LABEL 102
#define IDC_STATIC_DASHBOARD       103
#define IDC_STATIC_LOG_LABEL       104
#define IDC_EDIT_LOG               105
#define IDC_STATIC_INPUT_LABEL     106
#define IDC_EDIT_INPUT             107 // Suffix input
#define IDC_BUTTON_ADD             108

// --- Custom Window Messages ---
#define WM_APP_APPEND_LOG_CHUNK (WM_APP + 1) // lParam is LogChunk*, wParam is 0 for stdout, 1 for stderr
#define WM_APP_UPDATE_DASHBOARD (WM_APP + 2)
#define WM_APP_COMMAND_DONE     (WM_APP + 3) // Signals command processor finished a task

// --- Structures ---
typedef struct {
    wchar_t* prefix;
    wchar_t* suffix;
} QueuedTask;

typedef struct {
    wchar_t* text;      // Dynamically allocated wide string
    BOOL is_progress_line; // True if this line should replace the previous one in display
} LogChunk;

// --- Global Variables ---
// Handles
HINSTANCE g_hInstance = NULL;
HWND g_hwndMain = NULL;
HWND g_hwndPrefixLabel, g_hwndPrefixEdit;
HWND g_hwndDashboardLabel, g_hwndDashboard;
HWND g_hwndLogLabel, g_hwndLog;
HWND g_hwndInputLabel, g_hwndInputEdit; // Suffix input
HWND g_hwndButtonAdd;
HFONT g_hFont = NULL;

// Command Queue
QueuedTask g_taskQueue[MAX_QUEUE_SIZE];
int g_queueHead = 0;
int g_queueTail = 0;
int g_queueCount = 0;
CRITICAL_SECTION g_queueLock;
CONDITION_VARIABLE g_queueNotEmpty;
HANDLE g_hCommandProcessorThread = NULL;
BOOL g_appExiting = FALSE;

// Dashboard State
wchar_t g_currentCommand[512] = L"Idle";
CRITICAL_SECTION g_dashboardLock;

// Initial command prefix (can be set by command line argument in a more complex setup)
const wchar_t* g_initialCmdPrefix = DEFAULT_CMD_PREFIX;


// --- Forward Declarations ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
DWORD WINAPI CommandProcessorThread(LPVOID lpParam);
void AddToQueue(const wchar_t* prefix, const wchar_t* suffix);
void UpdateDashboardUI(void);
void PostLogChunkToUI(const char* utf8_chunk, BOOL is_stderr_color_hint, BOOL is_progress);
void PostLogChunkToUI_Wide(const wchar_t* wide_chunk, BOOL is_stderr_color_hint, BOOL is_progress);
wchar_t* Utf8ToWide(const char* utf8String);
// char* WideToUtf8(const wchar_t* wideString); // For command arguments if needed (not currently used)
void InitializeUIFont(void);
void CreateControls(HWND hwndParent);
void TrimTrailingCr(wchar_t* str);


// --- Entry Point ---
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    g_hInstance = hInstance;
    
    InitializeCriticalSection(&g_queueLock);
    InitializeCriticalSection(&g_dashboardLock);
    InitializeConditionVariable(&g_queueNotEmpty);

    WNDCLASSEXW wcex = {0};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wcex.lpszMenuName = NULL;
    wcex.lpszClassName = WINDOW_CLASS_NAME;
    wcex.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassExW(&wcex)) {
        MessageBoxW(NULL, L"Window Registration Failed!", L"Error", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

    InitializeUIFont();
    PostLogChunkToUI("Application starting...", FALSE, FALSE);
    wchar_t initialMsg[256];
    swprintf(initialMsg, 256, L"Initial command prefix set to: %s (editable in GUI)", g_initialCmdPrefix);
    PostLogChunkToUI_Wide(initialMsg, FALSE, TRUE); 
    PostLogChunkToUI("Enter command suffix and click 'Add to Queue' or press Enter.", FALSE, FALSE);
    PostLogChunkToUI("Close window or press Alt+F4 to quit.", FALSE, FALSE);


    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int windowWidth = 600;
    int windowHeight = 500; // Increased height a bit for more dashboard visibility
    int windowX = (screenWidth - windowWidth) / 2;
    int windowY = (screenHeight - windowHeight) / 2;

    g_hwndMain = CreateWindowExW(
        0, 
        WINDOW_CLASS_NAME,
        L"Cmd Queue GUI (C Version)",
        WS_OVERLAPPEDWINDOW, 
        windowX, windowY, windowWidth, windowHeight,
        NULL, NULL, hInstance, NULL
    );

    if (!g_hwndMain) {
        MessageBoxW(NULL, L"Window Creation Failed!", L"Error", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

    ShowWindow(g_hwndMain, nCmdShow);
    UpdateWindow(g_hwndMain);

    g_hCommandProcessorThread = CreateThread(NULL, 0, CommandProcessorThread, NULL, 0, NULL);
    if (g_hCommandProcessorThread == NULL) {
        MessageBoxW(NULL, L"Failed to create command processor thread!", L"Error", MB_ICONEXCLAMATION | MB_OK);
        return 1;
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            HWND hFocused = GetFocus();
            if (hFocused == g_hwndInputEdit) {
                SendMessage(g_hwndMain, WM_COMMAND, MAKEWPARAM(IDC_BUTTON_ADD, BN_CLICKED), (LPARAM)g_hwndButtonAdd);
                continue; 
            }
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_hCommandProcessorThread) {
        g_appExiting = TRUE;
        EnterCriticalSection(&g_queueLock);
        WakeConditionVariable(&g_queueNotEmpty); 
        LeaveCriticalSection(&g_queueLock);
        WaitForSingleObject(g_hCommandProcessorThread, INFINITE);
        CloseHandle(g_hCommandProcessorThread);
    }

    DeleteCriticalSection(&g_queueLock);
    DeleteCriticalSection(&g_dashboardLock);
    
    if (g_hFont) DeleteObject(g_hFont);
    
    return (int)msg.wParam;
}


// --- Window Procedure ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            CreateControls(hwnd);
            UpdateDashboardUI(); 
            break;

        case WM_COMMAND: {
            WORD controlId = LOWORD(wParam);
            WORD notifyCode = HIWORD(wParam); 

            if (controlId == IDC_BUTTON_ADD && notifyCode == BN_CLICKED) {
                wchar_t prefix_buffer[512];
                wchar_t suffix_buffer[1024];

                GetWindowTextW(g_hwndPrefixEdit, prefix_buffer, sizeof(prefix_buffer)/sizeof(wchar_t));
                GetWindowTextW(g_hwndInputEdit, suffix_buffer, sizeof(suffix_buffer)/sizeof(wchar_t));
                
                wchar_t *p = prefix_buffer + wcslen(prefix_buffer);
                while (p > prefix_buffer && iswspace(*(p-1))) *--p = 0;
                wchar_t *s = prefix_buffer;
                while (*s && iswspace(*s)) s++;
                memmove(prefix_buffer, s, (wcslen(s)+1)*sizeof(wchar_t));

                p = suffix_buffer + wcslen(suffix_buffer);
                while (p > suffix_buffer && iswspace(*(p-1))) *--p = 0;
                s = suffix_buffer;
                while (*s && iswspace(*s)) s++;
                memmove(suffix_buffer, s, (wcslen(s)+1)*sizeof(wchar_t));

                if (wcslen(prefix_buffer) == 0) {
                    PostLogChunkToUI("Error: Command prefix cannot be empty.", TRUE, FALSE);
                    SetFocus(g_hwndPrefixEdit);
                } else {
                    AddToQueue(prefix_buffer, suffix_buffer);
                    wchar_t logMsg[1600];
                    swprintf(logMsg, sizeof(logMsg)/sizeof(wchar_t), L"Added to queue: [%s] %s", prefix_buffer, suffix_buffer);
                    PostLogChunkToUI_Wide(logMsg, FALSE, TRUE); 
                    SetWindowTextW(g_hwndInputEdit, L"");
                    SetFocus(g_hwndInputEdit);
                    PostMessage(hwnd, WM_APP_UPDATE_DASHBOARD, 0, 0);
                }
            }
            break;
        }
        
        case WM_APP_APPEND_LOG_CHUNK: {
            LogChunk* chunk = (LogChunk*)lParam;
            if (!chunk || !chunk->text) break;

            int currentLen = GetWindowTextLengthW(g_hwndLog);
            SendMessageW(g_hwndLog, EM_SETSEL, currentLen, currentLen); 

            if (chunk->is_progress_line) {
                LRESULT lineCount = SendMessageW(g_hwndLog, EM_GETLINECOUNT, 0, 0);
                if (lineCount > 0) {
                    LRESULT lastLineStartCharIndex = SendMessageW(g_hwndLog, EM_LINEINDEX, lineCount - 1, 0);
                    if (lastLineStartCharIndex != (LRESULT)-1) {
                         SendMessageW(g_hwndLog, EM_SETSEL, lastLineStartCharIndex, -1); 
                    }
                }
            }
            
            SendMessageW(g_hwndLog, EM_REPLACESEL, TRUE, (LPARAM)chunk->text);

            LRESULT lines = SendMessageW(g_hwndLog, EM_GETLINECOUNT, 0, 0);
            while (lines > MAX_LOG_LINES_IN_EDIT_CONTROL) {
                LRESULT firstLineEnd = SendMessageW(g_hwndLog, EM_LINEINDEX, 1, 0); 
                if (firstLineEnd == -1) { 
                    SendMessageW(g_hwndLog, EM_SETSEL, 0, GetWindowTextLengthW(g_hwndLog)/2); 
                } else {
                    SendMessageW(g_hwndLog, EM_SETSEL, 0, firstLineEnd); 
                }
                SendMessageW(g_hwndLog, EM_REPLACESEL, TRUE, (LPARAM)L""); 
                lines = SendMessageW(g_hwndLog, EM_GETLINECOUNT, 0, 0);
            }

            currentLen = GetWindowTextLengthW(g_hwndLog);
            SendMessageW(g_hwndLog, EM_SETSEL, currentLen, currentLen);
            SendMessageW(g_hwndLog, EM_SCROLLCARET, 0, 0);
            
            free(chunk->text);
            free(chunk);
            break;
        }

        case WM_APP_UPDATE_DASHBOARD:
            UpdateDashboardUI();
            break;

        case WM_APP_COMMAND_DONE:
            EnterCriticalSection(&g_dashboardLock);
            wcscpy_s(g_currentCommand, sizeof(g_currentCommand)/sizeof(wchar_t), L"Idle");
            LeaveCriticalSection(&g_dashboardLock);
            UpdateDashboardUI();
            break;
            
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_ACTIVE || LOWORD(wParam) == WA_CLICKACTIVE) {
                if (g_hwndInputEdit) SetFocus(g_hwndInputEdit);
            }
            break;

        case WM_SIZE:
            // TODO: Implement control resizing here for better responsiveness
            // Example:
            // RECT rcClient;
            // GetClientRect(hwnd, &rcClient);
            // int newWidth = rcClient.right - rcClient.left - 2 * margin;
            // MoveWindow(g_hwndPrefixEdit, margin, y_pos_prefix_edit, newWidth, controlHeight, TRUE);
            // ... and so on for other controls ...
            // RedrawWindow(hwnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            break;

        case WM_DESTROY:
            g_appExiting = TRUE; 
            EnterCriticalSection(&g_queueLock);
            WakeConditionVariable(&g_queueNotEmpty); 
            LeaveCriticalSection(&g_queueLock);
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// --- UI Helpers ---
void InitializeUIFont(void) {
    LOGFONTW lf = {0};
    lf.lfHeight = -MulDiv(10, GetDeviceCaps(GetDC(NULL), LOGPIXELSY), 72); 
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Segoe UI");
    g_hFont = CreateFontIndirectW(&lf);
}

void CreateControls(HWND hwndParent) {
    int currentY = 10;
    int margin = 10;
    int controlHeight = 25;
    int labelHeight = 20;
    int gap = 5;
    
    RECT clientRect;
    GetClientRect(hwndParent, &clientRect);
    int windowWidth = clientRect.right - clientRect.left;
    int editWidth = windowWidth - 2 * margin;
    int labelWidth = editWidth;


    g_hwndPrefixLabel = CreateWindowExW(0, L"STATIC", L"Command Prefix:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        margin, currentY, labelWidth, labelHeight, hwndParent, (HMENU)IDC_STATIC_PREFIX_LABEL, g_hInstance, NULL);
    SendMessageW(g_hwndPrefixLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    currentY += labelHeight + gap;

    g_hwndPrefixEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_initialCmdPrefix,
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
        margin, currentY, editWidth, controlHeight, hwndParent, (HMENU)IDC_EDIT_PREFIX, g_hInstance, NULL);
    SendMessageW(g_hwndPrefixEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    currentY += controlHeight + gap * 2;

    g_hwndDashboardLabel = CreateWindowExW(0, L"STATIC", L"Dashboard:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        margin, currentY, labelWidth, labelHeight, hwndParent, (HMENU)IDC_STATIC_DASHBOARD_LABEL, g_hInstance, NULL);
    SendMessageW(g_hwndDashboardLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    currentY += labelHeight + gap;

    // Dashboard Display (Read-only Edit Control with Scrollbar)
    int dashboardHeight = 80; // Increased height for dashboard
    g_hwndDashboard = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", 
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_TABSTOP,
        margin, currentY, editWidth, dashboardHeight, hwndParent, (HMENU)IDC_STATIC_DASHBOARD, g_hInstance, NULL);
    SendMessageW(g_hwndDashboard, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    currentY += dashboardHeight + gap * 2;

    g_hwndLogLabel = CreateWindowExW(0, L"STATIC", L"Log Output:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        margin, currentY, labelWidth, labelHeight, hwndParent, (HMENU)IDC_STATIC_LOG_LABEL, g_hInstance, NULL);
    SendMessageW(g_hwndLogLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    currentY += labelHeight + gap;

    int logHeight = clientRect.bottom - currentY - controlHeight - gap * 3 - margin; 
    if (logHeight < 50) logHeight = 50; 
    g_hwndLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP,
        margin, currentY, editWidth, logHeight, hwndParent, (HMENU)IDC_EDIT_LOG, g_hInstance, NULL);
    SendMessageW(g_hwndLog, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    currentY += logHeight + gap * 2;

    int suffixLabelWidth = 80;
    g_hwndInputLabel = CreateWindowExW(0, L"STATIC", L"Cmd Suffix:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        margin, currentY, suffixLabelWidth, labelHeight, hwndParent, (HMENU)IDC_STATIC_INPUT_LABEL, g_hInstance, NULL);
    SendMessageW(g_hwndInputLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    int buttonWidth = 100;
    int suffixEditX = margin + suffixLabelWidth + gap;
    int suffixEditWidth = editWidth - suffixLabelWidth - gap - buttonWidth - gap;
    g_hwndInputEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
        suffixEditX, currentY, suffixEditWidth, controlHeight, hwndParent, (HMENU)IDC_EDIT_INPUT, g_hInstance, NULL);
    SendMessageW(g_hwndInputEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    int buttonX = suffixEditX + suffixEditWidth + gap;
    g_hwndButtonAdd = CreateWindowExW(0, L"BUTTON", L"Add to Queue",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 
        buttonX, currentY, buttonWidth, controlHeight, hwndParent, (HMENU)IDC_BUTTON_ADD, g_hInstance, NULL);
    SendMessageW(g_hwndButtonAdd, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    SetFocus(g_hwndInputEdit);
}

void UpdateDashboardUI(void) {
    if (!g_hwndDashboard || !g_hwndMain) return;

    wchar_t dashboardText[8192]; // Increased buffer for dashboard display
    dashboardText[0] = L'\0';    // Initialize to empty string

    EnterCriticalSection(&g_dashboardLock);
    wchar_t currentCmdCopy[sizeof(g_currentCommand) / sizeof(wchar_t)];
    wcscpy_s(currentCmdCopy, sizeof(currentCmdCopy) / sizeof(wchar_t), g_currentCommand);
    LeaveCriticalSection(&g_dashboardLock);

    // Temporary storage for queue items to minimize time queueLock is held
    QueuedTask tempQueueCopy[MAX_QUEUE_SIZE];
    int tempQueueCount = 0;

    EnterCriticalSection(&g_queueLock);
    int queueLen = g_queueCount;
    if (queueLen > 0) {
        int currentHead = g_queueHead;
        for (int i = 0; i < queueLen; ++i) {
            if (tempQueueCount >= MAX_QUEUE_SIZE) break; // Should not happen if queueLen is correct
            int idx = (currentHead + i) % MAX_QUEUE_SIZE;
            if (g_taskQueue[idx].prefix && g_taskQueue[idx].suffix) {
                tempQueueCopy[tempQueueCount].prefix = _wcsdup(g_taskQueue[idx].prefix);
                tempQueueCopy[tempQueueCount].suffix = _wcsdup(g_taskQueue[idx].suffix);
                // Check if _wcsdup succeeded
                if (tempQueueCopy[tempQueueCount].prefix && tempQueueCopy[tempQueueCount].suffix) {
                    tempQueueCount++;
                } else { // Free if one allocation failed but other succeeded
                    if (tempQueueCopy[tempQueueCount].prefix) free(tempQueueCopy[tempQueueCount].prefix);
                    if (tempQueueCopy[tempQueueCount].suffix) free(tempQueueCopy[tempQueueCount].suffix);
                    tempQueueCopy[tempQueueCount].prefix = NULL; // Mark as invalid
                    tempQueueCopy[tempQueueCount].suffix = NULL;
                    // Optionally log an error here
                }
            }
        }
    }
    LeaveCriticalSection(&g_queueLock);

    // Format the dashboardText using the copied queue data
    size_t currentTextLen = 0;
    int written = swprintf(dashboardText, sizeof(dashboardText) / sizeof(wchar_t),
                           L"Current CMD:\r\n%s\r\n\r\nQueue (%d pending):", currentCmdCopy, queueLen);
    
    if (written < 0) { // Error in swprintf or buffer too small for header
        wcscpy_s(dashboardText, sizeof(dashboardText) / sizeof(wchar_t), L"Error generating dashboard header.");
    } else {
        currentTextLen = written;
    }

    if (queueLen == 0) {
        const wchar_t* emptyMsg = L"\r\n[Empty]";
        if (currentTextLen + wcslen(emptyMsg) < sizeof(dashboardText) / sizeof(wchar_t)) {
            wcscat_s(dashboardText, sizeof(dashboardText) / sizeof(wchar_t), emptyMsg);
        }
    } else {
        const wchar_t* rn = L"\r\n";
        if (currentTextLen + wcslen(rn) < sizeof(dashboardText) / sizeof(wchar_t)) {
            wcscat_s(dashboardText, sizeof(dashboardText) / sizeof(wchar_t), rn);
            currentTextLen += wcslen(rn);
        }

        for (int i = 0; i < tempQueueCount; ++i) {
            if (!tempQueueCopy[i].prefix || !tempQueueCopy[i].suffix) continue; // Skip if wcsdup failed

            wchar_t singleItem[512 + 1024 + 30]; // prefix + space + suffix + numbering + \r\n + safety
            written = swprintf(singleItem, sizeof(singleItem) / sizeof(wchar_t),
                               L"%d. %s %s\r\n",
                               i + 1,
                               tempQueueCopy[i].prefix,
                               tempQueueCopy[i].suffix);

            if (written > 0) {
                if (currentTextLen + (size_t)written < sizeof(dashboardText) / sizeof(wchar_t)) {
                    wcscat_s(dashboardText, sizeof(dashboardText) / sizeof(wchar_t), singleItem);
                    currentTextLen += written;
                } else {
                    const wchar_t* truncationMsg = L"... (queue list truncated)\r\n";
                    if (currentTextLen + wcslen(truncationMsg) < sizeof(dashboardText) / sizeof(wchar_t)) {
                        wcscat_s(dashboardText, sizeof(dashboardText) / sizeof(wchar_t), truncationMsg);
                    }
                    break; // Dashboard text buffer full
                }
            }
        }
    }

    SetWindowTextW(g_hwndDashboard, dashboardText);

    // Free duplicated strings
    for (int i = 0; i < tempQueueCount; ++i) {
        if (tempQueueCopy[i].prefix) free(tempQueueCopy[i].prefix);
        if (tempQueueCopy[i].suffix) free(tempQueueCopy[i].suffix);
    }
    // For EDIT control, ensure it's scrolled to the top to show the "Current CMD" first
    SendMessageW(g_hwndDashboard, EM_SETSEL, (WPARAM)0, (LPARAM)0);
    SendMessageW(g_hwndDashboard, EM_SCROLLCARET, 0, 0);
}


// --- Command Queue & Processing ---
void AddToQueue(const wchar_t* prefix, const wchar_t* suffix) {
    EnterCriticalSection(&g_queueLock);

    if (g_queueCount < MAX_QUEUE_SIZE) {
        g_taskQueue[g_queueTail].prefix = _wcsdup(prefix);
        g_taskQueue[g_queueTail].suffix = _wcsdup(suffix);
        // Check if _wcsdup succeeded
        if (!g_taskQueue[g_queueTail].prefix || !g_taskQueue[g_queueTail].suffix) {
            if (g_taskQueue[g_queueTail].prefix) free(g_taskQueue[g_queueTail].prefix);
            if (g_taskQueue[g_queueTail].suffix) free(g_taskQueue[g_queueTail].suffix);
            g_taskQueue[g_queueTail].prefix = NULL;
            g_taskQueue[g_queueTail].suffix = NULL;
            PostLogChunkToUI("Error: Memory allocation failed for new task.", TRUE, FALSE);
        } else {
            g_queueTail = (g_queueTail + 1) % MAX_QUEUE_SIZE;
            g_queueCount++;
            WakeConditionVariable(&g_queueNotEmpty);
        }
    } else {
        PostLogChunkToUI("Error: Command queue is full.", TRUE, FALSE);
    }

    LeaveCriticalSection(&g_queueLock);
}

QueuedTask GetFromQueue(void) {
    QueuedTask task = {NULL, NULL};
    EnterCriticalSection(&g_queueLock);

    while (g_queueCount == 0 && !g_appExiting) {
        SleepConditionVariableCS(&g_queueNotEmpty, &g_queueLock, INFINITE);
    }

    if (g_appExiting && g_queueCount == 0) { 
        LeaveCriticalSection(&g_queueLock);
        return task; 
    }
    
    if (g_queueCount > 0) {
        task = g_taskQueue[g_queueHead];
        g_taskQueue[g_queueHead].prefix = NULL; 
        g_taskQueue[g_queueHead].suffix = NULL;
        g_queueHead = (g_queueHead + 1) % MAX_QUEUE_SIZE;
        g_queueCount--;
    }

    LeaveCriticalSection(&g_queueLock);
    return task;
}

DWORD WINAPI PipeReaderThread(LPVOID lpParam) {
    HANDLE hPipeRead = (HANDLE)lpParam;
    char buffer[PIPE_BUFFER_SIZE];
    DWORD bytesRead;
    BOOL isStdErr = FALSE; // TODO: This should be passed as part of lpParam if differentiation is needed.
                           // For now, all pipe output is treated as non-stderr for color hints.

    char partialLineBuffer[PIPE_BUFFER_SIZE * 2] = {0}; 
    int partialLen = 0;

    while (ReadFile(hPipeRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buffer[bytesRead] = '\0'; 

        if (partialLen + bytesRead < sizeof(partialLineBuffer)) {
            // Using strncat_s or similar would be safer if buffer could contain nulls before bytesRead
            strcat_s(partialLineBuffer, sizeof(partialLineBuffer), buffer);
            partialLen += bytesRead;
        } else {
            PostLogChunkToUI("Pipe reader buffer overflow (partial line).", TRUE, FALSE); 
            // Process what's there, then copy the new buffer content, possibly losing middle part.
            // A more robust way would be to process partialLineBuffer, then process `buffer` directly.
            // For simplicity, we might just clear and restart with `buffer`.
            if (strlen(buffer) < sizeof(partialLineBuffer)) {
                strcpy_s(partialLineBuffer, sizeof(partialLineBuffer), buffer);
                partialLen = strlen(buffer);
            } else {
                 partialLineBuffer[0] = '\0'; // Cannot fit, discard
                 partialLen = 0;
                 PostLogChunkToUI("Pipe reader new chunk too large after overflow.", TRUE, FALSE);
            }
        }
        
        char* lineStart = partialLineBuffer;
        char* lineEnd;

        while ((lineEnd = strpbrk(lineStart, "\r\n")) != NULL) {
            BOOL isProgress = (*lineEnd == '\r' && *(lineEnd + 1) != '\n' && *(lineEnd + 1) != '\0');
            *lineEnd = '\0'; 

            PostLogChunkToUI(lineStart, isStdErr, isProgress);

            lineStart = lineEnd + 1; 
            if (*lineEnd == '\r' && *lineStart == '\n') { // Handle CRLF
                 lineStart++;
            }
        }
        
        if (*lineStart != '\0') {
            strcpy_s(partialLineBuffer, sizeof(partialLineBuffer), lineStart);
            partialLen = strlen(partialLineBuffer);
        } else {
            partialLineBuffer[0] = '\0';
            partialLen = 0;
        }
    }
    
    if (partialLen > 0) {
        PostLogChunkToUI(partialLineBuffer, isStdErr, FALSE);
    }

    return 0;
}


DWORD WINAPI CommandProcessorThread(LPVOID lpParam) {
    while (!g_appExiting) {
        QueuedTask task = GetFromQueue();
        if (g_appExiting && (task.prefix == NULL || task.suffix == NULL)) { 
             if (task.prefix) free(task.prefix);
             if (task.suffix) free(task.suffix);
             break;
        }
        if (task.prefix == NULL || task.suffix == NULL) continue; 

        wchar_t fullCmdLine[2048];
        swprintf(fullCmdLine, sizeof(fullCmdLine)/sizeof(wchar_t), L"%s %s", task.prefix, task.suffix);

        EnterCriticalSection(&g_dashboardLock);
        wcscpy_s(g_currentCommand, sizeof(g_currentCommand)/sizeof(wchar_t), fullCmdLine);
        LeaveCriticalSection(&g_dashboardLock);
        PostMessage(g_hwndMain, WM_APP_UPDATE_DASHBOARD, 0, 0);

        wchar_t logMsg[2100];
        swprintf(logMsg, sizeof(logMsg)/sizeof(wchar_t), L"$ %s", fullCmdLine);
        PostLogChunkToUI_Wide(logMsg, FALSE, TRUE); 

        STARTUPINFOW si = {0};
        PROCESS_INFORMATION pi = {0};
        SECURITY_ATTRIBUTES sa = {0};

        si.cb = sizeof(STARTUPINFOW);
        si.dwFlags |= STARTF_USESTDHANDLES;
        
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = NULL;

        HANDLE hChildStd_OUT_Rd = NULL;
        HANDLE hChildStd_OUT_Wr = NULL;
        HANDLE hChildStd_ERR_Rd = NULL;
        HANDLE hChildStd_ERR_Wr = NULL;

        // Create pipes for stdout
        if (!CreatePipe(&hChildStd_OUT_Rd, &hChildStd_OUT_Wr, &sa, 0)) {
            PostLogChunkToUI_Wide(L"CreatePipe (stdout) failed.", TRUE, FALSE);
            goto cleanup_task;
        }
        if (!SetHandleInformation(hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0)) { // Read handle is not inherited
            PostLogChunkToUI_Wide(L"SetHandleInformation (stdout_rd) failed.", TRUE, FALSE);
            CloseHandle(hChildStd_OUT_Rd); CloseHandle(hChildStd_OUT_Wr);
            goto cleanup_task;
        }
        si.hStdOutput = hChildStd_OUT_Wr;

        // Create pipes for stderr
        if (!CreatePipe(&hChildStd_ERR_Rd, &hChildStd_ERR_Wr, &sa, 0)) {
            PostLogChunkToUI_Wide(L"CreatePipe (stderr) failed.", TRUE, FALSE);
            CloseHandle(hChildStd_OUT_Rd); CloseHandle(hChildStd_OUT_Wr);
            goto cleanup_task;
        }
        if (!SetHandleInformation(hChildStd_ERR_Rd, HANDLE_FLAG_INHERIT, 0)) { // Read handle is not inherited
            PostLogChunkToUI_Wide(L"SetHandleInformation (stderr_rd) failed.", TRUE, FALSE);
            CloseHandle(hChildStd_OUT_Rd); CloseHandle(hChildStd_OUT_Wr);
            CloseHandle(hChildStd_ERR_Rd); CloseHandle(hChildStd_ERR_Wr);
            goto cleanup_task;
        }
        si.hStdError = hChildStd_ERR_Wr;
        
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); // Or NULL if no input redirection

        BOOL success = CreateProcessW(
            NULL, fullCmdLine, NULL, NULL, TRUE, 
            CREATE_NO_WINDOW, NULL, NULL, &si, &pi );

        // After CreateProcess, parent must close its write ends of pipes
        // so ReadFile on read ends will break when child closes its write ends.
        CloseHandle(hChildStd_OUT_Wr); hChildStd_OUT_Wr = NULL; // Mark as closed
        CloseHandle(hChildStd_ERR_Wr); hChildStd_ERR_Wr = NULL; // Mark as closed

        if (success) {
            // TODO: Pass struct to PipeReaderThread to indicate if it's for stderr
            HANDLE hStdOutReader = CreateThread(NULL, 0, PipeReaderThread, hChildStd_OUT_Rd, 0, NULL);
            HANDLE hStdErrReader = CreateThread(NULL, 0, PipeReaderThread, hChildStd_ERR_Rd, 0, NULL);
            // If CreateThread fails, hStdOutReader/hStdErrReader will be NULL.

            WaitForSingleObject(pi.hProcess, INFINITE);
            
            DWORD exitCode;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            
            if(hStdOutReader) { WaitForSingleObject(hStdOutReader, 5000); CloseHandle(hStdOutReader); }
            if(hStdErrReader) { WaitForSingleObject(hStdErrReader, 5000); CloseHandle(hStdErrReader); }

            wchar_t exitMsg[100];
            swprintf(exitMsg, sizeof(exitMsg)/sizeof(wchar_t), L"Process finished. Exit code: %lu", exitCode);
            PostLogChunkToUI_Wide(exitMsg, FALSE, TRUE);

            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            wchar_t errorMsg[256];
            swprintf(errorMsg, sizeof(errorMsg)/sizeof(wchar_t), L"Error starting command: %s (Code: %lu)", fullCmdLine, GetLastError());
            PostLogChunkToUI_Wide(errorMsg, TRUE, TRUE);
        }

        CloseHandle(hChildStd_OUT_Rd); 
        CloseHandle(hChildStd_ERR_Rd);

cleanup_task:
        free(task.prefix);
        free(task.suffix);
        PostMessage(g_hwndMain, WM_APP_COMMAND_DONE, 0, 0); 
    }
    return 0;
}


// --- String Utilities ---
wchar_t* Utf8ToWide(const char* utf8String) {
    if (!utf8String) return NULL;
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8String, -1, NULL, 0);
    if (wideLen == 0) return NULL;
    wchar_t* wideString = (wchar_t*)malloc(wideLen * sizeof(wchar_t));
    if (!wideString) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, utf8String, -1, wideString, wideLen);
    return wideString;
}

void PostLogChunkToUI_Wide(const wchar_t* wide_chunk, BOOL is_stderr_color_hint, BOOL is_progress) {
    if (!g_hwndMain) { 
        if (wide_chunk) wprintf(L"%s\n", wide_chunk); 
        return;
    }
    if (!wide_chunk) return;

    LogChunk* chunkData = (LogChunk*)malloc(sizeof(LogChunk));
    if (!chunkData) return;
    
    chunkData->text = _wcsdup(wide_chunk); 
    if (!chunkData->text) {
        free(chunkData);
        return;
    }
    chunkData->is_progress_line = is_progress;
    
    PostMessageW(g_hwndMain, WM_APP_APPEND_LOG_CHUNK, (WPARAM)is_stderr_color_hint, (LPARAM)chunkData);
}

void PostLogChunkToUI(const char* utf8_chunk, BOOL is_stderr_color_hint, BOOL is_progress) {
    if (!g_hwndMain && utf8_chunk) { 
        printf("%s\n", utf8_chunk); 
        return;
    }
    if (!utf8_chunk) return;

    LogChunk* chunkData = (LogChunk*)malloc(sizeof(LogChunk));
    if (!chunkData) return;
    
    chunkData->text = Utf8ToWide(utf8_chunk);
    if (!chunkData->text) {
        free(chunkData);
        return;
    }
    TrimTrailingCr(chunkData->text); 

    chunkData->is_progress_line = is_progress;
    
    PostMessageW(g_hwndMain, WM_APP_APPEND_LOG_CHUNK, (WPARAM)is_stderr_color_hint, (LPARAM)chunkData);
}

void TrimTrailingCr(wchar_t* str) {
    if (!str) return;
    size_t len = wcslen(str);
    if (len > 0 && str[len - 1] == L'\r') {
        // Only trim \r if it's not part of \r\n
        // (i.e., it's the last char, or the char before it is not \n)
        // The pipe reader tries to handle \r for progress lines already.
        // This is a fallback.
        if (len == 1 || (len > 1 && str[len - 2] != L'\n')) { 
            str[len - 1] = L'\0';
        }
    }
}

// char* WideToUtf8(const wchar_t* wideString) {
//    if (!wideString) return NULL;
//    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wideString, -1, NULL, 0, NULL, NULL);
//    if (utf8Len == 0) return NULL;
//    char* utf8String = (char*)malloc(utf8Len); // No * sizeof(char) as it's 1
//    if (!utf8String) return NULL;
//    WideCharToMultiByte(CP_UTF8, 0, wideString, -1, utf8String, utf8Len, NULL, NULL);
//    return utf8String;
//}

