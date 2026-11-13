# 曲线控件：环形数据缓冲 + 自绘坐标系，Qt 5.14.2 / Qt 6.x 通用
set(PLOT_SOURCES
    PlotTypes.h
    PlotDataBuffer.h
    PlotDataBuffer.cpp
    PlotWidget.h
    PlotWidget.cpp
)

if(QT_VERSION_MAJOR GREATER_EQUAL 6)
    qt_add_library(plot STATIC ${PLOT_SOURCES})
else()
    add_library(plot STATIC ${PLOT_SOURCES})
endif()

# 组件以源码形式被上层使用：头文件目录 PUBLIC 导出，使用方直接 #include "PlotWidget.h"
target_include_directories(plot PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(plot PUBLIC Qt${QT_VERSION_MAJOR}::Widgets)

if(MSVC)
    target_compile_options(plot PRIVATE /utf-8)
endif()
