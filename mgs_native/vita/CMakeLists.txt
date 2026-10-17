# PS Vita build (README.md §6.29; VitaSDK in WSL, build.bat runs it). Output: build/mgs_mobile.vpk (first install)
# and build/eboot.bin (what tools/vita.py deploy sends for each build after that).
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "Set VITASDK")
    endif()
endif()

project(mgs_mobile C CXX ASM)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "MGS Mobile")
set(VITA_TITLEID "MGSM00001")
set(VITA_VERSION "00.10")   # (0.1)

set(CMAKE_CXX_STANDARD 17)
set(NATIVE ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(TP ${NATIVE}/third_party)

# The shared game code (the same files as the 3DS build: the desktop / Android ones minus SDL's main, audio,
# gamepad and platform) plus this folder's Vita layer. GL ES 1.1 calls go to vitaGL.
set(SHARED loader vfs runtime gles heap settings luachunk story gametools photomode screenshot pngio textures
    custommodel installer autotest keys cpu_native cpu_dynarmic guest_interp touchcontrols movement
    assetviewer modelsandbox overlay imgui_impl_gles1)
set(SOURCES main_vita.cpp platform_vita.cpp gamepad_vita.cpp audio_vita.cpp gl_vita.cpp
    ${TP}/imgui/imgui.cpp ${TP}/imgui/imgui_draw.cpp ${TP}/imgui/imgui_tables.cpp ${TP}/imgui/imgui_widgets.cpp
    ${TP}/miniz/miniz.c ${TP}/lzma/LzmaDec.c ${NATIVE}/native_arm.S)
foreach(f ${SHARED})
    list(APPEND SOURCES ${NATIVE}/${f}.cpp)
endforeach()
set_source_files_properties(${NATIVE}/native_arm.S PROPERTIES LANGUAGE ASM COMPILE_FLAGS "-x assembler-with-cpp")

add_executable(mgs_mobile ${SOURCES})
target_compile_definitions(mgs_mobile PRIVATE MGS_GLES MGS_VITA IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
target_include_directories(mgs_mobile PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${NATIVE} ${TP} ${TP}/imgui ${TP}/miniz
    ${TP}/lzma ${TP}/gles1 ${TP}/SDL3/include)
target_compile_options(mgs_mobile PRIVATE -O2 -g -Wall -Wno-unused-parameter -ffunction-sections
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-psabi>)
target_link_options(mgs_mobile PRIVATE -Wl,--gc-sections -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/sce_gap.ld -Wl,-z,max-page-size=0x10000)
target_link_libraries(mgs_mobile
    vitaGL vitashark SceShaccCgExt mathneon SceShaccCg_stub taihen_stub kubridge_stub
    SceGxm_stub SceDisplay_stub SceKernelDmacMgr_stub SceAppMgr_stub SceCommonDialog_stub
    SceCtrl_stub SceTouch_stub SceAudio_stub ScePower_stub SceNet_stub SceNetCtl_stub SceSysmodule_stub
    SceKernelThreadMgr_stub pthread m)

# No build-machine paths in the release: source paths relative, and the debug information (which names every
# source and SDK header folder, vitaGL's too) stripped before the SELF is made
get_filename_component(MGS_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
target_compile_options(mgs_mobile PRIVATE "-ffile-prefix-map=${MGS_REPO_ROOT}/=")
add_custom_command(TARGET mgs_mobile POST_BUILD COMMAND ${CMAKE_STRIP} --strip-debug $<TARGET_FILE:mgs_mobile> VERBATIM)
vita_create_self(eboot.bin mgs_mobile UNSAFE)
# The VPK, with the bubble and LiveArea images (8-bit palette PNGs made from vita/ by tools/make_vita_images.py).
# By hand rather than vita_create_vpk: that passes its FILE paths to vita-pack-vpk unquoted, and the repo path has spaces.
set(VPK_IMAGES icon0.png pic0.png livearea/contents/bg.png livearea/contents/startup.png livearea/contents/template.xml)
set(VPK_ARGS)
set(VPK_DEPS)
foreach(f ${VPK_IMAGES})
    list(APPEND VPK_ARGS -a "${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/${f}=sce_sys/${f}")
    list(APPEND VPK_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/${f}")
endforeach()
add_custom_command(OUTPUT mgs_mobile.vpk
    COMMAND vita-mksfoex -s APP_VER=${VITA_VERSION} -s TITLE_ID=${VITA_TITLEID} "${VITA_APP_NAME}" param.sfo
    COMMAND vita-pack-vpk -s param.sfo -b eboot.bin ${VPK_ARGS} mgs_mobile.vpk
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/eboot.bin" ${VPK_DEPS}
    VERBATIM)
add_custom_target(mgs_mobile.vpk-vpk ALL DEPENDS mgs_mobile.vpk)
