## init

```shell
go mod init
go mod tidy
```

## build

```shell
CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -ldflags="-s -w" -o cmdq.exe
```
