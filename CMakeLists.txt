# 需要的最低 CMake 版本（3.20 起支援這裡用到的所有指令）
cmake_minimum_required(VERSION 3.20)
# 專案名稱；LANGUAGES CXX = 只用 C++（不檢查 C 編譯器）
project(iProvision LANGUAGES CXX)

# 使用 C++17（std::filesystem、結構化綁定 auto [a, b]、inline constexpr 變數）
set(CMAKE_CXX_STANDARD 17)
# 編譯器不支援 C++17 就直接報錯，而不是退回舊標準
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 沒有指定建置類型（cmake -DCMAKE_BUILD_TYPE=Debug）的話
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    # 預設用 Release（有最佳化）
    set(CMAKE_BUILD_TYPE Release)
# 結束 if
endif()

# 架構：
#   iprovision（管理器）--Boost.Process--> so_runner <lib.so> [args]   x n 個子行程
#   so_runner 用 dlopen 載入 .so，呼叫 plugin_main(argc, argv)（見 include/plugin_api.hpp）
#   libisurgical_plugin.so：把 lib/libisurgical.so 包裝成有 plugin_main 的外掛
#   libtest_plugin.so：測試用外掛
#
# 這個檔案只負責「編譯和連結」，不會把任何值傳進原始碼（沒有 target_compile_definitions）。
# 程式裡用到的路徑和設定都寫在 src/app_config.hpp 等檔案的 constexpr / #define。

# 所有執行檔放在 build/ 最上層：管理器靠「so_runner 和自己在同一個資料夾」來找到它
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
# 所有 .so 也放在 build/ 最上層：configs/processes.yaml 用 ../build/xxx.so 指向它們
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ---------------------------------------------------------------------------
# 外部套件
# ---------------------------------------------------------------------------
# Boost.Process v2 + Asio：只用標頭檔，不需要連結 Boost 的函式庫
find_package(Boost 1.80 REQUIRED)
# pthread：Asio 內部會用到執行緒
find_package(Threads REQUIRED)
# yaml-cpp：讀取 configs/processes.yaml
find_package(yaml-cpp REQUIRED)
# OpenCV：isurgical.hpp 會 include OpenCV 的標頭，所以編譯 isurgical 外掛時需要
find_package(OpenCV REQUIRED COMPONENTS core imgproc)
# Google glog：所有 log 都經過它（sudo apt install libgoogle-glog-dev）
find_package(glog 0.6 REQUIRED)

# ---------------------------------------------------------------------------
# 預先編譯好的 libisurgical（放在 lib/）
# ---------------------------------------------------------------------------
# libisurgical 所在的資料夾
set(ISURGICAL_LIB_DIR   ${PROJECT_SOURCE_DIR}/lib)
# 完整版本號（檔名 libisurgical.so.0.1.0）
set(ISURGICAL_VERSION   0.1.0)
# 主版本號（SONAME 是 libisurgical.so.0）
set(ISURGICAL_SOVERSION 0)
# 真正的檔案
set(ISURGICAL_REAL      ${ISURGICAL_LIB_DIR}/libisurgical.so.${ISURGICAL_VERSION})
# SONAME：連結後，程式在執行時會用這個名字找它
set(ISURGICAL_SONAME    libisurgical.so.${ISURGICAL_SOVERSION})
# 檔案不存在
if(NOT EXISTS ${ISURGICAL_REAL})
    # 直接停止並說明原因
    message(FATAL_ERROR "libisurgical not found: ${ISURGICAL_REAL}")
# 結束 if
endif()
# 每次 configure 都重建 symlink：libisurgical.so.0 → libisurgical.so.0.1.0（執行時需要）
file(CREATE_LINK libisurgical.so.${ISURGICAL_VERSION} ${ISURGICAL_LIB_DIR}/${ISURGICAL_SONAME} SYMBOLIC)
# libisurgical.so → libisurgical.so.0（連結時需要）
file(CREATE_LINK ${ISURGICAL_SONAME} ${ISURGICAL_LIB_DIR}/libisurgical.so SYMBOLIC)

