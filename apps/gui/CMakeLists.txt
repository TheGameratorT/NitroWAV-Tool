set(CMAKE_AUTOMOC ON)

# Everything but main(), so the tests can use it too.
add_library(nwavguicore STATIC
	ExportDialog.cpp
	LoopPlayer.cpp
	MainWindow.cpp
	WaveformPeaks.cpp
	WaveformView.cpp
)
target_include_directories(nwavguicore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nwavguicore PUBLIC nwavcore Qt6::Widgets OpenAL::OpenAL)

add_executable(nwavgui main.cpp)
target_link_libraries(nwavgui PRIVATE nwavguicore)

find_package(Qt6 COMPONENTS Test QUIET)
if(Qt6Test_FOUND)
	add_executable(gui_smoke_test tests/gui_smoke_test.cpp)
	target_link_libraries(gui_smoke_test PRIVATE nwavguicore Qt6::Test)
	add_test(NAME gui_smoke_test COMMAND gui_smoke_test)
	set_tests_properties(gui_smoke_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;ALSOFT_DRIVERS=null")

	# Renders the player through OpenAL Soft's wave file backend, so no sound card is needed.
	set(WAVE_OUT ${CMAKE_CURRENT_BINARY_DIR}/loopplayer_wave_out.wav)
	file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/alsoft_wave.conf
		"[general]\ndrivers=wave\nfrequency=44100\nchannels=stereo\nsample-type=int16\nhrtf=false\n"
		"[wave]\nfile=${WAVE_OUT}\n")
	add_executable(loopplayer_wave_test tests/loopplayer_wave_test.cpp)
	target_link_libraries(loopplayer_wave_test PRIVATE nwavguicore Qt6::Core)
	add_test(NAME loopplayer_wave_test COMMAND loopplayer_wave_test)
	set_tests_properties(loopplayer_wave_test PROPERTIES
		ENVIRONMENT "ALSOFT_CONF=${CMAKE_CURRENT_BINARY_DIR}/alsoft_wave.conf;NWAV_WAVE_OUT=${WAVE_OUT}"
		SKIP_RETURN_CODE 77)
endif()
