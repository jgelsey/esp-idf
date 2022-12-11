# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/Users/jon/esp/esp-idf-v5.0/components/bootloader/subproject"
  "/Users/jon/esp/esp-idf-v5.0/examples/protocols/http_server/simple/build/bootloader"
  "/Users/jon/esp/esp-idf-v5.0/examples/protocols/http_server/simple/build/bootloader-prefix"
  "/Users/jon/esp/esp-idf-v5.0/examples/protocols/http_server/simple/build/bootloader-prefix/tmp"
  "/Users/jon/esp/esp-idf-v5.0/examples/protocols/http_server/simple/build/bootloader-prefix/src/bootloader-stamp"
  "/Users/jon/esp/esp-idf-v5.0/examples/protocols/http_server/simple/build/bootloader-prefix/src"
  "/Users/jon/esp/esp-idf-v5.0/examples/protocols/http_server/simple/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/jon/esp/esp-idf-v5.0/examples/protocols/http_server/simple/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/jon/esp/esp-idf-v5.0/examples/protocols/http_server/simple/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
