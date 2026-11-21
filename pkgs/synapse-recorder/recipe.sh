# SynapseOS, пакет записи экрана и камер, 21.11.2026
prepare
fetch "$(jq -er .h264_source "$spec")" "$(jq -er .h264_sha256 "$spec")" "$work/minih264e.h"
(cd "$source" && bash "$port/build-media.sh")
source "$port/link-recorder.sh"
copy "$source/COPYING.LGPLv2.1" usr/share/licenses/synapse-recorder/FFmpeg-LGPL-2.1
copy "$port/README.md" usr/share/help/synapse-recorder.md

copy "$port/minih264-NOTICE" usr/share/licenses/synapse-recorder/minih264-NOTICE
copy "$port/zlib-LICENSE" usr/share/licenses/synapse-recorder/zlib-LICENSE
