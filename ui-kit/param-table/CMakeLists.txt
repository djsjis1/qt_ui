# 参数表格控件：轮询触发 + 双击编辑，Qt 5.14.2 / Qt 6.x 通用
set(PARAM_TABLE_SOURCES
    ParamTableTypes.h
    ParamTableWidget.h
    ParamTableWidget.cpp
)

if(QT_VERSION_MAJOR GREATER_EQUAL 6)
    qt_add_library(param_table STATIC ${PARAM_TABLE_SOURCES})
else()
    add_library(param_table STATIC ${PARAM_TABLE_SOURCES})
endif()

# 组件以源码形式被上层使用：头文件目录 PUBLIC 导出，使用方直接 #include "ParamTableWidget.h"
target_include_directories(param_table PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(param_table PUBLIC Qt${QT_VERSION_MAJOR}::Widgets)

if(MSVC)
    target_compile_options(param_table PRIVATE /utf-8)
endif()
