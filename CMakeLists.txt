cmake_minimum_required(VERSION 3.20)

project(LlamaCpp)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(QtCreator REQUIRED COMPONENTS Core TextEditor ProjectExplorer)
find_package(Qt6 COMPONENTS Widgets LinguistTools Sql Test HttpServer Network TaskTree REQUIRED)

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

add_library(markus INTERFACE)
target_include_directories(markus INTERFACE 3rdparty/markus)

add_qtc_library(LlamaSpinner STATIC
  DEFINES QT_STATICPLUGIN
  SOURCES
    llamaspinner.cpp llamaspinner.h llamaspinner.json
  DEPENDS
    Qt::Widgets
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
    Qt::Network
    QtCreator::ExtensionSystem
    QtCreator::Utils
    QtCreator::KSyntaxHighlighting
    Qt::TaskTree
    markus
    LlamaSpinner
  SOURCES
    llama.qrc
    llamaicons.h
    autoscrollarea.cpp autoscrollarea.h
    llamachateditor.cpp llamachateditor.h
    llamachatinput.cpp llamachatinput.h
    llamachatmanager.cpp llamachatmanager.h
    llamachatmessage.cpp llamachatmessage.h
    llamalocatorfilter.cpp llamalocatorfilter.h
    llamaconversationsmodel.cpp llamaconversationsmodel.h
    llamaconversationsview.cpp llamaconversationsview.h
    llamaconstants.h
    llamasyntaxhighligher.cpp llamasyntaxhighlighter.h
    llamamarkdownwidget.cpp llamamarkdownwidget.h
    llamaplugin.cpp llamaplugin.h
    llamaprojectpanel.cpp llamaprojectpanel.h
    llamasearchtoolbar.cpp llamasearchtoolbar.h
    llamasettings.cpp llamasettings.h
    llamastorage.cpp llamastorage.h
    llamathinkingsectionparser.cpp llamathinkingsectionparser.h
    llamatheme.cpp llamatheme.h
    llamatr.h
    llamatypes.h
    LlamaCpp.json.in
    markdownrenderer.cpp markdownrenderer.h
    README.md
    tools/apply_patch_tool.cpp tools/apply_patch_tool.h
    tools/edit_file_tool.cpp tools/edit_file_tool.h
    tools/factory.h
    tools/mcpbridge.cpp tools/mcpbridge.h
    tools/mcpclient.cpp tools/mcpclient.h
    tools/mcpclient_test.cpp
    tools/mcptool.cpp tools/mcptool.h
    tools/patch.cpp tools/patch.h
    tools/readfile_tool.cpp tools/readfile_tool.h
    tools/task_tool.cpp tools/task_tool.h
    tools/shell_tool.cpp tools/shell_tool.h
    tools/tool.cpp tools/tool.h
    tools/tool_utils.cpp tools/tool_utils.h
    tools/web_utils.cpp tools/web_utils.h
    tools/webfetch_tool.cpp tools/webfetch_tool.h
    tools/websearch_tool.cpp tools/websearch_tool.h
    toolsettingswidget.cpp toolsettingswidget.h
)

add_subdirectory(tests)
add_subdirectory(translations)
