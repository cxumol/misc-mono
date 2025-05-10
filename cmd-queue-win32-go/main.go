package main

import (
	"bufio"
	"context"
	"flag"
	"fmt"
	"io"
	"log"
	"os/exec"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

const (
	maxLogLines            = 200
	defaultCmdPrefixFlag   = "yt-dlp -f 233 -N 12" // Default for the new prefix input
	windowClassName        = "CmdQueueGUIWindowClass"

	// Control IDs
	IDC_STATIC_PREFIX_LABEL    = 100
	IDC_EDIT_PREFIX            = 101
	IDC_STATIC_DASHBOARD_LABEL = 102
	IDC_STATIC_DASHBOARD       = 103
	IDC_STATIC_LOG_LABEL       = 104
	IDC_EDIT_LOG               = 105
	IDC_STATIC_INPUT_LABEL     = 106
	IDC_EDIT_INPUT             = 107 // Suffix input
	IDC_BUTTON_ADD             = 108

	// Custom Window Messages for thread-safe UI updates
	WM_APP_ADD_LOG          = WM_APP + 1
	WM_APP_UPDATE_DASHBOARD = WM_APP + 2
	WM_APP_SET_CURRENT_CMD  = WM_APP + 3 // This might become less directly used if dashboard shows full cmd
)

// QueuedTask holds both prefix and suffix for a command
type QueuedTask struct {
	Prefix string
	Suffix string
}

var (
	cmdPrefixFlagValue string // Stores the initial value from the -cmd flag
	taskQueue          = make(chan QueuedTask, 100)
	logLines           []string
	logLock            sync.Mutex
	currentCommand     string = "Idle"
	currentCommandLock sync.Mutex

	// Win32 Handles
	hwndMain           syscall.Handle
	hwndPrefixLabel    syscall.Handle
	hwndPrefixEdit     syscall.Handle
	hwndDashboardLabel syscall.Handle
	hwndDashboard      syscall.Handle
	hwndLogLabel       syscall.Handle
	hwndLog            syscall.Handle
	hwndInputLabel     syscall.Handle // Suffix label
	hwndInput          syscall.Handle // Suffix input
	hwndButtonAdd      syscall.Handle
	hInstance          syscall.Handle
	hFont              syscall.Handle

	// Context for command processor
	appCtx    context.Context
	cancelApp context.CancelFunc
)

func init() {
	runtime.LockOSThread()
}

func addToLog(line string) {
	logLock.Lock()
	logLines = append(logLines, line)
	if len(logLines) > maxLogLines {
		logLines = logLines[len(logLines)-maxLogLines:]
	}
	logLock.Unlock()

	if hwndMain != 0 {
		PostMessage(hwndMain, WM_APP_ADD_LOG, 0, 0)
	}
}

func setCurrentCommandUI(cmdStr string) {
	currentCommandLock.Lock()
	currentCommand = cmdStr
	currentCommandLock.Unlock()

	if hwndMain != 0 {
		PostMessage(hwndMain, WM_APP_SET_CURRENT_CMD, 0, 0) // Signal change
		PostMessage(hwndMain, WM_APP_UPDATE_DASHBOARD, 0, 0) // Trigger dashboard UI update
	}
}

func getCurrentCommand() string {
	currentCommandLock.Lock()
	defer currentCommandLock.Unlock()
	return currentCommand
}

func updateDashboardUI() {
	if hwndDashboard == 0 || hwndMain == 0 {
		return
	}
	queueLen := len(taskQueue)
	currentCmdStr := getCurrentCommand()
	dashboardText := fmt.Sprintf("Current CMD:\n%s\n\nQueue (%d pending):\n", currentCmdStr, queueLen)
	if queueLen > 0 {
		// Optionally, show next task in queue if desired and easily accessible
		// For now, just indicating non-empty state.
		dashboardText += "(Tasks in queue...)"
	} else {
		dashboardText += "[Empty]"
	}
	SetWindowText(hwndDashboard, dashboardText)
}

func updateLogUI() {
	if hwndLog == 0 {
		return
	}
	logLock.Lock()
	fullLogText := strings.Join(logLines, "\r\n")
	logLock.Unlock()

	SetWindowText(hwndLog, fullLogText)

	// Scroll to end using EM_SETSEL and EM_SCROLLCARET
	textLen := GetWindowTextLength(hwndLog)
	SendMessage(hwndLog, EM_SETSEL, uintptr(textLen), uintptr(textLen))
	SendMessage(hwndLog, EM_SCROLLCARET, 0, 0)
}

func commandProcessor(ctx context.Context) {
	time.Sleep(100 * time.Millisecond) // Wait for UI to be roughly ready

	for {
		select {
		case <-ctx.Done():
			addToLog("Command processor shutting down...")
			return
		case task := <-taskQueue:
			if task.Suffix == "" && task.Prefix == "" { // Check if it's an empty task
				setCurrentCommandUI("Idle")
				continue
			}

			// Use task.Prefix and task.Suffix
			if strings.TrimSpace(task.Prefix) == "" {
				addToLog("Error: Command prefix is empty.")
				setCurrentCommandUI("Idle")
				continue
			}
			fullCmdStr := task.Prefix + " " + task.Suffix
			setCurrentCommandUI(fullCmdStr)
			addToLog(fmt.Sprintf("$ %s", fullCmdStr))

			var cmdArgs []string
			prefixParts := strings.Fields(task.Prefix)
			commandName := prefixParts[0]
			if len(prefixParts) > 1 {
				cmdArgs = append(cmdArgs, prefixParts[1:]...)
			}
			suffixParts := strings.Fields(task.Suffix) // Suffix can also have multiple parts
			cmdArgs = append(cmdArgs, suffixParts...)

			cmd := exec.CommandContext(ctx, commandName, cmdArgs...)
			stdoutPipe, err := cmd.StdoutPipe()
			if err != nil {
				addToLog(fmt.Sprintf("Error creating stdout pipe: %v", err))
				setCurrentCommandUI("Idle")
				continue
			}
			stderrPipe, err := cmd.StderrPipe()
			if err != nil {
				addToLog(fmt.Sprintf("Error creating stderr pipe: %v", err))
				setCurrentCommandUI("Idle")
				continue
			}

			if err := cmd.Start(); err != nil {
				addToLog(fmt.Sprintf("Error starting command: %v", err))
				setCurrentCommandUI("Idle")
				continue
			}

			var wg sync.WaitGroup
			wg.Add(2)

			// Goroutine for stdout
			go func() {
				defer wg.Done()
				reader := bufio.NewReader(stdoutPipe)
				for {
					line, err := reader.ReadString('\n')
					if len(line) > 0 {
						addToLog(strings.TrimRight(line, "\r\n"))
					}
					if err != nil {
						if err != io.EOF {
							addToLog(fmt.Sprintf("Stdout read error: %v", err))
						}
						break // Exit loop on EOF or other error
					}
				}
			}()

			// Goroutine for stderr
			go func() {
				defer wg.Done()
				reader := bufio.NewReader(stderrPipe)
				for {
					line, err := reader.ReadString('\n')
					if len(line) > 0 {
						addToLog(strings.TrimRight(line, "\r\n")) // Log stderr lines
					}
					if err != nil {
						if err != io.EOF {
							addToLog(fmt.Sprintf("Stderr read error: %v", err))
						}
						break // Exit loop on EOF or other error
					}
				}
			}()

			wg.Wait() // Wait for both stdout and stderr to be fully processed
			err = cmd.Wait()
			if err != nil {
				if exitErr, ok := err.(*exec.ExitError); ok {
					addToLog(fmt.Sprintf("Process finished. Exit code: %d", exitErr.ExitCode()))
				} else {
					addToLog(fmt.Sprintf("Command failed: %v", err))
				}
			} else {
				addToLog("Process finished. Exit code: 0")
			}
			setCurrentCommandUI("Idle")
		}
	}
}

func wndProc(hwnd syscall.Handle, msg uint32, wParam, lParam uintptr) uintptr {
	currentY := int32(10)
	labelWidth := int32(580) // For full-width labels
	editWidth := int32(560)  // For full-width edits (window_width - 2*margin - borders)
	inputGroupY := int32(0)  // Will be set later

	switch msg {
	case WM_CREATE:
		lf := LOGFONTW{
			Height:  -12,
			Weight:  400, // FW_NORMAL
			CharSet: 1,   // DEFAULT_CHARSET
		}
		faceName := strToLfFaceName("Segoe UI")
		copy(lf.FaceName[:], faceName[:])
		var err error
		hFont, err = CreateFontIndirect(&lf)
		if err != nil {
			log.Printf("Failed to create font: %v", err)
		}

		// Command Prefix Label
		hwndPrefixLabel, _ = CreateWindowEx(0, syscall.StringToUTF16Ptr("STATIC"), syscall.StringToUTF16Ptr("Command Prefix:"),
			WS_CHILD|WS_VISIBLE|SS_LEFT,
			10, currentY, labelWidth, 20, hwnd, syscall.Handle(IDC_STATIC_PREFIX_LABEL), hInstance, nil)
		SendMessage(hwndPrefixLabel, WM_SETFONT, uintptr(hFont), 1)
		currentY += 20 + 5 // label height + gap

		// Command Prefix Edit
		hwndPrefixEdit, _ = CreateWindowEx(WS_EX_CLIENTEDGE, syscall.StringToUTF16Ptr("EDIT"), syscall.StringToUTF16Ptr(cmdPrefixFlagValue), // Use initial flag value
			WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,
			10, currentY, editWidth, 25, hwnd, syscall.Handle(IDC_EDIT_PREFIX), hInstance, nil)
		SendMessage(hwndPrefixEdit, WM_SETFONT, uintptr(hFont), 1)
		currentY += 25 + 10 // edit height + gap

		// Dashboard Label
		hwndDashboardLabel, _ = CreateWindowEx(0, syscall.StringToUTF16Ptr("STATIC"), syscall.StringToUTF16Ptr("Dashboard:"),
			WS_CHILD|WS_VISIBLE|SS_LEFT,
			10, currentY, labelWidth, 20, hwnd, syscall.Handle(IDC_STATIC_DASHBOARD_LABEL), hInstance, nil)
		SendMessage(hwndDashboardLabel, WM_SETFONT, uintptr(hFont), 1)
		currentY += 20 + 5

		// Dashboard Display
		hwndDashboard, _ = CreateWindowEx(WS_EX_CLIENTEDGE, syscall.StringToUTF16Ptr("STATIC"), syscall.StringToUTF16Ptr(""),
			WS_CHILD|WS_VISIBLE|SS_LEFT,
			10, currentY, editWidth, 80, hwnd, syscall.Handle(IDC_STATIC_DASHBOARD), hInstance, nil)
		SendMessage(hwndDashboard, WM_SETFONT, uintptr(hFont), 1)
		currentY += 80 + 10

		// Log Label
		hwndLogLabel, _ = CreateWindowEx(0, syscall.StringToUTF16Ptr("STATIC"), syscall.StringToUTF16Ptr("Log Output:"),
			WS_CHILD|WS_VISIBLE|SS_LEFT,
			10, currentY, labelWidth, 20, hwnd, syscall.Handle(IDC_STATIC_LOG_LABEL), hInstance, nil)
		SendMessage(hwndLogLabel, WM_SETFONT, uintptr(hFont), 1)
		currentY += 20 + 5

		// Log Edit Control
		hwndLog, _ = CreateWindowEx(WS_EX_CLIENTEDGE, syscall.StringToUTF16Ptr("EDIT"), syscall.StringToUTF16Ptr(""),
			WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_HSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_AUTOHSCROLL|ES_READONLY,
			10, currentY, editWidth, 200, hwnd, syscall.Handle(IDC_EDIT_LOG), hInstance, nil)
		SendMessage(hwndLog, WM_SETFONT, uintptr(hFont), 1)
		currentY += 200 + 10

		inputGroupY = currentY // Y position for the Suffix input group

		// Suffix Input Label
		hwndInputLabel, _ = CreateWindowEx(0, syscall.StringToUTF16Ptr("STATIC"), syscall.StringToUTF16Ptr("Cmd Suffix:"),
			WS_CHILD|WS_VISIBLE|SS_LEFT,
			10, inputGroupY, 80, 20, hwnd, syscall.Handle(IDC_STATIC_INPUT_LABEL), hInstance, nil)
		SendMessage(hwndInputLabel, WM_SETFONT, uintptr(hFont), 1)

		// Suffix Input Edit Control
		hwndInput, _ = CreateWindowEx(WS_EX_CLIENTEDGE, syscall.StringToUTF16Ptr("EDIT"), syscall.StringToUTF16Ptr(""),
			WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,
			100, inputGroupY, 370, 25, hwnd, syscall.Handle(IDC_EDIT_INPUT), hInstance, nil)
		SendMessage(hwndInput, WM_SETFONT, uintptr(hFont), 1)

		// Add Button
		hwndButtonAdd, _ = CreateWindowEx(0, syscall.StringToUTF16Ptr("BUTTON"), syscall.StringToUTF16Ptr("Add to Queue"),
			WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
			480, inputGroupY, 100, 25, hwnd, syscall.Handle(IDC_BUTTON_ADD), hInstance, nil)
		SendMessage(hwndButtonAdd, WM_SETFONT, uintptr(hFont), 1)

		updateLogUI()
		updateDashboardUI()
		SetFocus(hwndInput) // Set focus to suffix input field initially

	case WM_COMMAND:
		cmdID := LOWORD(uint32(wParam))
		// notifyCode := HIWORD(uint32(wParam))

		if cmdID == IDC_BUTTON_ADD {
			// Get Prefix
			prefixLen := GetWindowTextLength(hwndPrefixEdit)
			var prefixStr string
			if prefixLen > 0 {
				buffer := make([]uint16, prefixLen+1)
				GetWindowText(hwndPrefixEdit, &buffer[0], prefixLen+1)
				prefixStr = syscall.UTF16ToString(buffer)
				prefixStr = strings.TrimSpace(prefixStr)
			}
			if prefixStr == "" {
				addToLog("Error: Command prefix cannot be empty.")
				SetFocus(hwndPrefixEdit)
				return 0 // Don't proceed
			}

			// Get Suffix
			suffixLen := GetWindowTextLength(hwndInput)
			var suffixStr string
			if suffixLen > 0 {
				buffer := make([]uint16, suffixLen+1)
				GetWindowText(hwndInput, &buffer[0], suffixLen+1)
				suffixStr = syscall.UTF16ToString(buffer)
				suffixStr = strings.TrimSpace(suffixStr)
			}
			// Suffix can be empty, depending on the command.

			task := QueuedTask{Prefix: prefixStr, Suffix: suffixStr}
			taskQueue <- task
			addToLog(fmt.Sprintf("Added to queue: [%s] %s", prefixStr, suffixStr))
			SetWindowText(hwndInput, "") // Clear suffix input
			PostMessage(hwndMain, WM_APP_UPDATE_DASHBOARD, 0, 0)
			SetFocus(hwndInput) // Return focus to suffix input
		}

	case WM_SIZE:
		// Basic resize handling could be added here if desired.
		// For now, using fixed layout.
		// InvalidateRect(hwnd, nil, true) // If manual repaint needed after children are moved

	case WM_APP_ADD_LOG:
		updateLogUI()

	case WM_APP_UPDATE_DASHBOARD:
		updateDashboardUI()

	case WM_APP_SET_CURRENT_CMD:
		// This message signals that currentCommand global has changed.
		// updateDashboardUI() is usually called after this, which reads and displays it.
		// If there was a dedicated static text for "Current Command", it would be updated here.
		updateDashboardUI() // Ensure dashboard reflects any change potentially missed

	case WM_CLOSE:
		if DestroyWindow(hwnd) {
			hwndMain = 0
		}

	case WM_DESTROY:
		if hFont != 0 {
			DeleteObject(hFont)
			hFont = 0
		}
		cancelApp()
		PostQuitMessage(0)

	default:
		return DefWindowProc(hwnd, msg, wParam, lParam)
	}
	return 0
}

func LOWORD(dwValue uint32) uint16 {
	return uint16(dwValue & 0xFFFF)
}

// func HIWORD(dwValue uint32) uint16 { // Not currently used
// 	return uint16((dwValue >> 16) & 0xFFFF)
// }

func main() {
	flag.StringVar(&cmdPrefixFlagValue, "cmd", defaultCmdPrefixFlag, "Initial command prefix for the GUI input field.")
	flag.Parse()
	if len(flag.Args()) > 0 {
		log.Fatalf("Error: Positional arguments are not supported when GUI is active.")
	}

	var err error
	hInstance, err = GetModuleHandle()
	if err != nil {
		log.Fatalf("Failed to get module handle: %v", err)
	}

	cursor, err := LoadCursor(0, IDC_ARROW)
	if err != nil {
		log.Fatalf("Failed to load cursor: %v", err)
	}

	wc := WNDCLASSEXW{
		Style:      0, // CS_HREDRAW | CS_VREDRAW for redraw on resize
		WndProc:    syscall.NewCallback(wndProc),
		Instance:   hInstance,
		Cursor:     cursor,
		Background: syscall.Handle(COLOR_BTNFACE + 1),
		ClassName:  syscall.StringToUTF16Ptr(windowClassName),
	}
	wc.Size = uint32(unsafe.Sizeof(wc))

	if _, err := RegisterClassEx(&wc); err != nil {
		log.Fatalf("Failed to register window class: %v", err)
	}

	addToLog("Application starting...")
	addToLog(fmt.Sprintf("Initial command prefix set to: %s (editable in GUI)", cmdPrefixFlagValue))
	addToLog("Enter command suffix and click 'Add to Queue'.")
	addToLog("Close window or press Alt+F4 to quit.")

	screenWidth := GetSystemMetrics(SM_CXSCREEN)
	screenHeight := GetSystemMetrics(SM_CYSCREEN)
	windowWidth := int32(600)
	windowHeight := int32(500) // Increased height for the new prefix field and layout
	windowX := (screenWidth - windowWidth) / 2
	windowY := (screenHeight - windowHeight) / 2

	hwndMain, err = CreateWindowEx(
		0, // No WS_EX_CLIENTEDGE on main window frame itself unless desired
		syscall.StringToUTF16Ptr(windowClassName),
		syscall.StringToUTF16Ptr("Cmd Queue GUI"),
		WS_OVERLAPPEDWINDOW|WS_VISIBLE,
		windowX, windowY, windowWidth, windowHeight,
		0, 0, hInstance, nil,
	)
	if err != nil {
		log.Fatalf("Failed to create window: %v", err)
	}

	ShowWindow(hwndMain, SW_SHOWNORMAL)
	UpdateWindow(hwndMain)

	appCtx, cancelApp = context.WithCancel(context.Background())
	defer cancelApp()

	go commandProcessor(appCtx)

	var msg MSG
	for {
		ret, getMsgErr := GetMessage(&msg, 0, 0, 0) // Use getMsgErr to check error
		if ret == 0 { // WM_QUIT
			break
		}
		if ret == -1 { // Error
			log.Printf("Error in GetMessage: %v (return code: %d)", getMsgErr, ret)
			break
		}

		// Standard message loop for non-dialog windows
		// If you had modeless dialogs or needed IsDialogMessage for ACCELERATORS, it would go here.
		// For simple child edit controls, Translate/Dispatch is usually enough.
		// if !IsDialogMessage(hwndMain, &msg) { // Only if hwndMain was a dialog
		TranslateMessage(&msg)
		DispatchMessage(&msg)
		// }
	}

	// Allow time for goroutines to clean up after WM_QUIT
	// commandProcessor will exit due to cancelApp() called in WM_DESTROY
	// Wait for it by other means if necessary (e.g. a done channel from commandProcessor)
	// For now, a small delay.
	time.Sleep(250 * time.Millisecond)
	fmt.Println("Exited cleanly.")
}

const COLOR_BTNFACE = 15 // System color for button face, often used for dialog backgrounds
