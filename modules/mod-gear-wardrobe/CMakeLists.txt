# mod-gear-wardrobe：把客户端 .wob_wardrobe 隐藏命令换成真实装备。
#
# 放到 AzerothCore 的 modules/mod-gear-wardrobe/ 下重新 cmake 即可。
AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src/mod_gear_wardrobe.cpp")
AC_ADD_SCRIPT_LOADER(
  "mod_gear_wardrobe"
  "${CMAKE_CURRENT_LIST_DIR}/src/mod_gear_wardrobe_loader.cpp"
)
