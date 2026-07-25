# Merge two Intel HEX images into one flashable file.
#
# CH32H417 is dual-core with each core in its own flash partition (V3F @ 0x0,
# V5F @ 0x10000). There is no "embed one core into the other" model on this part
# (V3F is the boot core and comes out of reset first); the only single-file form
# is two independent images concatenated by address -- exactly what MounRiver's
# Merge.bin does. Each input .hex already carries absolute addresses (objcopy
# emits Extended Linear Address records), so merging is a text concatenation:
# drop the boot image's End-Of-File record, then append the app image verbatim.
#
# A third fragment carries the bootloader's app-image metadata record (see
# cmake/gen_metadata_hex.py). Without it the V3F bootloader's app_image_is_valid()
# gate fails on every cold boot and V5F is never woken, so a merged image that
# omitted it would flash cleanly and then never run the app.
#
# Inputs (passed via -D): BOOT (V3F .hex), APP (V5F .hex), META (metadata record
# .hex), OUT (merged .hex).

file(READ "${BOOT}" boot_txt)
file(READ "${APP}" app_txt)
file(READ "${META}" meta_txt)

# Strip the trailing EOF record (":00000001FF") from every fragment but the last,
# so the streams join into a single valid HEX file.
string(REGEX REPLACE ":00000001FF[ \t\r\n]*$" "" boot_txt "${boot_txt}")
string(REGEX REPLACE ":00000001FF[ \t\r\n]*$" "" app_txt "${app_txt}")

file(WRITE "${OUT}" "${boot_txt}${app_txt}${meta_txt}")
