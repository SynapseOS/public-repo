# Правила для путей, принадлежащих штатному sudo
def sudo_path:
    test("^((usr/)?bin/(sudo|sudoedit|visudo)|etc/(sudo\\.conf|sudoers|sudoers\\.d)|(lib|usr/lib|usr/libexec)/sudo)(/|$)");
def package_permissions($owner):
    if (.path | sudo_path) then
        $owner == "sudo" and
        (if .path == "bin/sudo" then .type == "file" and .mode == 2541 and .config == false
         elif .path == "etc/sudoers" then .type == "file" and .mode == 288 and .config == true
         else .mode <= 511 and ((.mode / 16 | floor) % 2 == 0) and ((.mode / 2 | floor) % 2 == 0)
         end)
    else .mode <= 511 end;

# Маркер именуется хешем штатного бинарника и не содержит данных
def check_privileged_elf:
    "usr/share/synapse/privileged-elf" as $dir |
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" as $empty |
    [.files[] | select(.path | startswith($dir + "/"))] as $markers |
    all(.files[]; .path != "usr/share/synapse" and .path != $dir) and
    if .name == "sudo" then
        [.files[] | select(.path == "bin/sudo")] as $bin |
        ($bin | length == 1) and ($markers | length == 1) and
        ($markers[0] | .path == ($dir + "/" + $bin[0].sha256) and .type == "file" and
                .mode == 420 and .config == false and .size == 0 and .sha256 == $empty)
    else ($markers | length == 0) end;
