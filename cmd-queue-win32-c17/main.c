// #define UNICODE
// #define _UNICODE

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h> // For wcscat_s, wcscpy_s, etc.
#include <tchar.h> // For _TCHAR, _tcscpy, etc. (though direct W functions are used)
#include <commctrl.h> // For some common controls if ever needed, not strictly for this set.

// --- Configuration ---
#define MAX_LOG_LINES_IN_EDIT_CONTROL 200
#define DEFAULT_CMD_PREFIX L"yt-dlp -f 233 -N 12"
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
void PostLogChunkToUI(const char* utf8_chunk, BOOL is_stderr, BOOL is_progress);
wchar_t* Utf8ToWide(const char* utf8String);
char* WideToUtf8(const wchar_t* wideString); // For command arguments if needed
void InitializeUIFont(void);
void CreateControls(HWND hwndParent);
void TrimTrailingCr(wchar_t* str);


// --- Entry Point ---
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    g_hInstance = hInstance;

    // Initialize COM for condition variables, though not strictly necessary for basic use
    // CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

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
    PostLogChunkToUI((const char*)initialMsg, FALSE, TRUE); // Using TRUE for is_progress to ensure it shows up
    PostLogChunkToUI("Enter command suffix and click 'Add to Queue' or press Enter.", FALSE, FALSE);
    PostLogChunkToUI("Close window or press Alt+F4 to quit.", FALSE, FALSE);


    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int windowWidth = 600;
    int windowHeight = 500;
    int windowX = (screenWidth - windowWidth) / 2;
    int windowY = (screenHeight - windowHeight) / 2;

    g_hwndMain = CreateWindowExW(
        0, // WS_EX_CLIENTEDGE for sunken border, not usually on main window
        WINDOW_CLASS_NAME,
        L"Cmd Queue GUI (C Version)",
        WS_OVERLAPPEDWINDOW, // Standard window
        windowX, windowY, windowWidth, windowHeight,
        NULL, NULL, hInstance, NULL
    );

    if (!g_hwndMain) {
        MessageBoxW(NULL, L"Window Creation Failed!", L"Error", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

    ShowWindow(g_hwndMain, nCmdShow);
    UpdateWindow(g_hwndMain);

    // Start command processor thread
    g_hCommandProcessorThread = CreateThread(NULL, 0, CommandProcessorThread, NULL, 0, NULL);
    if (g_hCommandProcessorThread == NULL) {
        MessageBoxW(NULL, L"Failed to create command processor thread!", L"Error", MB_ICONEXCLAMATION | MB_OK);
        // Handle error: clean up, exit, etc.
        return 1;
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        // Special handling for Enter key in the suffix input to trigger "Add" button
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            HWND hFocused = GetFocus();
            if (hFocused == g_hwndInputEdit) {
                 // Simulate button click by sending WM_COMMAND
                SendMessage(g_hwndMain, WM_COMMAND, MAKEWPARAM(IDC_BUTTON_ADD, BN_CLICKED), (LPARAM)g_hwndButtonAdd);
                continue; // Message handled, don't pass to Translate/Dispatch
            }
        }
        // Standard message loop for non-dialog windows
        // If IsDialogMessage is needed for TAB key navigation between controls and accelerators:
        // if (!IsDialogMessage(g_hwndMain, &msg)) { // g_hwndMain isn't a dialog, but for modeless dialog behavior
             TranslateMessage(&msg);
             DispatchMessage(&msg);
        // }
    }

    // Wait for command processor thread to finish
    if (g_hCommandProcessorThread) {
        g_appExiting = TRUE;
        EnterCriticalSection(&g_queueLock);
        WakeConditionVariable(&g_queueNotEmpty); // Wake up if sleeping
        LeaveCriticalSection(&g_queueLock);
        WaitForSingleObject(g_hCommandProcessorThread, INFINITE);
        CloseHandle(g_hCommandProcessorThread);
    }

    DeleteCriticalSection(&g_queueLock);
    DeleteCriticalSection(&g_dashboardLock);
    // CoUninitialize();

    if (g_hFont) DeleteObject(g_hFont);
    
    return (int)msg.wParam;
}


