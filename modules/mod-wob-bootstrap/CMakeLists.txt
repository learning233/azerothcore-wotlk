AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src/mod_wob_bootstrap.cpp")
AC_ADD_SCRIPT_LOADER(
  "mod_wob_bootstrap"
  "${CMAKE_CURRENT_LIST_DIR}/src/mod_wob_bootstrap_loader.cpp"
)
