include(BundleUtilities)

if(NOT DEFINED APP_BUNDLE)
  message(FATAL_ERROR "APP_BUNDLE is required")
endif()

if(NOT EXISTS "${APP_BUNDLE}")
  message(FATAL_ERROR "Bundle path does not exist: ${APP_BUNDLE}")
endif()

set(search_dirs "")
if(DEFINED SEARCH_DIRS_PIPE AND NOT SEARCH_DIRS_PIPE STREQUAL "")
  string(REPLACE "|" ";" search_dirs "${SEARCH_DIRS_PIPE}")
endif()

# Ensure dependencies are copied into the plugin bundle and relinked.
fixup_bundle("${APP_BUNDLE}" "" "${search_dirs}")

if(APPLE)
  find_program(CODESIGN_EXECUTABLE codesign)
  if(NOT CODESIGN_EXECUTABLE)
    message(WARNING "codesign not found; DAL plugin may fail strict validation.")
  else()
    execute_process(
      COMMAND "${CODESIGN_EXECUTABLE}" --force --deep --sign - --timestamp=none "${APP_BUNDLE}"
      RESULT_VARIABLE sign_rc
      OUTPUT_VARIABLE sign_out
      ERROR_VARIABLE sign_err
    )
    if(NOT sign_rc EQUAL 0)
      message(FATAL_ERROR "codesign failed for ${APP_BUNDLE}\n${sign_out}\n${sign_err}")
    endif()

    execute_process(
      COMMAND "${CODESIGN_EXECUTABLE}" --verify --verbose=2 --deep "${APP_BUNDLE}"
      RESULT_VARIABLE verify_rc
      OUTPUT_VARIABLE verify_out
      ERROR_VARIABLE verify_err
    )
    if(NOT verify_rc EQUAL 0)
      message(FATAL_ERROR "codesign verification failed for ${APP_BUNDLE}\n${verify_out}\n${verify_err}")
    endif()
  endif()
endif()
