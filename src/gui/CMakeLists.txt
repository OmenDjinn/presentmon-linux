# presentmon-gui: desktop front end (Qt 6 Quick + Kirigami). See docs/ARCHITECTURE.md section 3.5.
#
# Targets
#   pml_gui          static library + QML module "PresentMon": the C++ backend types and the QML pages
#   presentmon-gui   the application (main.cpp)
#   pml_gui_tests    backend unit tests (GTest; QGuiApplication on the offscreen platform, no window)
#
# Data comes in-process from the same libraries as the CLI and the overlay: pml_common (segments,
# overlay control), pml_metrics (engines, frame-time summary) and pml_overlay_core (OverlayStats, so
# the live numbers equal the in-game overlay's). Recording runs the presentmon CLI as a subprocess.
if(NOT TARGET pml_overlay_core)
	message(FATAL_ERROR "PML_BUILD_GUI needs PML_BUILD_LAYER and PML_BUILD_OVERLAY (it reuses the overlay's statistics)")
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Widgets Concurrent)
find_package(KF6Kirigami REQUIRED)       # QML module org.kde.kirigami, loaded at run time
find_package(KF6GlobalAccel REQUIRED)    # global shortcuts (record, overlay toggle)

set(CMAKE_AUTOMOC ON)

# The QML files live in qml/ but belong to the module root (qrc:/qt/qml/PresentMon/Main.qml).
set(PML_GUI_QML Main LivePage CapturesPage SettingsPage StatTile ChartPanel ProcessDelegate OverlayControls
	RecordControls CaptureDelegate CompareTable Histogram VrrPanel FrameCapControls Fmt)
foreach(name ${PML_GUI_QML})
	set_source_files_properties(qml/${name}.qml PROPERTIES QT_RESOURCE_ALIAS ${name}.qml)
endforeach()
set_source_files_properties(qml/Fmt.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_library(pml_gui STATIC)
qt_add_qml_module(pml_gui
	URI PresentMon
	VERSION 1.0
	RESOURCE_PREFIX /qt/qml
	OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/PresentMon
	SOURCES
		backend/format.cpp backend/format.hpp
		backend/processmodel.cpp backend/processmodel.hpp
		backend/frameseries.cpp backend/frameseries.hpp
		backend/livesession.cpp backend/livesession.hpp
		backend/framechart.cpp backend/framechart.hpp
		backend/recorder.cpp backend/recorder.hpp
		backend/csvreader.cpp backend/csvreader.hpp
		backend/captureanalysis.cpp backend/captureanalysis.hpp
		backend/capturelibrary.cpp backend/capturelibrary.hpp
		backend/layerstatus.cpp backend/layerstatus.hpp
		backend/hotkeys.cpp backend/hotkeys.hpp
		backend/drmvrr.cpp backend/drmvrr.hpp
	QML_FILES
		qml/Main.qml
		qml/LivePage.qml
		qml/CapturesPage.qml
		qml/SettingsPage.qml
		qml/StatTile.qml
		qml/ChartPanel.qml
		qml/ProcessDelegate.qml
		qml/OverlayControls.qml
		qml/RecordControls.qml
		qml/CaptureDelegate.qml
		qml/CompareTable.qml
		qml/Histogram.qml
		qml/VrrPanel.qml
		qml/ExperiencePanel.qml
		qml/FrameCapControls.qml
		qml/Fmt.qml)
target_include_directories(pml_gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/backend)
target_link_libraries(pml_gui PUBLIC
	Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Concurrent
	KF6::GlobalAccel
	pml_common pml_metrics pml_overlay_core)
target_compile_options(pml_gui PRIVATE ${PML_WARNINGS})

qt_add_executable(presentmon-gui main.cpp)
qt_add_resources(presentmon-gui "presentmon_gui_icon" PREFIX "/presentmon-gui" BASE data FILES data/presentmon-gui.svg)
target_link_libraries(presentmon-gui PRIVATE pml_gui pml_guiplugin Qt6::Widgets Qt6::QuickControls2)
target_compile_options(presentmon-gui PRIVATE ${PML_WARNINGS})
# Only our own module is static; Kirigami & co. are the system's shared QML plugins, loaded at run
# time. Without this, finalizing the executable tries to link them statically and warns.
set_target_properties(presentmon-gui PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
target_compile_definitions(presentmon-gui PRIVATE PML_VERSION="${PROJECT_VERSION}")

# Installed next to the presentmon CLI (the Recorder runs <bindir>/presentmon).
install(TARGETS presentmon-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# Desktop entry, written at install time like the layer manifest: its Exec path follows --prefix (a
# configure-time path would point at /usr/local after `cmake --install build --prefix ~/.local`).
install(CODE "
	if(IS_ABSOLUTE \"${CMAKE_INSTALL_BINDIR}\")
		set(PML_GUI_BINDIR \"${CMAKE_INSTALL_BINDIR}\")
	else()
		set(PML_GUI_BINDIR \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
	endif()
	set(_dir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/applications\")
	configure_file(\"${CMAKE_CURRENT_SOURCE_DIR}/data/presentmon-gui.desktop.in\" \"\${_dir}/presentmon-gui.desktop\" @ONLY)
	message(STATUS \"Installing: \${_dir}/presentmon-gui.desktop\")
")
install(FILES data/presentmon-gui.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

if(PML_BUILD_TESTS)
	find_package(Qt6 REQUIRED COMPONENTS Test)
	add_executable(pml_gui_tests
		tests/test_main.cpp
		tests/test_support.hpp
		tests/live_tests.cpp
		tests/capture_tests.cpp
		tests/chart_tests.cpp)
	target_link_libraries(pml_gui_tests PRIVATE pml_gui pml_guiplugin Qt6::Test GTest::gtest)
	target_compile_options(pml_gui_tests PRIVATE ${PML_WARNINGS})
	set_target_properties(pml_gui_tests PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
	target_compile_definitions(pml_gui_tests PRIVATE
		PML_GOLD_DIR="${PML_PRESENTMON_DIR}/Tests/Gold"
		PML_BIN_DIR="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
	add_test(NAME pml_gui_tests COMMAND pml_gui_tests)
	set_tests_properties(pml_gui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

	# The application itself, headless: loads every page, renders, saves a screenshot, checks QML warnings.
	find_program(PML_PYTHON3 python3)
	if(PML_PYTHON3)
		add_test(NAME pml_gui_smoke
			COMMAND ${PML_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/tests/gui_smoke.py --build-dir ${CMAKE_BINARY_DIR})
		set_tests_properties(pml_gui_smoke PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
	endif()
endif()
