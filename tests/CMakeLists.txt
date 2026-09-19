add_qtc_test(llamatools_test
  INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
  DEPENDS
    Qt::Test Qt::Core Qt::Gui Qt::Network Qt::Sql Qt::Widgets
    Qt::TaskTree
    QtCreator::Utils
    QtCreator::TextEditor
    QtCreator::Core
    QtCreator::ProjectExplorer
  SOURCES
    llamatools_test.cpp
    ../llamachatmanager.cpp
    ../llamasettings.cpp
    ../llamastorage.cpp
    ../llamathinkingsectionparser.cpp
    ../toolsettingswidget.cpp
    ../tools/apply_patch_tool.cpp
    ../tools/edit_file_tool.cpp
    ../tools/mcpbridge.cpp
    ../tools/mcpclient.cpp
    ../tools/mcptool.cpp
    ../tools/patch.cpp
    ../tools/bash_tool.cpp
    ../tools/task_tool.cpp
    ../tools/tool.cpp
    ../tools/tool_utils.cpp
    ../tools/web_utils.cpp
    ../tools/webfetch_tool.cpp
    ../tools/websearch_tool.cpp
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
  DEPENDS
    Qt::Test Qt::Core
  SOURCES
    fimcache_test.cpp
    ../fimcache.cpp
)

add_qtc_test(autoscrollarea_test
  INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
  DEPENDS
    Qt::Test Qt::Core Qt::Gui Qt::Widgets
  SOURCES
    autoscrollarea_test.cpp
    ../autoscrollarea.cpp
)

add_qtc_test(markdownrenderer_test
  INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
  DEPENDS
    Qt::Test Qt::Core Qt::Gui Qt::Widgets
    QtCreator::Utils
    QtCreator::TextEditor
    QtCreator::KSyntaxHighlighting
    markus
  SOURCES
    markdownrenderer_test.cpp
    ../markdownrenderer.cpp
    ../llamasyntaxhighligher.cpp
)
