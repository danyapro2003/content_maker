cmake_minimum_required(VERSION 3.16)

project(content_clipper VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network)

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        mainwindow.ui
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(content_clipper
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
        pipelinemanager.h pipelinemanager.cpp
        mediaextractor.h mediaextractor.cpp
        metadataparser.h metadataparser.cpp
        appsettings.h appsettings.cpp
        llmclient.h llmclient.cpp
        whisperrunner.h whisperrunner.cpp
        cliprenderer.h cliprenderer.cpp
        srtutils.h srtutils.cpp
        transcriptpacker.h transcriptpacker.cpp
        diarizerunner.h diarizerunner.cpp
        stories3runner.h stories3runner.cpp
        stories3pipeline.h stories3pipeline.cpp
        s3cache.h s3cache.cpp
        subtitlecomposer.h subtitlecomposer.cpp
        plans.md
    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET content_clipper APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(content_clipper SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(content_clipper
            ${PROJECT_SOURCES}
        )
    endif()
endif()

target_link_libraries(content_clipper PRIVATE Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)

# python-скрипты (диаризация и пр.) кладём рядом с exe
add_custom_command(TARGET content_clipper POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/scripts $<TARGET_FILE_DIR:content_clipper>/scripts)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.content_clipper)
endif()
set_target_properties(content_clipper PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS content_clipper
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(content_clipper)
endif()

option(CONTENT_CLIPPER_BUILD_TESTS "Build V3 regression tests" OFF)
if(CONTENT_CLIPPER_BUILD_TESTS)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(test_stories3 tests/test_stories3.cpp
        appsettings.cpp llmclient.cpp s3cache.cpp stories3runner.cpp stories3pipeline.cpp
        pipelinemanager.cpp mediaextractor.cpp metadataparser.cpp whisperrunner.cpp
        cliprenderer.cpp srtutils.cpp subtitlecomposer.cpp transcriptpacker.cpp diarizerunner.cpp)
    target_include_directories(test_stories3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(test_stories3 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    target_compile_definitions(test_stories3 PRIVATE CLIPPER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_stories3 PRIVATE Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)
    add_test(NAME stories3 COMMAND test_stories3 ${Python3_EXECUTABLE})
    add_test(NAME transcript_v3 COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_transcript_v3.py)
    set_tests_properties(stories3 transcript_v3 PROPERTIES TIMEOUT 60)
endif()
