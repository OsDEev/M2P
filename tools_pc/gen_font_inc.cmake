# Generates blank font-blob initializers for the PC build.
#
# The retail build extracts these bitmaps from the original DOL at decomp
# configure time (config/GALE01/config.yml `extract:`). The PC build does
# not have the DOL, so blank (zeroed) glyphs are generated instead:
#   - sysdolphin/baselib/sislib_font.inc  (287 x 512-byte menu/UI glyphs)
#   - sysdolphin/baselib/debug_font.inc   (128 x 56-byte console glyphs)
#
# Usage: cmake -DOUT_DIR=<dir> -P tools_pc/gen_font_inc.cmake
# Real extracted blobs (src/sysdolphin/baselib/*.inc) take precedence via
# include order; these are only a fallback.
if(NOT OUT_DIR)
    message(FATAL_ERROR "OUT_DIR not set")
endif()

function(gen_blank_inc relpath entries)
    set(path "${OUT_DIR}/${relpath}")
    get_filename_component(dir "${path}" PATH)
    file(MAKE_DIRECTORY "${dir}")
    if(EXISTS "${path}")
        message(STATUS "[melee] keeping existing ${relpath}")
        return()
    endif()
    set(text "// Auto-generated blank font blob (PC port fallback).\n")
    foreach(i RANGE 1 ${entries})
        string(APPEND text "{0},\n")
    endforeach()
    file(WRITE "${path}" "${text}")
    message(STATUS "[melee] generated ${path}")
endfunction()

gen_blank_inc("sysdolphin/baselib/sislib_font.inc" 287)
gen_blank_inc("sysdolphin/baselib/debug_font.inc" 128)
