# recipe.sh: Собрать компилятор Go из закреплённых исходников
# Автор: Елчинян Арен Артурович, 2026, x86_64, Linux amd64 ABI
# Подсистема: Go, первое изменение: 22.10.2026, последнее изменение: 22.10.2026
# Статус доступа: общедоступный

prepare
fetch 'https://go.dev/dl/go1.27.3.linux-amd64.tar.gz' \
	'65efbc80fdc1fa2cd2631cad5b481777a68e8725f34e89f50fc6ac714e379b40' "$work/bootstrap.tar.gz"

if [[ ! -x $work/bootstrap/bin/go ]]; then
	mkdir -p "$work/bootstrap-new"
	bsdtar -xf "$work/bootstrap.tar.gz" -C "$work/bootstrap-new" --strip-components=1
	mv "$work/bootstrap-new" "$work/bootstrap"
fi

export GOROOT_BOOTSTRAP="$work/bootstrap" CGO_ENABLED=0 GOOS=linux GOARCH=amd64 GOAMD64=v1
export GOTOOLCHAIN=local GOMAXPROCS="$JOBS" GOFLAGS="-p=$JOBS"
export GOCACHE="$work/cache" GOMODCACHE="$work/modules"
(
	cd "$source/src"
	bash ./make.bash
)

copy "$source/bin/go" usr/lib/go/bin/go
copy "$source/bin/gofmt" bin/gofmt

for tool in asm compile link pack preprofile vet cover; do
	if [[ -f $source/pkg/tool/linux_amd64/$tool ]]; then
		copy "$source/pkg/tool/linux_amd64/$tool" "usr/lib/go/pkg/tool/linux_amd64/$tool"
	fi
done

"$CC" -O2 -Wall -Wextra -Werror "$port/launch.c" -o "$payload/bin/go"
copy "$source/LICENSE" usr/share/licenses/go/LICENSE

copy "$port/probe.go" usr/share/go/examples/probe.go
