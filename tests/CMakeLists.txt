add_qtc_test(llamatools_test
  INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
  DEPENDS
    Qt::Test Qt::Core Qt::Gui Qt::Network Qt::Widgets
    QtCreator::Utils
    QtCreator::TextEditor
    QtCreator::Core
    QtCreator::ProjectExplorer
  SOURCES
    llamatools_test.cpp
    ../llamasettings.cpp
    ../toolsettingswidget.cpp
    ../tools/apply_patch_tool.cpp
    ../tools/patch.cpp
    ../tools/tool.cpp
    ../tools/tool_utils.cpp
    ../tools/web_utils.cpp
    ../tools/webfetch_tool.cpp
    ../tools/websearch_tool.cpp
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
