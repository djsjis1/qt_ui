cmake_minimum_required(VERSION 3.21)

set(PROJECT_NAME "demo")

project(${PROJECT_NAME}
    VERSION 0.1.0
    DESCRIPTION "qt ${PROJECT_NAME}"
    LANGUAGES CXX C
)

# 组件库需要同时支持 Qt 5.14.2 与 Qt 6.x，统一使用 C++17：
# Qt 5.14 的头文件对 C++20/23 兼容不完整，提高标准会在 Qt5 构建时报错。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt 的元对象、界面文件、资源文件需要由 CMake 自动驱动 moc/uic/rcc
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# 优先使用 Qt 6，找不到时回落到 Qt 5（5.14.2 起），两者共用同一套源码
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

if(QT_VERSION_MAJOR GREATER_EQUAL 6)
    # Qt 6.3+ 的统一入口设置，Qt 5 没有这个命令
    qt_standard_project_setup()
endif()

# 可复用 UI 组件库：一个文件夹一个控件，新增组件照抄一行 add_subdirectory
add_subdirectory(ui-kit/plot)
add_subdirectory(ui-kit/param-table)

if(QT_VERSION_MAJOR GREATER_EQUAL 6)
    qt_add_executable(${PROJECT_NAME}
        main.cpp
        ui/mainwidget.h
        ui/mainwidget.cpp
    )
else()
    add_executable(${PROJECT_NAME}
        main.cpp
        ui/mainwidget.h
        ui/mainwidget.cpp
    )
endif()

# ui 目录加入头文件搜索路径，源文件里可以 #include "mainwidget.h"
target_include_directories(${PROJECT_NAME} PRIVATE ui)

# MSVC 默认按本地代码页解析源文件，显式指定 UTF-8 避免源码中的中文乱码
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE Qt${QT_VERSION_MAJOR}::Widgets plot param_table)

# 可执行文件统一输出到 bin 目录，方便运行脚本与手工查找
set_target_properties(${PROJECT_NAME} PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
