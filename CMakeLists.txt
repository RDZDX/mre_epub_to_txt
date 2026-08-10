cmake_minimum_required(VERSION 3.10)

project("epub_to_txt")

set(APP_NAME "epub_to_txt")
set(DEVELOPER_NAME "RDZDX")
set(RAM "1500") # KB reservation for the ZIP buffer, inflate workspace, and app runtime
set(IMSI "1234567890" CACHE STRING "Imsi of first SIM-card")
set(API "File")
set(APPID "0")
set(CERTID "1" CACHE STRING "")
set(CERT "none" CACHE STRING "Path to cert")
option(BACKGROUND "App can work background" OFF)

set(MRE_SDK "$ENV{MRE_SDK}" CACHE STRING "MRE SDK dir")
set(TinyMRESDK "$ENV{TinyMRESDK}" CACHE STRING "TinyMRESDK dir")

if(BACKGROUND)
    add_definitions(-DSUPPORT_BG)
endif()

include(cmake/MreUtils.cmake)

add_subdirectory(resources)
add_subdirectory(mreapi)
add_subdirectory(run)
add_subdirectory(main)
