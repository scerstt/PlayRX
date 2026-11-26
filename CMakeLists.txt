cmake_minimum_required(VERSION 3.22)
project(playrx C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# Play RX builds on:
#   Windows  MSYS2 UCRT64 (README.md)                    PlayRX.exe, PlayRX_sim.exe, PlayLeagueCam.dll
#   macOS    Apple silicon, Homebrew (docs/INSTALL-MAC.md) "Play RX.app" (mac/build.sh does all of it)
#   Linux    Debian / Ubuntu / Raspberry Pi OS (docs/INSTALL-LINUX.md)   play-rx, headless (linux/install.sh)

set(COMPAT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/compat)  # <sys/resource.h> for kepyur's radio on Windows

# the release number shown in the window title and the UI: the VERSION file (CMake re-runs when it changes)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ARX_VERSION)
string(STRIP "${ARX_VERSION}" ARX_VERSION)

if(WIN32)
  # Keep <windows.h> (pulled in by libusb.h) from defining min/max and other noise.
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
  # The patched libusb (scripts/build-libusb.sh: WinUSB RAW_IO on request) ahead of MSYS2's, so devourer's
  # pkg_check_modules(libusb-1.0) finds it. Without it the build still works, but 40 MS/s does not stream.
  set(ARX_LIBUSB_DIR "C:/msys64/opt/arx-libusb" CACHE PATH "patched libusb install (scripts/build-libusb.sh)")
  if(EXISTS "${ARX_LIBUSB_DIR}/lib/pkgconfig/libusb-1.0.pc")
    set(ENV{PKG_CONFIG_PATH} "${ARX_LIBUSB_DIR}/lib/pkgconfig;$ENV{PKG_CONFIG_PATH}")
    set(ARX_LIBUSB_DLL "${ARX_LIBUSB_DIR}/bin/libusb-1.0.dll")
    message(STATUS "Play RX: patched libusb from ${ARX_LIBUSB_DIR}")
  else()
    get_filename_component(_mingw_bin_l "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(ARX_LIBUSB_DLL "${_mingw_bin_l}/libusb-1.0.dll")
    message(WARNING "Play RX: patched libusb not found (run scripts/build-libusb.sh): 40 MS/s will not stream")
  endif()
endif()

if(APPLE)
  enable_language(OBJCXX)
  # Homebrew's libraries (libusb, jpeg-turbo): Apple silicon /opt/homebrew, Intel /usr/local
  foreach(_brew /opt/homebrew /usr/local)
    if(EXISTS ${_brew}/lib/pkgconfig)
      set(ENV{PKG_CONFIG_PATH} "${_brew}/lib/pkgconfig:${_brew}/opt/jpeg-turbo/lib/pkgconfig:${_brew}/opt/libusb/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    endif()
  endforeach()
endif()

# The adapter's on-chip programs (wifi-sdr's src/fw/*.S, MIPS) come prebuilt in fw/prebuilt-fw, through fw/mips-stub.sh
# standing in for the mipsel binutils. -DARX_ASSEMBLE_FW=ON assembles them from source instead (needs
# mipsel-linux-gnu-as / ld / objcopy; pass MIPS_AS, MIPS_LD, MIPS_OBJCOPY if they are not on PATH).
option(ARX_ASSEMBLE_FW "assemble the adapter's on-chip programs with mipsel binutils instead of fw/prebuilt-fw" OFF)
if(NOT ARX_ASSEMBLE_FW)
  set(_stub ${CMAKE_CURRENT_SOURCE_DIR}/fw/mips-stub.sh)
  if(WIN32)
    find_program(ARX_SH sh REQUIRED)  # MSYS2's
    set(_mips_stub ${ARX_SH} ${_stub})
  else()
    execute_process(COMMAND chmod +x ${_stub})
    set(_mips_stub ${_stub})
  endif()
  set(MIPS_AS "${_mips_stub}" CACHE STRING "" FORCE)
  set(MIPS_LD "${_mips_stub}" CACHE STRING "" FORCE)
  set(MIPS_OBJCOPY "${_mips_stub}" CACHE STRING "" FORCE)
endif()

# kepyur's native code (with patches/ applied): the video decoder + scanner DSP (kepyur_dsp) and wifi-sdr +
# devourer + libusb (wsdrcore)
add_subdirectory(third_party/kepyur/native kepyur)
if(WIN32)
  foreach(t kepyur_radio kp_sim)
    if(TARGET ${t})
      target_include_directories(${t} BEFORE PRIVATE ${COMPAT_DIR})
    endif()
  endforeach()
  foreach(t kp_live kp_sim)
    if(TARGET ${t})
      target_compile_definitions(${t} PRIVATE SIGPIPE=SIGTERM)
    endif()
  endforeach()
endif()

# test signals for the simulated build (PlayRX_sim): rec/sim_*.cs16
add_executable(gen_testsignal tools/gen_testsignal.cpp)
target_compile_options(gen_testsignal PRIVATE -O2 -Wall)

if(NOT TARGET wsdrcore)
  message(FATAL_ERROR "wsdrcore missing: git submodule update --init --recursive, then apply patches/ (README.md)")
endif()

#   PlayRX      the app: real adapters
#   PlayRX_sim  the same with virtual adapters and transmitters (src/sim), for trying it without hardware
set(SIM_SOURCES src/sim/sim_receiver.cpp src/sim/sim_adapters.cpp)

# ---------------------------------------------------------------------------------------------------- Windows
if(WIN32)
  set(APP_COMMON src/app/main.cpp src/radio/radio.cpp src/radio/combine.cpp
                 src/feeds/feed_server.cpp src/timing/timing.cpp src/platform/win_libusb_env.cpp src/stream/live_out.cpp
                 src/stream/rtmp.cpp src/stream/live_encoder_win.cpp src/vcam/vcam_writer.cpp src/lowlat/lowlat_win.cpp src/capture/capture_win.cpp
                 src/capture/net_feed.cpp)
  set(APP_INCLUDES src/capture src/vcam src/lowlat src/stream src/platform src/radio src/feeds src/sim src/timing)
  # the window: WebView2 through webview (header-only, MSYS2 mingw-w64-ucrt-x86_64-webview)
  set(APP_LIBS kepyur_dsp wsdrcore user32 shell32 ws2_32 windowscodecs ole32 oleaut32 advapi32 shlwapi version bcrypt winmm
               mf mfplat mfreadwrite mfuuid uuid strmiids d3d11 dxgi setupapi cfgmgr32 powrprof)

  add_executable(PlayRX WIN32 ${APP_COMMON} src/radio/adapters.cpp)
  add_executable(PlayRX_sim WIN32 ${APP_COMMON} ${SIM_SOURCES})
  target_compile_definitions(PlayRX_sim PRIVATE ANALOGRX_SIM)
  set_source_files_properties(src/app/main.cpp PROPERTIES COMPILE_DEFINITIONS ARX_VERSION="${ARX_VERSION}")

  get_filename_component(_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
  foreach(t PlayRX PlayRX_sim)
    target_compile_definitions(${t} PRIVATE ARX_EDITION_RX)
    target_include_directories(${t} BEFORE PRIVATE ${COMPAT_DIR} ${APP_INCLUDES})
    target_link_libraries(${t} PRIVATE ${APP_LIBS})
    target_compile_options(${t} PRIVATE -O2 -Wall -Wno-format)  # %zu: MinGW false alarm (UCRT has it)
    target_link_options(${t} PRIVATE -static-libgcc -static-libstdc++)
    # the MinGW runtime DLLs next to the app, so it starts outside the MSYS2 shell
    foreach(dll "${ARX_LIBUSB_DLL}" "${_mingw_bin}/libwinpthread-1.dll")
      add_custom_command(TARGET ${t} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:${t}>")
    endforeach()
  endforeach()

  # the virtual cameras ("Play RX Pilot 1" ...): a DirectShow source DLL that FPV Trackside, OBS and others load,
  # fed from the app's shared memory (src/vcam/vcam_shared.h); self-contained (no MinGW runtime DLLs). The file keeps
  # its original name, PlayLeagueCam.dll, so cameras registered by earlier installs keep working.
  add_library(PlayLeagueCam SHARED src/vcam/vcam_filter.cpp)
  set_target_properties(PlayLeagueCam PROPERTIES PREFIX "" OUTPUT_NAME PlayLeagueCam)
  target_include_directories(PlayLeagueCam PRIVATE src/vcam)
  target_compile_options(PlayLeagueCam PRIVATE -O2 -Wall)
  target_link_libraries(PlayLeagueCam PRIVATE ole32 oleaut32 uuid strmiids gdi32 user32 advapi32)
  target_link_options(PlayLeagueCam PRIVATE -static)

# ---------------------------------------------------------------------------------------------------- macOS
elseif(APPLE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)
  set(MAC_COMMON src/app/main.cpp src/app/mac_window.mm src/radio/radio.cpp src/radio/combine.cpp
                 src/feeds/feed_server.cpp src/timing/timing.cpp src/stream/live_out.cpp src/stream/rtmp.cpp
                 src/stream/live_encoder_mac.mm src/capture/capture_stub.cpp src/capture/net_feed.cpp)
  set(MAC_INCLUDES src/capture src/stream src/platform src/app src/radio src/feeds src/sim src/timing third_party/webview)
  string(REGEX MATCH "^[0-9]+[.][0-9]+[.][0-9]+" ARX_VERSION_SHORT "${ARX_VERSION}")
  set_source_files_properties(src/app/main.cpp PROPERTIES COMPILE_DEFINITIONS ARX_VERSION="${ARX_VERSION}")
  set_source_files_properties(src/app/mac_window.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

  # target | app name | bundle id suffix | sources | definitions
  set(_mac_apps "PlayRX|Play RX|playrx|src/radio/adapters.cpp|ARX_EDITION_RX"
                "PlayRX_sim|Play RX Sim|playrx.sim|src/sim/sim_receiver.cpp,src/sim/sim_adapters.cpp|ARX_EDITION_RX,ANALOGRX_SIM")
  foreach(_app ${_mac_apps})
    string(REPLACE "|" ";" _f "${_app}")
    list(GET _f 0 t)
    list(GET _f 1 product)
    list(GET _f 2 idsuffix)
    list(GET _f 3 extra)
    list(GET _f 4 defs)
    string(REPLACE "," ";" extra "${extra}")
    string(REPLACE "," ";" defs "${defs}")
    add_executable(${t} MACOSX_BUNDLE ${MAC_COMMON} ${extra})
    target_compile_definitions(${t} PRIVATE ${defs})
    target_include_directories(${t} BEFORE PRIVATE ${MAC_INCLUDES})
    target_link_libraries(${t} PRIVATE kepyur_dsp wsdrcore PkgConfig::TURBOJPEG
                          "-framework Cocoa" "-framework WebKit" "-framework AVFoundation" "-framework CoreMedia"
                          "-framework CoreVideo" "-framework VideoToolbox" "-framework Foundation")
    target_compile_options(${t} PRIVATE -O2 -Wall -Wno-format)
    set_target_properties(${t} PROPERTIES
      OUTPUT_NAME "${product}"
      MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/mac/Info.plist.in
      MACOSX_BUNDLE_BUNDLE_NAME "${product}"
      MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.playrx.${idsuffix}"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${ARX_VERSION_SHORT}"  # macOS wants x.y.z here; the build number is in the bundle version
      MACOSX_BUNDLE_BUNDLE_VERSION "${ARX_VERSION}")
    # the web UI inside the app: Contents/Resources/web
    add_custom_command(TARGET ${t} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/web "$<TARGET_BUNDLE_CONTENT_DIR:${t}>/Resources/web")
  endforeach()

# ---------------------------------------------------------------------------------------------------- Linux
else()
  # headless: no window; the UI in a browser at http://<this-computer>:8090/
  find_package(PkgConfig REQUIRED)
  find_package(Threads REQUIRED)
  pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)
  set(LINUX_COMMON src/app/main.cpp src/radio/radio.cpp src/radio/combine.cpp src/feeds/feed_server.cpp
                   src/timing/timing.cpp src/stream/live_out.cpp src/capture/capture_stub.cpp src/capture/net_feed.cpp)
  set_source_files_properties(src/app/main.cpp PROPERTIES COMPILE_DEFINITIONS ARX_VERSION="${ARX_VERSION}")
  add_executable(PlayRX ${LINUX_COMMON} src/radio/adapters.cpp)
  add_executable(PlayRX_sim ${LINUX_COMMON} ${SIM_SOURCES})
  target_compile_definitions(PlayRX_sim PRIVATE ANALOGRX_SIM)
  set_target_properties(PlayRX PROPERTIES OUTPUT_NAME play-rx)
  set_target_properties(PlayRX_sim PROPERTIES OUTPUT_NAME play-rx-sim)
  foreach(t PlayRX PlayRX_sim)
    target_compile_definitions(${t} PRIVATE ARX_EDITION_RX)
    target_include_directories(${t} BEFORE PRIVATE src/capture src/stream src/platform src/app src/radio src/feeds src/sim src/timing)
    target_link_libraries(${t} PRIVATE kepyur_dsp wsdrcore PkgConfig::TURBOJPEG Threads::Threads)
    target_compile_options(${t} PRIVATE -O3 -Wall -Wno-format)  # -O3 like kepyur_dsp: the radio work is CPU-bound on a Pi
  endforeach()
endif()
