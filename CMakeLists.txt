cmake_minimum_required(VERSION 3.16)
project(evs-replay CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0)

# Ingest: empfängt die 6 SRT-Streams und schreibt Ring-Dateien + Index
add_executable(evs-ingest evs-ingest.cpp)
target_link_libraries(evs-ingest PRIVATE PkgConfig::GST Threads::Threads rt)

# Playout: PGM/PVW-Wiedergabe aus dem Ring
add_executable(evs-playout evs-playout.cpp)
target_link_libraries(evs-playout PRIVATE PkgConfig::GST Threads::Threads rt)

# Monitor (Qt6): nur gebaut, wenn Qt6 Widgets vorhanden ist
find_package(Qt6 COMPONENTS Widgets)
if(Qt6_FOUND)
  add_executable(evs-monitor evs-monitor.cpp)
  target_link_libraries(evs-monitor PRIVATE Qt6::Widgets PkgConfig::GST rt)
  # Qt-Schlüsselwörter (slots/signals/emit) abschalten: "slots" ist bei uns ein Feld im Frame-Index
  target_compile_definitions(evs-monitor PRIVATE QT_NO_KEYWORDS)

  # Clip- und Listenfenster (zweiter Bildschirm)
  add_executable(evs-control evs-control.cpp)
  target_link_libraries(evs-control PRIVATE Qt6::Widgets rt)
  target_compile_definitions(evs-control PRIVATE QT_NO_KEYWORDS)
else()
  message(WARNING "Qt6 Widgets nicht gefunden - evs-monitor wird nicht gebaut (sudo apt install qt6-base-dev)")
endif()
