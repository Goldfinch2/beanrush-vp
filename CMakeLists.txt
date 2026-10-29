cmake_minimum_required(VERSION 3.21)

project(beanrush VERSION 0.2.0 LANGUAGES C CXX)

# Bean Rush on the VectorPie SDK. Builds, from WSL2 Debian:
#   cmake --preset pc && cmake --build --preset pc     # build-pc/beanrush.exe
#   cmake --preset pi && cmake --build --preset pi     # build-pi/beanrush (arm64)
#   cmake --build --preset pi --target vpk             # build-pi/beanrush-<version>.vpk

# The installed VectorPie SDK (`cmake --install` in the SDK: CMAKE_PREFIX_PATH
# at ~/VectorPie/dist/pc or /pi, as the presets set it).
find_package(VectorPie 0.1 REQUIRED CONFIG)

set(CMAKE_C_STANDARD 99)

add_executable(beanrush
    src/main.c
    src/course.c
    src/beans.c
    src/render.c
    src/props.c
    src/music.c)
vp_program(beanrush)                   # the SDK's link settings (static .exe, SDL2.dll)
if(NOT MSVC)
    target_compile_options(beanrush PRIVATE -Wall -Wextra)
    target_link_libraries(beanrush PRIVATE m)
endif()

# The music, packed and scrambled into one file (tools/pack_music.py): its
# licence forbids letting players extract the tracks, so they never ship as
# .ogg files, and they are not in the repository (music/ is the builder's
# own copy). Without them the game builds and plays silent. The names here
# are the ones src/main.c plays.
set(MUSIC_TRACKS
    funny_toys_loop_no_drumfills
    bunny_is_so_funny_long_loop
    funny_toys_loop
    shine_upon_us
    bunnies_are_sleeping_loop)
list(TRANSFORM MUSIC_TRACKS PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/music/)
list(TRANSFORM MUSIC_TRACKS APPEND .ogg)
set(HAVE_MUSIC ON)
foreach(track ${MUSIC_TRACKS})
    if(NOT EXISTS ${track})
        set(HAVE_MUSIC OFF)
    endif()
endforeach()
set(MUSIC_COPY)
set(VPK_MUSIC_COPY)
if(HAVE_MUSIC)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(MUSIC_DAT ${CMAKE_BINARY_DIR}/music.dat)
    add_custom_command(OUTPUT ${MUSIC_DAT}
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_music.py ${MUSIC_DAT} ${MUSIC_TRACKS}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_music.py ${MUSIC_TRACKS}
        COMMENT "Packing the music"
        VERBATIM)
    add_custom_target(music DEPENDS ${MUSIC_DAT})
    add_dependencies(beanrush music)
    set(MUSIC_COPY COMMAND ${CMAKE_COMMAND} -E copy ${MUSIC_DAT} $<TARGET_FILE_DIR:beanrush>/data/)
    set(VPK_MUSIC_COPY COMMAND ${CMAKE_COMMAND} -E copy ${MUSIC_DAT} ${CMAKE_BINARY_DIR}/vpk/beanrush/data/)
else()
    message(WARNING "Bean Rush: the music tracks are not in music/: building without music")
endif()

# The music and its credits go next to the executable, where vp_asset_path finds them.
add_custom_command(TARGET beanrush POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/data $<TARGET_FILE_DIR:beanrush>/data
    ${MUSIC_COPY}
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/game.ini ${CMAKE_CURRENT_SOURCE_DIR}/leds.cfg
            $<TARGET_FILE_DIR:beanrush>/)

# The marquee (the menu's, and HDMI's while playing on the board): put a
# marquee.png here. Without one, the menu shows its default marquee.
set(MARQUEE ${CMAKE_CURRENT_SOURCE_DIR}/marquee.png)
set(VPK_MARQUEE_COPY)
if(EXISTS ${MARQUEE})
    add_custom_command(TARGET beanrush POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${MARQUEE} $<TARGET_FILE_DIR:beanrush>/)
endif()

# The game package, for Settings -> GAMES -> INSTALL GAMES (the Pi build only).
set(VPK_STAGE ${CMAKE_BINARY_DIR}/vpk/beanrush)
if(EXISTS ${MARQUEE})
    set(VPK_MARQUEE_COPY COMMAND ${CMAKE_COMMAND} -E copy ${MARQUEE} ${VPK_STAGE}/)
endif()
add_custom_target(vpk
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/vpk
    COMMAND ${CMAKE_COMMAND} -E make_directory ${VPK_STAGE}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:beanrush> ${VPK_STAGE}/
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/data ${VPK_STAGE}/data
    ${VPK_MUSIC_COPY}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/game.ini
            ${CMAKE_CURRENT_SOURCE_DIR}/leds.cfg ${VPK_STAGE}/
    ${VPK_MARQUEE_COPY}
    COMMAND sh ${VECTORPIE_PACK} ${VPK_STAGE} ${CMAKE_BINARY_DIR}
    DEPENDS beanrush
    VERBATIM)
