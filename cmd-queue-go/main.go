package main

import (
	"bufio"
	"context"
	"flag"
	"fmt"
	"io"
	"log"
	"os/exec"
	"strings"
	"sync"
	"time"

	"github.com/gdamore/tcell/v2"
	"github.com/rivo/tview"
)

const (
	maxLogLines      = 200
	defaultCmdPrefix = "yt-dlp -f 233"
)

var (
	cmdPrefix          string
	taskQueue          = make(chan string, 100)
	logLines           []string
	logLock            sync.Mutex
	currentCommand     string = "Idle" // Initial value, will be set in UI via updateDashboard
	currentCommandLock sync.Mutex
	app                *tview.Application
	dashboardView      *tview.TextView
	logView            *tview.TextView
	inputField         *tview.InputField
	// appReady ensures addToLog and setCurrentCommand only queue updates after app.Run() starts
	appReady     = make(chan struct{})
	appReadyOnce sync.Once
)

// addToLog safely queues UI updates only when the app is ready
func addToLog(line string) {
	logLock.Lock()
	logLines = append(logLines, line)
	if len(logLines) > maxLogLines {
		logLines = logLines[len(logLines)-maxLogLines:]
	}
	logLock.Unlock() // Unlock before potentially blocking on appReady or QueueUpdateDraw

	select {
	case <-appReady: // Only proceed if app is ready
		if app != nil && logView != nil {
			app.QueueUpdateDraw(func() {
				logView.Clear()
				logLock.Lock() // Lock again for reading logLines
				fmt.Fprint(logView, strings.Join(logLines, "\n"))
				logLock.Unlock()
				logView.ScrollToEnd()
			})
		}
	default:
		// App not ready yet, log will be picked up by initial draw or first update
	}
}

// setCurrentCommand safely queues UI updates only when the app is ready
func setCurrentCommand(cmdStr string) {
	currentCommandLock.Lock()
	currentCommand = cmdStr
	currentCommandLock.Unlock()

	select {
	case <-appReady: // Only proceed if app is ready
		if app != nil {
			app.QueueUpdateDraw(updateDashboard)
		}
	default:
		// App not ready, dashboard will pick up currentCommand on its first draw
	}
}

func getCurrentCommand() string {
	currentCommandLock.Lock()
	defer currentCommandLock.Unlock()
	return currentCommand
}

func updateDashboard() {
	if dashboardView == nil || app == nil {
		return
	}
	dashboardView.Clear()
	queueLen := len(taskQueue) // Note: len of channel is an approximation

	fmt.Fprintf(dashboardView, "[yellow]Current CMD:[white]\n%s\n\n", getCurrentCommand())
	fmt.Fprintf(dashboardView, "[yellow]Queue (%d pending):[white]\n", queueLen)
	if queueLen > 0 {
		// Attempt to peek at the next item if possible (conceptually)
		// For a real peek, you'd need a different queue structure or non-blocking read
		fmt.Fprint(dashboardView, "(Items in queue, next will be processed)")
	} else {
		fmt.Fprint(dashboardView, "[Empty]")
	}
}

func commandProcessor(ctx context.Context) {
	<-appReady // Wait for the application to signal it's ready before starting processing
	for {
		select {
		case <-ctx.Done():
			addToLog("[cyan]Command processor shutting down...[/]")
			return
		case suffix := <-taskQueue:
			if suffix == "" {
				setCurrentCommand("Idle")
				continue
			}
			fullCmdStr := cmdPrefix + " " + suffix
			setCurrentCommand(fullCmdStr)
			addToLog(fmt.Sprintf("[sky_blue]$ %s[/]", fullCmdStr))

			var cmdArgs []string
			prefixParts := strings.Fields(cmdPrefix)
			if len(prefixParts) == 0 {
				addToLog(fmt.Sprintf("[red]Error: Command prefix '%s' is empty or invalid.[/]", cmdPrefix))
				setCurrentCommand("Idle")
				continue
			}
			commandName := prefixParts[0]
			if len(prefixParts) > 1 {
				cmdArgs = append(cmdArgs, prefixParts[1:]...)
			}
			suffixParts := strings.Fields(suffix) // This handles spaces in suffix correctly
			cmdArgs = append(cmdArgs, suffixParts...)

			cmd := exec.CommandContext(ctx, commandName, cmdArgs...)
			stdoutPipe, err := cmd.StdoutPipe()
			if err != nil {
				addToLog(fmt.Sprintf("[red]Error creating stdout pipe: %v[/]", err))
				setCurrentCommand("Idle")
				continue
			}
			stderrPipe, err := cmd.StderrPipe()
			if err != nil {
				addToLog(fmt.Sprintf("[red]Error creating stderr pipe: %v[/]", err))
				setCurrentCommand("Idle")
				continue
			}

			if err := cmd.Start(); err != nil {
				addToLog(fmt.Sprintf("[red]Error starting command: %v[/]", err))
				setCurrentCommand("Idle")
				continue
			}

			var wg sync.WaitGroup
			wg.Add(2)
			go func() {
				defer wg.Done()
				scanner := bufio.NewScanner(stdoutPipe)
				for scanner.Scan() {
					addToLog(scanner.Text())
				}
				if err := scanner.Err(); err != nil && err != io.EOF {
					addToLog(fmt.Sprintf("[orange]Stdout read error: %v[/]", err))
				}
			}()
			go func() {
				defer wg.Done()
				scanner := bufio.NewScanner(stderrPipe)
				for scanner.Scan() {
					addToLog(fmt.Sprintf("[dark_orange]%s[/]", scanner.Text()))
				}
				if err := scanner.Err(); err != nil && err != io.EOF {
					addToLog(fmt.Sprintf("[orange]Stderr read error: %v[/]", err))
				}
			}()
			wg.Wait()
			err = cmd.Wait()
			if err != nil {
				if exitErr, ok := err.(*exec.ExitError); ok {
					addToLog(fmt.Sprintf("[yellow]Process finished. Exit code: %d[/]", exitErr.ExitCode()))
				} else {
					addToLog(fmt.Sprintf("[red]Command failed: %v[/]", err))
				}
			} else {
				addToLog("[yellow]Process finished. Exit code: 0[/]")
			}
			setCurrentCommand("Idle")
		}
	}
}

