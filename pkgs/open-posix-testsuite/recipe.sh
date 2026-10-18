# recipe.sh: Собрать неизменённые проверки POSIX и сохранить результаты компиляции
# Автор: Елчинян Арен Артурович, 2026
# Подсистема: сборка пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный

source "$ROOT/build/toolchain.sh"
fetch "$(jq -r .source "$spec")" "$(jq -r .sha256 "$spec")" "$work/ltp.tar.gz"
source="$work/source"
rm -rf "$source"
mkdir -p "$source"
prefix="ltp-$(jq -r .commit "$spec")/testcases/open_posix_testsuite/"
bsdtar -xf "$work/ltp.tar.gz" -C "$source" --strip-components 3 --include "$prefix*"
sed -e "s|@CC@|$CC|g" -e 's|@CFLAGS@|-O2|g' -e 's|@LIBS@|-pthread -lrt -lm|g' \
	-e 's|@LDFLAGS@|-static|g' -e 's|@prefix@|/usr/libexec/open-posix-testsuite|g' \
	-e 's|@exec_prefix@|${prefix}|g' -e 's|@testdir@||g' \
	"$source/include/mk/config.mk.in" >"$source/include/mk/config.mk"
! grep -Eq '@[A-Za-z_]+@' "$source/include/mk/config.mk"

(
	cd "$source" &&
		sh scripts/generate-makefiles.sh
) >"$work/configure.log" 2>&1

make -C "$source/lib" -j"$JOBS" >"$work/build.log" 2>&1
make -C "$source/conformance" -j"$JOBS" all >>"$work/build.log" 2>&1
suite="$payload/usr/libexec/open-posix-testsuite"
mkdir -p "$suite"
: >"$suite/tests.tsv"
: >"$work/cases.jsonl"

while IFS= read -r -d '' file; do
	directory=$(dirname "$file")
	mapfile -t targets < <(sed -n 's/^MAKE_TARGETS+=[[:space:]]*//p' "$file" | tr ' ' '\n' | sed '/^$/d')

	if ((${#targets[@]})); then
		make -C "$directory" -j"$JOBS" all >>"$work/build.log" 2>&1
	fi

	for target in "${targets[@]}"; do
		binary="$directory/$target"
		kind=RUN
		state=BUILD_FAIL

		case "$target" in
			*.test)
				kind=BUILD
				;;
			*.sh)
				kind=SHELL
				;;
		esac

		base=$(basename "$directory")

		if [[ $base == speculative ]]; then
			base="$(basename "$(dirname "$directory")")_speculative"
		fi

		assertion=${target#"$base"_}
		assertion=${assertion%.run-test}
		assertion=${assertion%.test}
		assertion=${assertion%.sh}
		relative=${binary#"$source/"}
		id="${directory#"$source/"}/$assertion"

		if [[ -f $binary ]]; then
			state=OK

			if [[ $kind != BUILD ]]; then
				copy "$binary" "usr/libexec/open-posix-testsuite/$relative"
			fi
		fi

		printf '%s\t%s\t%s\t%s\n' "$id" "$kind" "$state" "$relative" >>"$suite/tests.tsv"
		checksum=''

		if [[ $state == OK && $kind != BUILD ]]; then
			checksum=$(sha256sum "$binary" | cut -d' ' -f1)
		fi

		jq -cn --arg id "$id" --arg kind "$kind" --arg build "$state" --arg path "$relative" \
			--arg sha "$checksum" '{id:$id,kind:$kind,build:$build,path:$path} +
                (if $sha=="" then {} else {sha256:$sha} end)' >>"$work/cases.jsonl"
	done
done < <(find "$source/conformance" -name Makefile -print0 | sort -z)

while IFS= read -r -d '' file; do
	[[ $(basename "$file") =~ ^[0-9] ]] &&
		continue
	copy "$file" "usr/libexec/open-posix-testsuite/${file#"$source/"}"
done < <(find "$source/conformance" -type f -name '*.sh' -print0)

jq -cs --slurpfile upstream "$spec" '{upstream:$upstream[0],compiler:"Synapse static musl wrapper",cases:.,source_assertions_modified:false,
    timeout_default_seconds:300,resource_limits:{address_space_bytes:536870912,core_bytes:0}}' \
	"$work/cases.jsonl" >"$suite/inventory.json"
copy "$source/COPYING" usr/libexec/open-posix-testsuite/COPYING
copy "$work/build.log" usr/libexec/open-posix-testsuite/build.log
"$CC" -O2 -Wall -Wextra -Werror "$port/runner.c" -o "$payload/bin/open-posix-testsuite"
parent_payload=$payload
mapfile -d '' data < <(find "$suite/conformance" -type f -print0 | sort -z)

for index in 0 1 2 3; do
	child="open-posix-testsuite-data-$((index + 1))"
	payload="$BUILD_ROOT/$child/payload"
	rm -rf "$payload"
	mkdir -p "$payload"

	for file in "${data[@]:index*450:450}"; do
		copy "$file" "${file#"$parent_payload/"}"
	done

	pack "$child" "$payload" "$ROOT/pkgs/$child/manifest.json"
done

[[ ${#data[@]} -le 1800 ]]
payload=$parent_payload
rm -rf "$suite/conformance"
copy "$work/ltp.tar.gz" usr/share/open-posix-testsuite/upstream.tar.gz
copy "$port/runner.c" usr/share/open-posix-testsuite/runner.c
copy "$port/recipe.sh" usr/share/open-posix-testsuite/recipe.sh