// --- Window Procedure ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            CreateControls(hwnd);
            UpdateDashboardUI(); // Initial dashboard state
            break;

        case WM_COMMAND: {
            WORD controlId = LOWORD(wParam);
            WORD notifyCode = HIWORD(wParam); // e.g., BN_CLICKED

            if (controlId == IDC_BUTTON_ADD && notifyCode == BN_CLICKED) {
                wchar_t prefix_buffer[512];
                wchar_t suffix_buffer[1024];

                GetWindowTextW(g_hwndPrefixEdit, prefix_buffer, sizeof(prefix_buffer)/sizeof(wchar_t));
                GetWindowTextW(g_hwndInputEdit, suffix_buffer, sizeof(suffix_buffer)/sizeof(wchar_t));
                
                // Trim whitespace (basic example)
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
                    PostLogChunkToUI((const char*)logMsg, FALSE, TRUE); // Use TRUE for is_progress to force display logic
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
            SendMessageW(g_hwndLog, EM_SETSEL, currentLen, currentLen); // Move caret to end

            if (chunk->is_progress_line) {
                // Find start of the last visible line to replace it
                // This is tricky if lines wrap. Assuming no wrap for ES_AUTOHSCROLL.
                // A simple approach: if the log already contains text, and this is a progress line,
                // try to replace the content after the last \r\n.
                // More robust: Get current line count, then index of last line.
                LRESULT lineCount = SendMessageW(g_hwndLog, EM_GETLINECOUNT, 0, 0);
                if (lineCount > 0) {
                    LRESULT lastLineStartCharIndex = SendMessageW(g_hwndLog, EM_LINEINDEX, lineCount - 1, 0);
                    if (lastLineStartCharIndex != (LRESULT)-1) {
                         SendMessageW(g_hwndLog, EM_SETSEL, lastLineStartCharIndex, -1); // Select last line
                    }
                }
            }
            
            // Append text (or replace selection if set above)
            SendMessageW(g_hwndLog, EM_REPLACESEL, TRUE, (LPARAM)chunk->text);

            // Ensure log does not exceed max lines
            LRESULT lines = SendMessageW(g_hwndLog, EM_GETLINECOUNT, 0, 0);
            while (lines > MAX_LOG_LINES_IN_EDIT_CONTROL) {
                LRESULT firstLineEnd = SendMessageW(g_hwndLog, EM_LINEINDEX, 1, 0); // Index of start of 2nd line
                if (firstLineEnd == -1) { // Only one very long line, or error
                    SendMessageW(g_hwndLog, EM_SETSEL, 0, GetWindowTextLengthW(g_hwndLog)/2); // Delete half
                } else {
                    SendMessageW(g_hwndLog, EM_SETSEL, 0, firstLineEnd); // Select first line
                }
                SendMessageW(g_hwndLog, EM_REPLACESEL, TRUE, (LPARAM)L""); // Delete selected
                lines = SendMessageW(g_hwndLog, EM_GETLINECOUNT, 0, 0);
            }

            // Scroll to end
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
            // Command finished, update dashboard (current command becomes "Idle")
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
            // Basic resize handling (can be improved)
            // For simplicity, this example doesn't dynamically resize all controls perfectly.
            // You'd use MoveWindow for each child control here based on GetClientRect.
            // Example: MoveWindow(g_hwndLog, 10, y_pos_log, LOWORD(lParam) - 20, new_log_height, TRUE);
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            break;

        case WM_DESTROY:
            g_appExiting = TRUE; // Signal threads
            EnterCriticalSection(&g_queueLock);
            WakeConditionVariable(&g_queueNotEmpty); // Wake command processor if it's waiting
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
    lf.lfHeight = -MulDiv(10, GetDeviceCaps(GetDC(NULL), LOGPIXELSY), 72); // 10pt font
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


    // Command Prefix Label
    g_hwndPrefixLabel = CreateWindowExW(0, L"STATIC", L"Command Prefix:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        margin, currentY, labelWidth, labelHeight, hwndParent, (HMENU)IDC_STATIC_PREFIX_LABEL, g_hInstance, NULL);
    SendMessageW(g_hwndPrefixLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    currentY += labelHeight + gap;

    // Command Prefix Edit
    g_hwndPrefixEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_initialCmdPrefix,
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
        margin, currentY, editWidth, controlHeight, hwndParent, (HMENU)IDC_EDIT_PREFIX, g_hInstance, NULL);
    SendMessageW(g_hwndPrefixEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    currentY += controlHeight + gap * 2;

    // Dashboard Label
    g_hwndDashboardLabel = CreateWindowExW(0, L"STATIC", L"Dashboard:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        margin, currentY, labelWidth, labelHeight, hwndParent, (HMENU)IDC_STATIC_DASHBOARD_LABEL, g_hInstance, NULL);
    SendMessageW(g_hwndDashboardLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    currentY += labelHeight + gap;

    // Dashboard Display (Static Text)
    g_hwndDashboard = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"", // Initially empty
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, // SS_LEFTNOWORDWRAP important for multi-line static
        margin, currentY, editWidth, 60, hwndParent, (HMENU)IDC_STATIC_DASHBOARD, g_hInstance, NULL);
    SendMessageW(g_hwndDashboard, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    currentY += 60 + gap * 2;

    // Log Label
    g_hwndLogLabel = CreateWindowExW(0, L"STATIC", L"Log Output:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        margin, currentY, labelWidth, labelHeight, hwndParent, (HMENU)IDC_STATIC_LOG_LABEL, g_hInstance, NULL);
    SendMessageW(g_hwndLogLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    currentY += labelHeight + gap;

    // Log Edit Control
    int logHeight = clientRect.bottom - currentY - controlHeight - gap * 3 - margin; // Calculate remaining height
    if (logHeight < 50) logHeight = 50; // Minimum height
    g_hwndLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP,
        margin, currentY, editWidth, logHeight, hwndParent, (HMENU)IDC_EDIT_LOG, g_hInstance, NULL);
    SendMessageW(g_hwndLog, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    currentY += logHeight + gap * 2;

    // Suffix Input Label
    int suffixLabelWidth = 80;
    g_hwndInputLabel = CreateWindowExW(0, L"STATIC", L"Cmd Suffix:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        margin, currentY, suffixLabelWidth, labelHeight, hwndParent, (HMENU)IDC_STATIC_INPUT_LABEL, g_hInstance, NULL);
    SendMessageW(g_hwndInputLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    // Suffix Input Edit Control
    int buttonWidth = 100;
    int suffixEditX = margin + suffixLabelWidth + gap;
    int suffixEditWidth = editWidth - suffixLabelWidth - gap - buttonWidth - gap;
    g_hwndInputEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
        suffixEditX, currentY, suffixEditWidth, controlHeight, hwndParent, (HMENU)IDC_EDIT_INPUT, g_hInstance, NULL);
    SendMessageW(g_hwndInputEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    // Add Button
    int buttonX = suffixEditX + suffixEditWidth + gap;
    g_hwndButtonAdd = CreateWindowExW(0, L"BUTTON", L"Add to Queue",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, // BS_DEFPUSHBUTTON might also work well here
        buttonX, currentY, buttonWidth, controlHeight, hwndParent, (HMENU)IDC_BUTTON_ADD, g_hInstance, NULL);
    SendMessageW(g_hwndButtonAdd, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    SetFocus(g_hwndInputEdit);
}

void UpdateDashboardUI(void) {
    if (!g_hwndDashboard || !g_hwndMain) return;

    EnterCriticalSection(&g_queueLock);
    int queueLen = g_queueCount;
    LeaveCriticalSection(&g_queueLock);

    EnterCriticalSection(&g_dashboardLock);
    wchar_t currentCmdCopy[sizeof(g_currentCommand)/sizeof(wchar_t)];
    wcscpy_s(currentCmdCopy, sizeof(currentCmdCopy)/sizeof(wchar_t), g_currentCommand);
    LeaveCriticalSection(&g_dashboardLock);

    wchar_t dashboardText[1024];
    swprintf(dashboardText, sizeof(dashboardText)/sizeof(wchar_t),
             L"Current CMD:\r\n%s\r\n\r\nQueue (%d pending):", currentCmdCopy, queueLen);

    if (queueLen == 0) {
        wcscat_s(dashboardText, sizeof(dashboardText)/sizeof(wchar_t), L"\r\n[Empty]");
    } else {
         wcscat_s(dashboardText, sizeof(dashboardText)/sizeof(wchar_t), L"\r\n(Tasks in queue...)");
    }
    SetWindowTextW(g_hwndDashboard, dashboardText);
}


// --- Command Queue & Processing ---
void AddToQueue(const wchar_t* prefix, const wchar_t* suffix) {
    EnterCriticalSection(&g_queueLock);

    if (g_queueCount < MAX_QUEUE_SIZE) {
        g_taskQueue[g_queueTail].prefix = _wcsdup(prefix);
        g_taskQueue[g_queueTail].suffix = _wcsdup(suffix);
        g_queueTail = (g_queueTail + 1) % MAX_QUEUE_SIZE;
        g_queueCount++;
        WakeConditionVariable(&g_queueNotEmpty);
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

    if (g_appExiting && g_queueCount == 0) { // Check again after waking if exiting
        LeaveCriticalSection(&g_queueLock);
        return task; // Return empty task
    }
    
    if (g_queueCount > 0) {
        task = g_taskQueue[g_queueHead];
        g_taskQueue[g_queueHead].prefix = NULL; // Prevent double free if app exits abruptly
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
    BOOL isStdErr = FALSE; // This needs to be passed if we differentiate, or use two threads

    // A simple way to check if it's stderr pipe (if we had two such threads)
    // For now, assume stdout for this example thread, or make it generic.

    char partialLineBuffer[PIPE_BUFFER_SIZE * 2] = {0}; // Buffer for incomplete UTF-8 sequences or lines
    int partialLen = 0;

    while (ReadFile(hPipeRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buffer[bytesRead] = '\0'; // Null-terminate the read chunk

        // Append to partial line buffer
        if (partialLen + bytesRead < sizeof(partialLineBuffer)) {
            strcat_s(partialLineBuffer, sizeof(partialLineBuffer), buffer);
            partialLen += bytesRead;
        } else {
            // Buffer overflow, log an error and process what we have
            PostLogChunkToUI("Pipe reader buffer overflow.", TRUE, FALSE); // is_stderr = TRUE for error
            // process partialLineBuffer as is
            partialLen = 0; // reset
        }
        
        char* lineStart = partialLineBuffer;
        char* lineEnd;

        while ((lineEnd = strpbrk(lineStart, "\r\n")) != NULL) {
            BOOL isProgress = (*lineEnd == '\r' && *(lineEnd + 1) != '\n');
            *lineEnd = '\0'; // Null-terminate the line

            PostLogChunkToUI(lineStart, isStdErr, isProgress);

            lineStart = lineEnd + 1; // Move to start of next potential line
            if (isProgress && *lineStart == '\n') { // Handle CRLF if \r was for progress line end
                lineStart++; 
            } else if (!isProgress && *lineEnd == '\r' && *lineStart == '\n') { // Normal CRLF
                 lineStart++;
            }
        }
        
        // Move remaining part to the beginning of partialLineBuffer
        if (*lineStart != '\0') {
            strcpy_s(partialLineBuffer, sizeof(partialLineBuffer), lineStart);
            partialLen = strlen(partialLineBuffer);
        } else {
            partialLineBuffer[0] = '\0';
            partialLen = 0;
        }
    }
    
    // Process any remaining data in partialLineBuffer after pipe closes
    if (partialLen > 0) {
        PostLogChunkToUI(partialLineBuffer, isStdErr, FALSE);
    }

    return 0;
}


DWORD WINAPI CommandProcessorThread(LPVOID lpParam) {
    while (!g_appExiting) {
        QueuedTask task = GetFromQueue();
        if (g_appExiting && (task.prefix == NULL || task.suffix == NULL)) { // Check if woken up to exit
             if (task.prefix) free(task.prefix);
             if (task.suffix) free(task.suffix);
             break;
        }
        if (task.prefix == NULL) continue; // Should not happen if not exiting

        wchar_t fullCmdLine[2048];
        swprintf(fullCmdLine, sizeof(fullCmdLine)/sizeof(wchar_t), L"%s %s", task.prefix, task.suffix);

        EnterCriticalSection(&g_dashboardLock);
        wcscpy_s(g_currentCommand, sizeof(g_currentCommand)/sizeof(wchar_t), fullCmdLine);
        LeaveCriticalSection(&g_dashboardLock);
        PostMessage(g_hwndMain, WM_APP_UPDATE_DASHBOARD, 0, 0);

        wchar_t logMsg[2100];
        swprintf(logMsg, sizeof(logMsg)/sizeof(wchar_t), L"$ %s", fullCmdLine);
        PostLogChunkToUI((const char*)logMsg, FALSE, TRUE); // Post as UTF-16 directly for this internal log

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

        CreatePipe(&hChildStd_OUT_Rd, &hChildStd_OUT_Wr, &sa, 0);
        SetHandleInformation(hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0); // Read handle is not inherited
        si.hStdOutput = hChildStd_OUT_Wr;

        CreatePipe(&hChildStd_ERR_Rd, &hChildStd_ERR_Wr, &sa, 0);
        SetHandleInformation(hChildStd_ERR_Rd, HANDLE_FLAG_INHERIT, 0);
        si.hStdError = hChildStd_ERR_Wr;
        
        // si.hStdInput can be set to GetStdHandle(STD_INPUT_HANDLE) or a pipe for input if needed
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); // Or NULL if no input redirection

        BOOL success = CreateProcessW(
            NULL,           // Application name (use NULL if cmd line has it)
            fullCmdLine,    // Command line (mutable buffer)
            NULL,           // Process security attributes
            NULL,           // Thread security attributes
            TRUE,           // Inherit handles (for pipes)
            CREATE_NO_WINDOW, // Creation flags: no console window
            NULL,           // Environment block (NULL for parent's)
            NULL,           // Current directory (NULL for parent's)
            &si,            // STARTUPINFO
            &pi             // PROCESS_INFORMATION
        );

        if (success) {
            CloseHandle(hChildStd_OUT_Wr); // Close write ends in parent
            CloseHandle(hChildStd_ERR_Wr);

            HANDLE hStdOutReader = CreateThread(NULL, 0, PipeReaderThread, hChildStd_OUT_Rd, 0, NULL);
            HANDLE hStdErrReader = CreateThread(NULL, 0, PipeReaderThread, hChildStd_ERR_Rd, 0, NULL);
            // TODO: In PipeReaderThread, differentiate if it's stdout or stderr to pass to PostLogChunkToUI

            WaitForSingleObject(pi.hProcess, INFINITE);
            
            DWORD exitCode;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            
            // Wait for reader threads to finish processing all output
            if(hStdOutReader) WaitForSingleObject(hStdOutReader, 5000); // Timeout
            if(hStdErrReader) WaitForSingleObject(hStdErrReader, 5000); // Timeout
            
            if(hStdOutReader) CloseHandle(hStdOutReader);
            if(hStdErrReader) CloseHandle(hStdErrReader);

            wchar_t exitMsg[100];
            swprintf(exitMsg, sizeof(exitMsg)/sizeof(wchar_t), L"Process finished. Exit code: %lu", exitCode);
            PostLogChunkToUI((const char*)exitMsg, FALSE, TRUE);

            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            wchar_t errorMsg[256];
            swprintf(errorMsg, sizeof(errorMsg)/sizeof(wchar_t), L"Error starting command: %s (Code: %lu)", fullCmdLine, GetLastError());
            PostLogChunkToUI((const char*)errorMsg, TRUE, TRUE);
        }
        CloseHandle(hChildStd_OUT_Rd); // Close read ends
        CloseHandle(hChildStd_ERR_Rd);

        free(task.prefix);
        free(task.suffix);
        PostMessage(g_hwndMain, WM_APP_COMMAND_DONE, 0, 0); // Signal UI command is done
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

// Used for posting internal log messages that are already wchar_t
// For actual pipe data, use the utf8_chunk version
void PostLogChunkToUI_Wide(const wchar_t* wide_chunk, BOOL is_stderr_color_hint, BOOL is_progress) {
    if (!g_hwndMain) { // Window not created yet
        wprintf(L"%s\n", wide_chunk); // Fallback to console if GUI not ready
        return;
    }
    LogChunk* chunkData = (LogChunk*)malloc(sizeof(LogChunk));
    if (!chunkData) return;
    
    chunkData->text = _wcsdup(wide_chunk); // Duplicate the string
    if (!chunkData->text) {
        free(chunkData);
        return;
    }
    chunkData->is_progress_line = is_progress;
    // is_stderr_color_hint is not used in current log display but could be
    
    PostMessageW(g_hwndMain, WM_APP_APPEND_LOG_CHUNK, (WPARAM)is_stderr_color_hint, (LPARAM)chunkData);
}


void PostLogChunkToUI(const char* utf8_chunk, BOOL is_stderr_color_hint, BOOL is_progress) {
    // This special handling for wchar_t* cast is because some internal logs are already wide
    if ((uintptr_t)utf8_chunk > 0xFFFF && wcslen((const wchar_t*)utf8_chunk) > 0 && ((const wchar_t*)utf8_chunk)[0] < 256 && IsWindow(g_hwndMain)) {
         // Heuristic: if it looks like a wide string pointer and window exists, assume it's wide.
         // This is a HACK because I mixed PostLogChunkToUI calls. Better to have two distinct functions.
         if (wcslen((const wchar_t*)utf8_chunk) < strlen(utf8_chunk)) { // More robust check
            PostLogChunkToUI_Wide((const wchar_t*)utf8_chunk, is_stderr_color_hint, is_progress);
            return;
         }
    }
    
    if (!g_hwndMain && utf8_chunk) { // Window not created yet
        printf("%s\n", utf8_chunk); // Fallback to console
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
    // Remove trailing \r if it's a progress line and not followed by \n
    // The pipe reader should ideally handle this better
    TrimTrailingCr(chunkData->text); 

    chunkData->is_progress_line = is_progress;
    
    PostMessageW(g_hwndMain, WM_APP_APPEND_LOG_CHUNK, (WPARAM)is_stderr_color_hint, (LPARAM)chunkData);
}

void TrimTrailingCr(wchar_t* str) {
    if (!str) return;
    size_t len = wcslen(str);
    if (len > 0 && str[len - 1] == L'\r') {
        if (len == 1 || (len > 1 && str[len - 2] != L'\n')) { // Check it's not part of \r\n
            str[len - 1] = L'\0';
        }
    }
}


// char* WideToUtf8(const wchar_t* wideString) { // If needed for command arguments
//     if (!wideString) return NULL;
//     int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wideString, -1, NULL, 0, NULL, NULL);
//     if (utf8Len == 0) return NULL;
//     char* utf8String = (char*)malloc(utf8Len); // No * sizeof(char) needed
//     if (!utf8String) return NULL;
//     WideCharToMultiByte(CP_UTF8, 0, wideString, -1, utf8String, utf8Len, NULL, NULL);
//     return utf8String;
// }
