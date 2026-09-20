cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# 设置页"一键添加浏览器源"需要枚举场景/创建源
find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/util.cpp
    src/http.cpp
    src/lcu.cpp
    src/gamedata.cpp
    src/assets.cpp
    src/monitor.cpp
    src/webfiles.cpp
    src/webserver.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/web-resources.rc"
)

# 页面文件(data/web)内嵌为资源,运行期可解压;data/ 目录同时由模板 helper
# 自动安装到 <插件名>/data 与拷贝到 rundir
configure_file(src/web-resources.rc.in "${CMAKE_CURRENT_BINARY_DIR}/web-resources.rc" @ONLY)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

if(MSVC)
  # 第三方单头库(nlohmann/stb)不参与告警升级
  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE "/external:I${CMAKE_CURRENT_SOURCE_DIR}/src/deps")
endif()

# WinHTTP(REST)、WMI(进程命令行)、COM、WinSock(本地 HTTP 服务)
target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE winhttp wbemuuid ole32 oleaut32 user32 shlwapi ws2_32 shell32
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# ---------------------------------------------------------------- NSIS 安装器
# OBS 31 起按"每插件一根目录"扫描 C:\ProgramData\obs-studio\plugins\<名字>\bin\64bit
# 与 <名字>\data —— 与上面 set_target_properties_plugin 的安装树一致,故安装器把
# 安装树原样放入该根目录即可。CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL 让高版本
# 安装时先卸载旧版(自动覆盖、不留陈旧文件),并在系统里注册卸载入口。
if(MSVC)
  set(CPACK_GENERATOR "NSIS")
  set(CPACK_PACKAGE_NAME "${CMAKE_PROJECT_NAME}")
  set(CPACK_PACKAGE_VERSION "${CMAKE_PROJECT_VERSION}")
  set(CPACK_PACKAGE_VENDOR "${PLUGIN_AUTHOR}")
  set(CPACK_PACKAGE_DISPLAY_NAME "海克斯大乱斗战绩条(OBS 插件)")
  set(CPACK_PACKAGE_HOMEPAGE_URL "${PLUGIN_WEBSITE}")
  set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "${CMAKE_PROJECT_NAME}")
  set(CPACK_PACKAGE_FILE_NAME "${CMAKE_PROJECT_NAME}-${CMAKE_PROJECT_VERSION}-windows-x64-Installer")
  set(CPACK_OUTPUT_FILE_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/release")
  set(CPACK_NSIS_INSTALL_ROOT "$COMMONAPPDATA")
  set(CPACK_PACKAGE_INSTALL_DIRECTORY "obs-studio\\plugins")
  set(CPACK_NSIS_DISPLAY_NAME "${CPACK_PACKAGE_DISPLAY_NAME}")
  set(CPACK_NSIS_PACKAGE_NAME "${CPACK_PACKAGE_DISPLAY_NAME}")
  set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
  set(CPACK_NSIS_MODIFY_PATH OFF)
  set(CPACK_NSIS_CONTACT "${PLUGIN_EMAIL}")
  set(CPACK_NSIS_HELP_LINK "${PLUGIN_WEBSITE}")
  set(CPACK_NSIS_URL_INFO_ABOUT "${PLUGIN_WEBSITE}")
  include(CPack)
endif()
