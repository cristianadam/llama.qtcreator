add_qtc_test(llamatools_test
  INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
  DEPENDS
    Qt::Test Qt::Core Qt::Gui
    QtCreator::Utils
  SOURCES
    llamatools_test.cpp
    ../llamatools.cpp
)
