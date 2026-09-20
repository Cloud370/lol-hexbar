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
