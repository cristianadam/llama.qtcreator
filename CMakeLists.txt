cmake_minimum_required(VERSION 3.20)

project(LlamaCpp)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(QtCreator REQUIRED COMPONENTS Core TextEditor ProjectExplorer)
find_package(Qt6 COMPONENTS Widgets LinguistTools Sql REQUIRED)

# Add a CMake option that enables building your plugin with tests.
# You don't want your released plugin binaries to contain tests,
# so make that default to 'NO'.
# Enable tests by passing -DWITH_TESTS=ON to CMake.
option(WITH_TESTS "Builds with tests" NO)

if (WITH_TESTS)
  # Look for QtTest
  find_package(Qt6 REQUIRED COMPONENTS Test)

  # Tell CMake functions like add_qtc_plugin about the QtTest component.
  set(IMPLICIT_DEPENDS Qt::Test)

  # Enable ctest for auto tests.
  enable_testing()
endif()

add_qtc_library(md4c STATIC
  PUBLIC_INCLUDES 3rdparty/md4c/src
  SOURCES
    3rdparty/md4c/src/entity.c
    3rdparty/md4c/src/md4c.c
    3rdparty/md4c/src/md4c-html.c
)

add_qtc_plugin(LlamaCpp
  LONG_DESCRIPTION_MD README.md
  PLUGIN_DEPENDS
    QtCreator::Core
    QtCreator::ProjectExplorer
    QtCreator::TextEditor
  DEPENDS
    Qt::Widgets
    Qt::Sql
    QtCreator::ExtensionSystem
    QtCreator::Utils
    md4c
  SOURCES
    llama.qrc
    llamaicons.h    
    llamachateditor.cpp llamachateditor.h
    llamachatinput.cpp llamachatinput.h
    llamachatmanager.cpp llamachatmanager.h
    llamachatmessage.cpp llamachatmessage.h
    llamalocatorfilter.cpp llamalocatorfilter.h
    llamaconversationsmodel.cpp llamaconversationsmodel.h
    llamaconversationsview.cpp llamaconversationsview.h
    llamaconstants.h
    llamamarkdownwidget.cpp llamamarkdownwidget.h
    llamaplugin.cpp llamaplugin.h
    llamaprojectpanel.cpp llamaprojectpanel.h
    llamasettings.cpp llamasettings.h
    llamastorage.cpp llamastorage.h
    llamatheme.cpp llamatheme.h
    llamatr.h
    llamatypes.h
    README.md
)

add_subdirectory(translations)