# 宣告一個「已經編譯好」的共享函式庫 target，叫 isurgical
add_library(isurgical SHARED IMPORTED)
# 設定這個 target 的屬性
set_target_properties(isurgical PROPERTIES
    # 實際檔案位置
    IMPORTED_LOCATION             ${ISURGICAL_REAL}
    # 它的 SONAME
    IMPORTED_SONAME               ${ISURGICAL_SONAME}
    # 連結它的 target 自動加上這些 include 路徑：include/（isurgical.hpp）和 OpenCV
    INTERFACE_INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/include;${OpenCV_INCLUDE_DIRS}"
    # 連結它的 target 自動連結 OpenCV
    INTERFACE_LINK_LIBRARIES      "${OpenCV_LIBS}"
# 結束 set_target_properties
)

# ---------------------------------------------------------------------------
# GPU 執行期函式庫路徑（GTX 1080 Ti / Pascal, sm_61）
# ---------------------------------------------------------------------------
# ONNX Runtime 會 dlopen libonnxruntime_providers_cuda.so，只有「執行檔」的 DT_RPATH
# 能影響它載入哪個 cuDNN。系統的 cuDNN 9.26 沒有 Pascal kernel，必須用 cuDNN 9.5.1，
# 所以 so_runner 的 RPATH 要包含這些路徑。詳見 README.md 的「GPU 與 cuDNN」。
# ONNX 相關檔案的根目錄
set(ONNX_ROOT /media/andyhsueh/Storage2/sideproject/Onnx)
# ONNX Runtime 的 lib 資料夾；CACHE 代表可以用 cmake -DONNXRUNTIME_LIB_DIR=... 覆蓋
set(ONNXRUNTIME_LIB_DIR ${ONNX_ROOT}/onnxruntime-linux-x64-gpu-1.20.1/lib
    CACHE PATH "ONNX Runtime GPU lib dir (CUDA 12 build)")
# 支援 Pascal 的 cuDNN 的 lib 資料夾；同樣可以覆蓋
set(CUDNN_LIB_DIR ${ONNX_ROOT}/cudnn-9.5.1/lib
    CACHE PATH "Pascal-capable cuDNN lib dir")
# 檢查這兩個資料夾
foreach(dir ONNXRUNTIME_LIB_DIR CUDNN_LIB_DIR)
    # 不存在
    if(NOT EXISTS ${${dir}})
        # 只警告（還是可以編譯，但 GPU 推論會退回 CPU）
        message(WARNING "${dir} does not exist: ${${dir}} - GPU inference will fall back to CPU")
    # 結束 if
    endif()
# 結束 foreach
endforeach()

# ---------------------------------------------------------------------------
# app_log：glog + configs/log_messages.yaml 的 log 函式庫（共享函式庫）
# ---------------------------------------------------------------------------
# 編譯成 build/libapp_log.so。必須是「共享」函式庫：so_runner 和它 dlopen 的外掛
# 會用到同一份（同一個訊息表、同一個 glog），外掛不用自己初始化 log
add_library(app_log SHARED src/log/app_log.cpp)
# include/app_log.hpp；PUBLIC = 連結 app_log 的 target 也自動拿到這個 include 路徑
target_include_directories(app_log PUBLIC ${PROJECT_SOURCE_DIR}/include)
# glog（寫 log）、yaml-cpp（讀訊息表）只在 app_log 內部使用
target_link_libraries(app_log PRIVATE glog::glog yaml-cpp::yaml-cpp)
# 開啟大部分警告
target_compile_options(app_log PRIVATE -Wall -Wextra)
# 符號要對外公開（外掛呼叫 app_log::Write），不受下面外掛的 hidden 設定影響
set_target_properties(app_log PROPERTIES CXX_VISIBILITY_PRESET default)

