cmake_minimum_required(VERSION 3.16)
project(nwavtool CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

add_library(nwavcore STATIC
	src/ima.cpp
	src/nwav1.cpp
	src/resample.cpp
	src/wav.cpp
)
target_include_directories(nwavcore PUBLIC src)

add_executable(nwavtool src/main.cpp)
target_link_libraries(nwavtool PRIVATE nwavcore)

# The player test compiles the NWAV module's core for the host. Point it at the
# module directory (the one holding module.yaml) to enable it.
set(NWAV_MODULE_DIR "" CACHE PATH "Path to the NWAV module, enables the player test")
if(NWAV_MODULE_DIR)
	enable_testing()
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