func main() {
	flag.StringVar(&cmdPrefix, "cmd", defaultCmdPrefix, "Command prefix")
	flag.Parse()
	if len(flag.Args()) > 0 {
		log.Fatalf("Error: Positional arguments not supported...")
	}

	app = tview.NewApplication()
	app.EnablePaste(true) // <--- ADD THIS LINE TO ENABLE PASTE SUPPORT

	logLock.Lock()
	logLines = append(logLines, "[green]Application starting...[/]")
	logLines = append(logLines, fmt.Sprintf("[cyan]Using command prefix: %s[/]", cmdPrefix))
	logLines = append(logLines, "[cyan]Press Esc or Ctrl-C in input field (or Ctrl-C globally) to quit.[/]")
	logLock.Unlock()

	dashboardView = tview.NewTextView().
		SetDynamicColors(true).
		SetScrollable(true)
	dashboardView.SetBorder(true).SetTitle(" Dashboard ")

	logView = tview.NewTextView().
		SetDynamicColors(true).
		SetScrollable(true).
		SetMaxLines(maxLogLines * 2) // Sufficiently large
	logView.SetBorder(true).SetTitle(" Log ")

	inputField = tview.NewInputField().
		SetLabel("Cmd Suffix > ").
		SetFieldWidth(0)

	inputField.SetDoneFunc(func(key tcell.Key) {
		if key == tcell.KeyEnter {
			suffix := strings.TrimSpace(inputField.GetText())
			if suffix != "" {
				taskQueue <- suffix
				addToLog(fmt.Sprintf("[green]Added to queue: %s[/]", suffix))
				inputField.SetText("")
				if app != nil {
					app.QueueUpdateDraw(updateDashboard)
				}
			}
		} else if key == tcell.KeyEscape || key == tcell.KeyCtrlC { // Ctrl-C in input field
			app.Stop()
		}
	})
	inputField.SetBorder(true).SetTitle(" Input ")

	mainFlex := tview.NewFlex().SetDirection(tview.FlexRow).
		AddItem(tview.NewFlex().SetDirection(tview.FlexColumn).
			AddItem(dashboardView, 0, 1, false).
			AddItem(logView, 0, 2, false),
			0, 1, false).
		AddItem(inputField, 3, 0, true)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel() // This will be called when main exits

	go commandProcessor(ctx)

	app.SetBeforeDrawFunc(func(screen tcell.Screen) bool {
		appReadyOnce.Do(func() {
			close(appReady)
		})
		// Initial population of views
		logView.Clear()
		logLock.Lock()
		fmt.Fprint(logView, strings.Join(logLines, "\n"))
		logLock.Unlock()
		logView.ScrollToEnd()
		updateDashboard()
		return false
	})

	// Global input capture for Ctrl-C to quit application
	app.SetInputCapture(func(event *tcell.EventKey) *tcell.EventKey {
		if event.Key() == tcell.KeyCtrlC {
			app.Stop()
			return nil // Event handled
		}
		return event // Pass through other events
	})


	if err := app.SetRoot(mainFlex, true).EnableMouse(true).Run(); err != nil {
		log.Fatalf("Error running application: %v", err)
	}

	// cancel() // Already deferred, this would be redundant or potentially problematic if called too early
	// No need to explicitly call cancel() here again as defer will handle it.
	// Give commandProcessor a moment to notice context cancellation and log its shutdown.
	// This is a bit of a race, ideally, commandProcessor would signal its completion.
	time.Sleep(200 * time.Millisecond)
	fmt.Println("Exited cleanly.")
}
