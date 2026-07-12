#!/usr/bin/env bash
# Import Beckhoff-SSC-generated sources into ecat/core0/SSC/Src.
#
# The SSC Tool project is the STOCK SDK ecat_io configuration (see
# ../README.md): every project-specific adaptation is applied here from
# version-controlled files, so regenerating with the SSC Tool never requires
# editing the tool project. Steps:
#   1. copy the generated Src/ (stack + headers; the digital_io.c application
#      template is not used -- core0/src/ecat_appl.c replaces it),
#   2. apply the SDK's mandatory ssc_pdi_mask.patch (Sync0/Sync1 must not
#      raise PDI interrupts; they have dedicated IRQ lines on HPM parts),
#   3. install the stream-bridge object dictionary override,
#   4. rewrite the SII image (eeprom.h) to match the 48-byte stream PDOs.
#
# Usage: import_ssc.sh [GENERATED_SSC_SRC_DIR]
#        default: ~/Downloads/ecat_io/SSC/Src

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ecat_dir="$(cd "${script_dir}/.." && pwd)"

generated_src="${1:-${HOME}/Downloads/ecat_io/SSC/Src}"
dest="${ecat_dir}/core0/SSC/Src"
overrides="${ecat_dir}/core0/ssc_overrides"
sdk_base="${HPM_SDK_BASE:-${ecat_dir}/../bsp/hpm_sdk}"
pdi_patch="${sdk_base}/samples/ethercat/ecat_io/SSC/ssc_pdi_mask.patch"

[ -f "${generated_src}/ecatslv.c" ] || {
    echo "error: ${generated_src} does not look like SSC Tool output (no ecatslv.c)" >&2
    exit 1
}
[ -f "${pdi_patch}" ] || {
    echo "error: SDK patch not found: ${pdi_patch} (set HPM_SDK_BASE?)" >&2
    exit 1
}

rm -rf "${dest}"
mkdir -p "${dest}"

# The SSC Tool numbers files from repeated generations (foo-2.c); take only
# the base set. digital_io.c is the generated application template, replaced
# by core0/src/ecat_appl.c. The tool emits CRLF line endings; normalize to LF
# so the SDK patch below applies and repo tooling stays happy.
for f in "${generated_src}"/*.c "${generated_src}"/*.h; do
    base="$(basename "${f}")"
    case "${base}" in
    *-[0-9].c | *-[0-9].h | digital_io.c) continue ;;
    esac
    sed -e 's/\r$//' "${f}" >"${dest}/${base}"
done

echo "== applying ${pdi_patch}"
patch --directory="${dest}" --forward --strip=1 <"${pdi_patch}"
grep -q "independent Sync0" "${dest}/ecatslv.c" || {
    echo "error: pdi mask patch did not apply" >&2
    exit 1
}

echo "== installing object dictionary override"
cp "${overrides}/digital_ioObjects.h" "${dest}/digital_ioObjects.h"

echo "== rewriting SII image"
python3 "${script_dir}/patch_sii.py" "${dest}/eeprom.h" "${dest}/eeprom.h"

echo "== done: $(ls "${dest}" | wc -l) files in ${dest}"
