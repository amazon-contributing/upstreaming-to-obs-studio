project(simulcast)

set(PROJECT_NAME simulcast)

find_qt(COMPONENTS Core Widgets Svg Network)

find_package(CURL REQUIRED)

add_library(${PROJECT_NAME} MODULE)
add_library(OBS::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE)

target_sources(
  ${PROJECT_NAME}
  PRIVATE # cmake-format: sortable
          src/berryessa-every-minute.cpp
          src/berryessa-every-minute.hpp
          src/berryessa-submitter.cpp
          src/berryessa-submitter.hpp
          src/common.h
          src/copy-from-obs/remote-text.cpp
          src/copy-from-obs/remote-text.hpp
          src/global-service.cpp
          src/global-service.h
          src/goliveapi-network.cpp
          src/goliveapi-network.hpp
          src/goliveapi-postdata.cpp
          src/goliveapi-postdata.hpp
          src/presentmon-csv-capture.cpp
          src/presentmon-csv-capture.hpp
          src/presentmon-csv-parser.cpp
          src/presentmon-csv-parser.hpp
          src/simulcast-dock-widget.cpp
          src/simulcast-dock-widget.h
          src/simulcast-output.cpp
          src/simulcast-output.h
          src/simulcast-plugin.cpp
          src/simulcast-plugin.h)

configure_file(src/plugin-macros.h.in plugin-macros.generated.h)

target_sources(${PROJECT_NAME} PRIVATE plugin-macros.generated.h)

target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE OBS::libobs
          OBS::frontend-api
          Qt::Core
          Qt::Widgets
          Qt::Svg
          Qt::Network
          CURL::libcurl)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

set_target_properties(
  ${PROJECT_NAME}
  PROPERTIES FOLDER plugins
             PREFIX ""
             AUTOMOC ON
             AUTOUIC ON
             AUTORCC ON)

if(OS_WINDOWS)
  set_property(
    TARGET ${PROJECT_NAME}
    APPEND
    PROPERTY AUTORCC_OPTIONS --format-version 1)
endif()

setup_plugin_target(${PROJECT_NAME})
