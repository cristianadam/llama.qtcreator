cmake_minimum_required(VERSION 3.20)

project(LlamaCpp)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt Creator bundles yaml-cpp (used for the skill frontmatter) and exports
# it as the QtCreator::yaml-cpp imported target: the headers come from the
# Qt Creator source tree, the library is the one shipped in the app bundle
# (resolved at runtime via the app's rpath).
find_package(QtCreator REQUIRED COMPONENTS Core TextEditor ProjectExplorer yaml-cpp)
find_package(Qt6 COMPONENTS Widgets LinguistTools Sql Test HttpServer Network TaskTree Svg
               REQUIRED)

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
    src/llamaspinner.cpp src/llamaspinner.h src/llamaspinner.json
  DEPENDS
    Qt::Widgets
)

add_qtc_plugin(LlamaCpp
  LONG_DESCRIPTION_MD README.md
  INCLUDES
    src
  PLUGIN_DEPENDS
    QtCreator::Core
    QtCreator::ProjectExplorer
    QtCreator::TextEditor
  DEPENDS
    QtCreator::yaml-cpp
    Qt::Widgets
    Qt::Sql
    Qt::Network
    QtCreator::ExtensionSystem
    QtCreator::Utils
    QtCreator::KSyntaxHighlighting
    QtCreator::quickjsng
    Qt::TaskTree
    Qt::Svg
    markus
    LlamaSpinner
  SOURCES
    src/llama.qrc
    src/llamaicons.h
    src/ansitext.cpp src/ansitext.h
    src/autoscrollarea.cpp src/autoscrollarea.h
    src/fimcache.cpp src/fimcache.h
    src/llamachateditor.cpp src/llamachateditor.h
    src/llamachatinput.cpp src/llamachatinput.h
    src/llamachatmanager.cpp src/llamachatmanager.h
    src/llamachatmessage.cpp src/llamachatmessage.h
    src/llamalocatorfilter.cpp src/llamalocatorfilter.h
    src/llamaconversationsmodel.cpp src/llamaconversationsmodel.h
    src/llamaconversationsview.cpp src/llamaconversationsview.h
    src/llamaconstants.h
    src/llamahtmlexporter.cpp src/llamahtmlexporter.h
    src/llamasyntaxhighligher.cpp src/llamasyntaxhighlighter.h
    src/llamamarkdownwidget.cpp src/llamamarkdownwidget.h
    src/llamaplugin.cpp src/llamaplugin.h
    src/llamaprojectpanel.cpp src/llamaprojectpanel.h
    src/llamasearchtoolbar.cpp src/llamasearchtoolbar.h
    src/llamasettings.cpp src/llamasettings.h
    src/llamastorage.cpp src/llamastorage.h
    src/llamathinkingsectionparser.cpp src/llamathinkingsectionparser.h
    src/llamatheme.cpp src/llamatheme.h
    src/llamatr.h
    src/llamatypes.h
    LlamaCpp.json.in
    src/katexengine.cpp src/katexengine.h
    src/markdownrenderer.cpp src/markdownrenderer.h
    src/mermaidengine.cpp src/mermaidengine.h
    README.md
    src/tools/apply_patch_tool.cpp src/tools/apply_patch_tool.h
    src/tools/edit_file_tool.cpp src/tools/edit_file_tool.h
    src/tools/factory.h
    src/tools/mcpbridge.cpp src/tools/mcpbridge.h
    src/tools/mcpclient.cpp src/tools/mcpclient.h
    src/tools/katexengine_test.cpp
    src/tools/mcpclient_test.cpp
    src/tools/mermaidengine_test.cpp
    src/tools/mcptool.cpp src/tools/mcptool.h
    src/tools/mcpserverconfig.h
    src/tools/patch.cpp src/tools/patch.h
    src/tools/readfile_tool.cpp src/tools/readfile_tool.h
    src/tools/skill_tool.cpp src/tools/skill_tool.h
    src/tools/search_tool.cpp src/tools/search_tool.h
    src/tools/find_tool.cpp src/tools/find_tool.h
    src/tools/ls_tool.cpp src/tools/ls_tool.h
    src/tools/ripgrep.cpp src/tools/ripgrep.h
    src/tools/mxc.cpp src/tools/mxc.h
    src/tools/task_tool.cpp src/tools/task_tool.h
    src/tools/bash_tool.cpp src/tools/bash_tool.h
    src/tools/tool.cpp src/tools/tool.h
    src/tools/tool_utils.cpp src/tools/tool_utils.h
    src/tools/todowrite_tool.cpp src/tools/todowrite_tool.h
    src/tools/write_tool.cpp src/tools/write_tool.h
    src/tools/web_utils.cpp src/tools/web_utils.h
    src/tools/webfetch_tool.cpp src/tools/webfetch_tool.h
    src/tools/websearch_tool.cpp src/tools/websearch_tool.h
    src/tools/windows_sandbox.cpp src/tools/windows_sandbox.h
    src/projectinstructions.cpp src/projectinstructions.h
    src/promptssettingswidget.cpp src/promptssettingswidget.h
    src/skills.cpp src/skills.h
    src/skillssettingswidget.cpp src/skillssettingswidget.h
    src/toolsettingswidget.cpp src/toolsettingswidget.h
)

add_subdirectory(tests)
add_subdirectory(translations)
