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