# ---------------------------------------------------------------------------
# so_runner：子行程的宿主，載入 .so 並呼叫 plugin_main
# ---------------------------------------------------------------------------
# 編譯成執行檔 build/so_runner
# app_config.cpp：用和管理器一樣的方式找到 configs/log_messages.yaml
add_executable(so_runner src/runner/so_runner.cpp src/app_config.cpp)
# include/plugin_api.hpp；src/（app_config.hpp）
target_include_directories(so_runner PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
# dlopen / dlsym 所在的函式庫（Linux 上是 libdl）；app_log（log）
target_link_libraries(so_runner PRIVATE ${CMAKE_DL_LIBS} app_log)
# 開啟大部分警告
target_compile_options(so_runner PRIVATE -Wall -Wextra)
# 把函式庫的搜尋路徑寫進執行檔（RPATH），執行時不需要設定 LD_LIBRARY_PATH。
# 順序很重要：cuDNN 9.5.1 要排在系統路徑之前
set_target_properties(so_runner PROPERTIES
    # 寫入 lib/、cuDNN 9.5.1、ONNX Runtime 三個資料夾
    BUILD_RPATH "${ISURGICAL_LIB_DIR};${CUDNN_LIB_DIR};${ONNXRUNTIME_LIB_DIR}")
# 用 DT_RPATH 而不是較新的 DT_RUNPATH：DT_RPATH 也會套用到 dlopen 載入的 .so 及它們的依賴
target_link_options(so_runner PRIVATE -Wl,--disable-new-dtags)

# ---------------------------------------------------------------------------
# 外掛（.so）
# ---------------------------------------------------------------------------
# 外掛預設隱藏所有符號，只有標了 PLUGIN_EXPORT 的 plugin_main 對外公開
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# isurgical 外掛：編譯成 build/libisurgical_plugin.so
add_library(isurgical_plugin SHARED plugins/isurgical_plugin.cpp)
# 連結 libisurgical（會自動帶入 include/、OpenCV）；app_log（log）
target_link_libraries(isurgical_plugin PRIVATE isurgical app_log)
# 開啟大部分警告
target_compile_options(isurgical_plugin PRIVATE -Wall -Wextra)
# 執行時從 lib/ 找 libisurgical.so.0
set_target_properties(isurgical_plugin PROPERTIES BUILD_RPATH ${ISURGICAL_LIB_DIR})
# 同樣用 DT_RPATH
target_link_options(isurgical_plugin PRIVATE -Wl,--disable-new-dtags)

# test 外掛：編譯成 build/libtest_plugin.so（測試用，沒有其他依賴）
add_library(test_plugin SHARED plugins/test_plugin.cpp)
# include/plugin_api.hpp
target_include_directories(test_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
# app_log（log）
target_link_libraries(test_plugin PRIVATE app_log)
# 開啟大部分警告
target_compile_options(test_plugin PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------------------
# iprovision：管理器（Boost.Process）
# ---------------------------------------------------------------------------
# 編譯成執行檔 build/iprovision，以及它的所有原始碼
add_executable(iprovision
    # 進入點
    src/main.cpp
    # 固定設定與路徑
    src/app_config.cpp
    # 命令列介面
    src/console/console.cpp
    # Ctrl+C 處理
    src/console/shutdown_signal.cpp
    # 切字、時間格式、打錯字提示
    src/console/text_utils.cpp
    # 管理多個子行程
    src/manager/process_manager.cpp
    # 一個子行程
    src/manager/managed_process.cpp
    # 讀取 processes.yaml
    src/manager/program_config.cpp
# 結束 add_executable
)
# 讓原始碼可以寫 #include "manager/process_manager.hpp"（以 src/ 為基準）
target_include_directories(iprovision PRIVATE ${PROJECT_SOURCE_DIR}/src)
# Boost 標頭、pthread、yaml-cpp、app_log（log）
target_link_libraries(iprovision PRIVATE Boost::headers Threads::Threads yaml-cpp::yaml-cpp app_log)
# 開啟大部分警告
target_compile_options(iprovision PRIVATE -Wall -Wextra)
# 編譯管理器時，一併編譯 so_runner 和外掛（管理器執行時需要它們）
add_dependencies(iprovision so_runner isurgical_plugin test_plugin)
