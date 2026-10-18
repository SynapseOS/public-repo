# Зависимости выходных файлов
$(BUILD_ROOT)/curl/.built: $(BUILD_ROOT)/openssl/.built
$(BUILD_ROOT)/wget/.built: $(BUILD_ROOT)/openssl/.built
$(BUILD_ROOT)/python/.built: $(BUILD_ROOT)/openssl/.built $(BUILD_ROOT)/zlib/.built $(BUILD_ROOT)/sqlite/.built
$(BUILD_ROOT)/git/.built: $(BUILD_ROOT)/curl/.built $(BUILD_ROOT)/openssl/.built $(BUILD_ROOT)/zlib/.built
$(BUILD_ROOT)/libstdcxx/.built: $(SDK)/.ready
$(BUILD_ROOT)/binutils/.built: $(BUILD_ROOT)/libstdcxx/.built
$(BUILD_ROOT)/gcc/.built: $(BUILD_ROOT)/binutils/.built $(BUILD_ROOT)/libstdcxx/.built $(BUILD_ROOT)/m4/.built
$(BUILD_ROOT)/posix-bench/.built: $(SDK)/.ready
$(BUILD_ROOT)/open-posix-testsuite/.built: $(SDK)/.ready
$(BUILD_ROOT)/mc/.built: $(BUILD_ROOT)/glib/.built $(BUILD_ROOT)/pcre2/.built $(SDK)/.ncurses-ready
$(BUILD_ROOT)/glib/.built: $(BUILD_ROOT)/pcre2/.built
$(BUILD_ROOT)/less/.built: $(SDK)/.ncurses-ready
$(BUILD_ROOT)/screen/.built: $(SDK)/.ncurses-ready
$(BUILD_ROOT)/links/.built: $(SDK)/.mbedtls-ready
$(BUILD_ROOT)/setup/.built: $(SDK)/.mbedtls-ready
$(BUILD_ROOT)/tcpdump/.built: $(BUILD_ROOT)/libpcap/.built
$(BUILD_ROOT)/nmap/.built: $(BUILD_ROOT)/libpcap/.built
$(BUILD_ROOT)/nmap/.built: $(BUILD_ROOT)/libstdcxx/.built $(BUILD_ROOT)/zlib/.built $(BUILD_ROOT)/openssl/.built
$(BUILD_ROOT)/mindustry/.built: $(BUILD_ROOT)/openjdk-current/.built
$(BUILD_ROOT)/openjdk-current/.built: $(BUILD_ROOT)/openjdk/.built
$(BUILD_ROOT)/mesa/.built: $(BUILD_ROOT)/openjdk/.built
$(BUILD_ROOT)/opengl/.built: $(BUILD_ROOT)/mesa/.built
$(BUILD_ROOT)/weston/.built: $(BUILD_ROOT)/mesa/.built
$(BUILD_ROOT)/wayland-client/.built: $(BUILD_ROOT)/weston/.built
$(BUILD_ROOT)/xkeyboard-config/.built: $(BUILD_ROOT)/weston/.built
$(BUILD_ROOT)/sdl2/.built: $(BUILD_ROOT)/mesa/.built $(BUILD_ROOT)/wayland-client/.built
$(BUILD_ROOT)/mindustry-desktop/.built: $(BUILD_ROOT)/mindustry/.built $(BUILD_ROOT)/opengl/.built $(BUILD_ROOT)/sdl2/.built
$(BUILD_ROOT)/netsurf/.built: $(BUILD_ROOT)/sdl2/.built $(BUILD_ROOT)/curl/.built $(BUILD_ROOT)/openssl/.built $(BUILD_ROOT)/weston/.built
$(BUILD_ROOT)/synbench/.built: $(BUILD_ROOT)/synlibc/.built
$(BUILD_ROOT)/nmap-nse-a/.built: $(BUILD_ROOT)/nmap/.built
$(BUILD_ROOT)/nmap-nse-b/.built: $(BUILD_ROOT)/nmap/.built
$(BUILD_ROOT)/nmap-nselib/.built: $(BUILD_ROOT)/nmap/.built
$(BUILD_ROOT)/open-posix-testsuite-data-1/.built: $(BUILD_ROOT)/open-posix-testsuite/.built
$(BUILD_ROOT)/open-posix-testsuite-data-2/.built: $(BUILD_ROOT)/open-posix-testsuite/.built
$(BUILD_ROOT)/open-posix-testsuite-data-3/.built: $(BUILD_ROOT)/open-posix-testsuite/.built
$(BUILD_ROOT)/open-posix-testsuite-data-4/.built: $(BUILD_ROOT)/open-posix-testsuite/.built

.PHONY: ncurses mbedtls
ncurses: $(SDK)/.ncurses-ready
mbedtls: $(SDK)/.mbedtls-ready
$(SDK)/.ncurses-ready: $(SDK)/.ready
	+$(MAKE) -C "$(SDK)/apps/apps/common/ncurses"
	@touch "$@"
$(SDK)/.mbedtls-ready: $(SDK)/.ready
	+$(MAKE) -C "$(SDK)/apps/apps/common/mbedtls"
	@touch "$@"
$(BUILD_ROOT)/sdl2/.built: $(ROOT)/pkgs/mindustry-desktop/SDL_synapsevideo.c
$(BUILD_ROOT)/qt6-llvm-runtime/.built: $(ROOT)/pkgs/qt6-base/runtime.sh
$(BUILD_ROOT)/qt6-base/.built: $(BUILD_ROOT)/weston/.built $(BUILD_ROOT)/qt6-llvm-runtime/.built $(ROOT)/build/cxx-shared.sh
$(BUILD_ROOT)/qt6-wayland/.built: $(BUILD_ROOT)/qt6-base/.built
$(BUILD_ROOT)/ayugram/.built: $(BUILD_ROOT)/qt6-wayland/.built $(ROOT)/pkgs/qt6-base/launch.c $(ROOT)/pkgs/qt6-base/runtime.sh
$(BUILD_ROOT)/ayugram/.built: $(wildcard $(or $(TELEGRAM_API_FILE),$(HOME)/.config/synapseos/telegram-api.env))
