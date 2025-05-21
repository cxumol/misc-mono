#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#define MAX_LOG_LINES 200
#define WINDOW_WIDTH 600
#define WINDOW_HEIGHT 500

// Control IDs
#define IDC_STATIC_PREFIX_LABEL 100
#define IDC_EDIT_PREFIX 101
#define IDC_STATIC_DASHBOARD_LABEL 102
#define IDC_STATIC_DASHBOARD 103
#define IDC_STATIC_LOG_LABEL 104
#define IDC_EDIT_LOG 105
#define IDC_STATIC_INPUT_LABEL 106
#define IDC_EDIT_INPUT 107
#define IDC_BUTTON_ADD 108

// Global variables
HWND hwndMain, hwndPrefixLabel, hwndPrefixEdit, hwndDashboardLabel, hwndDashboard, hwndLogLabel, hwndLog, hwndInputLabel, hwndInput, hwndButtonAdd;
HINSTANCE hInstance;
HFONT hFont;
char logLines[MAX_LOG_LINES][1024];
int logLineCount = 0;
char currentCommand[1024] = "Idle";
char cmdPrefixFlagValue[] = "yt-dlp -f 233 -N 12"; // Default for the new prefix input

// Function prototypes
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void addToLog(const char *line);
void updateLogUI();
void updateDashboardUI();
void setCurrentCommandUI(const char *cmdStr);
void commandProcessor();
void setFocusToInput();

// Helper functions
void addToLog(const char *line) {
    if (logLineCount < MAX_LOG_LINES) {
        strcpy(logLines[logLineCount], line);
        logLineCount++;
    } else {
        memmove(logLines, logLines + 1, (MAX_LOG_LINES - 1) * sizeof(logLines[0]));
        strcpy(logLines[MAX_LOG_LINES - 1], line);
    }
    updateLogUI();
}

void updateLogUI() {
    char fullLogText[8192] = "";
    for (int i = 0; i < logLineCount; i++) {
        strcat(fullLogText, logLines[i]);
        strcat(fullLogText, "\r\n");
    }
    SetWindowText(hwndLog, fullLogText);
}

void updateDashboardUI() {
    char dashboardText[1024];
    sprintf(dashboardText, "Current CMD:\n%s\nQueue: %d", currentCommand, 0);
    SetWindowText(hwndDashboard, dashboardText);
}

void setCurrentCommandUI(const char *cmdStr) {
    strcpy(currentCommand, cmdStr);
    updateDashboardUI();
}

void setFocusToInput() {
    SetFocus(hwndInput);
}

// Windows Message Handler
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        hFont = CreateFont(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH, "Segoe UI");

        // Command Prefix Label
        hwndPrefixLabel = CreateWindow("STATIC", "Command Prefix:", WS_CHILD | WS_VISIBLE, 10, 10, 580, 20, hwnd, (HMENU)IDC_STATIC_PREFIX_LABEL, hInstance, NULL);
        SendMessage(hwndPrefixLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Command Prefix Edit Box
        hwndPrefixEdit = CreateWindow("EDIT", cmdPrefixFlagValue, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 10, 35, 560, 25, hwnd, (HMENU)IDC_EDIT_PREFIX, hInstance, NULL);
        SendMessage(hwndPrefixEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Dashboard Label
        hwndDashboardLabel = CreateWindow("STATIC", "Dashboard:", WS_CHILD | WS_VISIBLE, 10, 75, 580, 20, hwnd, (HMENU)IDC_STATIC_DASHBOARD_LABEL, hInstance, NULL);
        SendMessage(hwndDashboardLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Dashboard Display
        hwndDashboard = CreateWindow("STATIC", "", WS_CHILD | WS_VISIBLE, 10, 100, 560, 80, hwnd, (HMENU)IDC_STATIC_DASHBOARD, hInstance, NULL);
        SendMessage(hwndDashboard, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Log Label
        hwndLogLabel = CreateWindow("STATIC", "Log Output:", WS_CHILD | WS_VISIBLE, 10, 190, 580, 20, hwnd, (HMENU)IDC_STATIC_LOG_LABEL, hInstance, NULL);
        SendMessage(hwndLogLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Log Edit Box
        hwndLog = CreateWindow("EDIT", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 10, 215, 560, 200, hwnd, (HMENU)IDC_EDIT_LOG, hInstance, NULL);
        SendMessage(hwndLog, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Input Label
        hwndInputLabel = CreateWindow("STATIC", "Cmd Suffix:", WS_CHILD | WS_VISIBLE, 10, 425, 80, 20, hwnd, (HMENU)IDC_STATIC_INPUT_LABEL, hInstance, NULL);
        SendMessage(hwndInputLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Input Edit Box
        hwndInput = CreateWindow("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 100, 425, 370, 25, hwnd, (HMENU)IDC_EDIT_INPUT, hInstance, NULL);
        SendMessage(hwndInput, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Add Button
        hwndButtonAdd = CreateWindow("BUTTON", "Add to Queue", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 480, 425, 100, 25, hwnd, (HMENU)IDC_BUTTON_ADD, hInstance, NULL);
        SendMessage(hwndButtonAdd, WM_SETFONT, (WPARAM)hFont, TRUE);

        setFocusToInput();
        break;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BUTTON_ADD) {
            // Get Prefix
            char prefixStr[1024];
            GetWindowText(hwndPrefixEdit, prefixStr, sizeof(prefixStr));

            if (strlen(prefixStr) == 0) {
                addToLog("Error: Command prefix cannot be empty.");
                setFocusToInput();
                return 0;
            }

            // Get Suffix
            char suffixStr[1024];
            GetWindowText(hwndInput, suffixStr, sizeof(suffixStr));

            // Process task
            setCurrentCommandUI(prefixStr);
            addToLog("Added to queue");

            setFocusToInput(); // Focus back to input
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {  // Enter key pressed
            PostMessage(hwnd, WM_COMMAND, IDC_BUTTON_ADD, 0);
        }
        break;

    case WM_CLOSE:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    return 0;
}

void commandProcessor() {
    // Simulate the process (e.g., a dummy long-running task)
    Sleep(3000);
    addToLog("Task completed");
}

int main() {
    // Initialize Window
    hInstance = GetModuleHandle(NULL);
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "CmdQueueClass";
    RegisterClass(&wc);

    hwndMain = CreateWindowEx(0, "CmdQueueClass", "Command Queue", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, WINDOW_WIDTH, WINDOW_HEIGHT, NULL, NULL, hInstance, NULL);

    if (!hwndMain) {
        return 1;
    }

    ShowWindow(hwndMain, SW_SHOW);
    UpdateWindow(hwndMain);

    // Start the command processor in a separate thread or background process
    CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)commandProcessor, NULL, 0, NULL);

    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
