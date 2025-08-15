set(languages cs da de fr hr ja pl ru sl sv uk zh_CN zh_TW)
add_translation_targets(llamacpp
  TS_LANGUAGES ${languages}
  QM_LANGUAGES ${languages} en
  OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/${IDE_DATA_PATH}/translations"
  INSTALL_DESTINATION "${IDE_DATA_PATH}/translations"
  TARGETS LlamaCpp
)
