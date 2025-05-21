## init

```shell
go mod init cmdq-win32
# go mod tidy
```

## build

```shell
CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -ldflags="-s -w -H windowsgui" -o hello.exe
```

## .h
https://gist.githubusercontent.com/nathan-osman/18c2e227ad00a223b61c0b3c16d452c3/raw/d2da5c08ed82aac945c0053ce60d12e8c2a9d265/win32.go

## TODO

1. refactor in plain C (C 17, makefile, zig cc, x86_64-windows-gnu)
2. Auto-focus on button input area when swicthing to main window
3. Hitting keyboard "Enter" at button input is equalavant to click "add to queue" button
4. Log Output area need to deal with self-updating output lines (e.g. tqdm progress bar, yt-dlp dynamic downloading progress)
5. handle unicode/utf8 correctly
6. no need to popup black window (main process and sub precess)
