add_qtc_test(llamatools_test
  INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
  DEPENDS
    Qt::Test Qt::Core Qt::Gui Qt::Network Qt::Sql Qt::Widgets Qt::Svg
    Qt::TaskTree
    QtCreator::Utils
    QtCreator::TextEditor
    QtCreator::Core
    QtCreator::ProjectExplorer
    QtCreator::KSyntaxHighlighting
    QtCreator::quickjsng
    QtCreator::yaml-cpp
    markus
  SOURCES
    llamatools_test.cpp
    ../src/llama.qrc
    ../src/ansitext.cpp
    ../src/llamachatmanager.cpp
    ../src/llamasyntaxhighligher.cpp
    ../src/katexengine.cpp
    ../src/markdownrenderer.cpp
    ../src/mermaidengine.cpp
    ../src/llamasettings.cpp
    ../src/llamastorage.cpp
    ../src/llamahtmlexporter.cpp
    ../src/llamatheme.cpp
    ../src/llamathinkingsectionparser.cpp
    ../src/promptssettingswidget.cpp
    ../src/promptssettingswidget.h
    ../src/projectinstructions.cpp
    ../src/skills.cpp
    ../src/skillssettingswidget.cpp
    ../src/toolsettingswidget.cpp
    ../src/tools/apply_patch_tool.cpp
    ../src/tools/edit_file_tool.cpp
    ../src/tools/mcpbridge.cpp
    ../src/tools/mcpclient.cpp
    ../src/tools/mcptool.cpp
    ../src/tools/patch.cpp
    ../src/tools/bash_tool.cpp
    ../src/tools/readfile_tool.cpp
    ../src/tools/skill_tool.cpp
    ../src/tools/task_tool.cpp
    ../src/tools/tool.cpp
    ../src/tools/todowrite_tool.cpp
    ../src/tools/write_tool.cpp
    ../src/tools/tool_utils.cpp
    ../src/tools/web_utils.cpp
    ../src/tools/webfetch_tool.cpp
    ../src/tools/websearch_tool.cpp
    ../src/tools/search_tool.cpp
    ../src/tools/find_tool.cpp
    ../src/tools/ls_tool.cpp
    ../src/tools/ripgrep.cpp
    ../src/tools/mxc.cpp
    ../src/tools/windows_sandbox.cpp
)

# The AUTOMOC scanner (dep‑file mode) chokes on llamatools_test.cpp in this
# environment, so skip AUTOMOC for it and run moc on it explicitly instead
# (qt_wrap_cpp generates llamatools_test.moc and puts it on the include path,
# picked up by the #include at the end of the source).
if (TARGET llamatools_test)
  set_source_files_properties(llamatools_test.cpp PROPERTIES SKIP_AUTOMOC ON)
  qt_wrap_cpp(llamatools_test_moc llamatools_test.cpp TARGET llamatools_test)
endif()

add_qtc_test(fimcache_test
  INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
  DEPENDS
    Qt::Test Qt::Core
  SOURCES
    fimcache_test.cpp
    ../src/fimcache.cpp
)

add_qtc_test(autoscrollarea_test
  INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
  DEPENDS
    Qt::Test Qt::Core Qt::Gui Qt::Widgets
  SOURCES
    autoscrollarea_test.cpp
    ../src/autoscrollarea.cpp
)

add_qtc_test(markdownrenderer_test
  INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
  DEPENDS
    Qt::Test Qt::Core Qt::Gui Qt::Widgets Qt::Svg
    QtCreator::Utils
    QtCreator::TextEditor
    QtCreator::KSyntaxHighlighting
    QtCreator::quickjsng
    markus
  SOURCES
    markdownrenderer_test.cpp
    ../src/ansitext.cpp
    ../src/markdownrenderer.cpp
    ../src/katexengine.cpp
    ../src/mermaidengine.cpp
    ../src/llamasyntaxhighligher.cpp
    ../src/llama.qrc
)
