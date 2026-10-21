cmake_minimum_required(VERSION 3.20)
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0 CACHE STRING "")
  project(Glass CXX OBJCXX)
else()
  project(Glass CXX RC)
endif()
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.92.9b GIT_SHALLOW TRUE)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_MakeAvailable(imgui json)
set(IMGUI_CORE ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp)

if(WIN32)
  # WebView2 : sert à faire tourner le lecteur web officiel de Spotify (invisible)
  FetchContent_Declare(webview2 URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.4258.31 DOWNLOAD_NAME webview2.zip)
  FetchContent_MakeAvailable(webview2)
  add_executable(Glass WIN32 main.cpp win.cpp glass.rc ${IMGUI_CORE}
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
  target_include_directories(Glass PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${webview2_SOURCE_DIR}/build/native/include)
  target_compile_options(Glass PRIVATE /utf-8 /Zi)
  target_link_options(Glass PRIVATE /DEBUG /OPT:REF /OPT:ICF)  # .pdb pour le rapport de plantage (crash.txt)
  target_compile_definitions(Glass PRIVATE UNICODE _UNICODE NOMINMAX)
  target_link_libraries(Glass PRIVATE nlohmann_json::nlohmann_json
    dbghelp d3d11 d3dcompiler dwmapi mfplat mfuuid wininet windowscodecs shlwapi propsys version
    ${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib)
else()
  # macOS : GLFW + OpenGL pour la fenêtre, AVFoundation pour le son, WKWebView pour le lecteur Spotify
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(glfw)
  # icônes (Material Icons, Apache 2.0) et police (Inter, OFL) livrées dans Glass.app
  set(RES ${CMAKE_BINARY_DIR}/res)
  file(DOWNLOAD https://raw.githubusercontent.com/google/material-design-icons/master/font/MaterialIcons-Regular.ttf ${RES}/MaterialIcons-Regular.ttf)
  FetchContent_Declare(inter URL https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip)
  FetchContent_MakeAvailable(inter)
  set(MAC_RES ${RES}/MaterialIcons-Regular.ttf ${inter_SOURCE_DIR}/extras/ttf/Inter-Regular.ttf ${inter_SOURCE_DIR}/extras/ttf/Inter-Bold.ttf
    ${CMAKE_SOURCE_DIR}/assets/glass-256.png ${CMAKE_SOURCE_DIR}/assets/Glass.icns)
  set_source_files_properties(${MAC_RES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  add_executable(Glass MACOSX_BUNDLE main.cpp mac.mm ${IMGUI_CORE} ${MAC_RES}
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
  set_target_properties(Glass PROPERTIES MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/assets/Info.plist)
  target_include_directories(Glass PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_compile_definitions(Glass PRIVATE GL_SILENCE_DEPRECATION)
  target_compile_options(Glass PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
  target_link_libraries(Glass PRIVATE nlohmann_json::nlohmann_json glfw curl
    "-framework Cocoa" "-framework OpenGL" "-framework AVFoundation" "-framework CoreMedia" "-framework WebKit"
    "-framework MediaPlayer" "-framework ImageIO" "-framework CoreGraphics" "-framework ServiceManagement" "-framework UniformTypeIdentifiers")
endif()
