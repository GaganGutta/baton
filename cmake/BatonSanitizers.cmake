# Sanitizer flags are applied globally (before dependencies are added) so that
# GoogleTest and friends are instrumented as well. TSan in particular reports
# false positives when only part of a program is instrumented.
if(BATON_SANITIZE)
  string(REPLACE ";" "," _baton_sanitize_list "${BATON_SANITIZE}")
  add_compile_options(
    -fsanitize=${_baton_sanitize_list}
    -fno-sanitize-recover=all
    -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${_baton_sanitize_list})
  message(STATUS "baton: sanitizers enabled: ${_baton_sanitize_list}")
endif()
