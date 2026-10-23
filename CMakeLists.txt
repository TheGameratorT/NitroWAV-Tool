cmake_minimum_required(VERSION 3.16)
project(nwavtool CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

add_library(nwavcore STATIC
	src/convert.cpp
	src/decode.cpp
	src/ima.cpp
	src/nwav1.cpp
	src/resample.cpp
	src/wav.cpp
)
target_include_directories(nwavcore PUBLIC src)

# FFmpeg's libav* libraries read every audio format but WAV, which our own RIFF
# code keeps reading and writing (smpl loops, cue events, ADPCM as it is). They
# are optional: without them the tools still read WAV.
option(NWAV_USE_FFMPEG "Read MP3, FLAC, Ogg, AAC and other formats through FFmpeg" ON)
if(NWAV_USE_FFMPEG)
	find_package(PkgConfig QUIET)
	if(PKG_CONFIG_FOUND)
		pkg_check_modules(LIBAV QUIET IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
	endif()
	if(LIBAV_FOUND)
		message(STATUS "FFmpeg found (libavformat ${LIBAV_libavformat_VERSION}), other audio formats can be read")
		target_compile_definitions(nwavcore PRIVATE NWAV_HAVE_LIBAV)
		target_link_libraries(nwavcore PUBLIC PkgConfig::LIBAV)
	else()
		message(STATUS "FFmpeg (libavformat, libavcodec, libavutil, libswresample) not found, only WAV can be read")
	endif()
endif()

enable_testing()

add_subdirectory(apps/cli)

# The GUI needs Qt6 Widgets and OpenAL Soft. The core and the CLI need nothing.
option(NWAV_BUILD_GUI "Build the nwavgui waveform editor" ON)
if(NWAV_BUILD_GUI)
	find_package(Qt6 COMPONENTS Widgets QUIET)
	find_package(OpenAL CONFIG QUIET)
	if(Qt6_FOUND AND OpenAL_FOUND)
		add_subdirectory(apps/gui)
	else()
		message(STATUS "Qt6 Widgets or OpenAL not found, skipping nwavgui")
	endif()
endif()

add_executable(loopcursor_test tests/loopcursor_test.cpp)
target_link_libraries(loopcursor_test PRIVATE nwavcore)
add_test(NAME loopcursor_test COMMAND loopcursor_test)

add_executable(decode_test tests/decode_test.cpp)
target_link_libraries(decode_test PRIVATE nwavcore)
add_test(NAME decode_test COMMAND decode_test)

add_executable(rewrite_test tests/rewrite_test.cpp)
target_link_libraries(rewrite_test PRIVATE nwavcore)
add_test(NAME rewrite_test COMMAND rewrite_test)

# The player test compiles the NWAV module's core for the host. Point it at the
# module directory (the one holding module.yaml) to enable it.
set(NWAV_MODULE_DIR "" CACHE PATH "Path to the NWAV module, enables the player test")
if(NWAV_MODULE_DIR)
	add_executable(player_test
		tests/player_test.cpp
		${NWAV_MODULE_DIR}/source9/core/wav.cpp
		${NWAV_MODULE_DIR}/source9/core/decode.cpp
	)
	target_compile_definitions(player_test PRIVATE NWAV_HOST_TEST)
	target_include_directories(player_test PRIVATE
		tests
		${NWAV_MODULE_DIR}/include
		${NWAV_MODULE_DIR}/source9/core
	)
	target_link_libraries(player_test PRIVATE nwavcore)
	add_test(NAME player_test COMMAND player_test)
endif()
