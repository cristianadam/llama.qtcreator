add_qtc_test(llamatools_test
  INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
  DEPENDS
    Qt::Test Qt::Core Qt::Gui
    QtCreator::Utils
    QtCreator::TextEditor
  SOURCES
    llamatools_test.cpp
    ../tools/editfile_tool.cpp
    ../tools/tool.cpp
    ../tools/tool_utils.cpp
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
