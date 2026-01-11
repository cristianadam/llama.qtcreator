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
