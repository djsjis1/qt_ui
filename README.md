# qt_ui

Qt Widgets 可复用 UI 组件库，Qt 5.14.2 与 Qt 6.x 共用同一份源码。

一个文件夹 = 一个可以独立搬运的控件，约定见 [ui-kit/README.md](ui-kit/README.md)。

## 组件

| 组件 | 说明 |
| --- | --- |
| `ui-kit/plot` | 实时曲线控件：每通道环形缓冲 + 按像素列 min/max 聚合 + 自绘坐标轴；只提供接口，缩放/平移/框选/游标由调用者驱动 |
| `ui-kit/param-table` | 参数矩阵表格：行=参数、列=数据源，列可显隐，双击编辑并校验范围，读写底层由调用者实现 |

## 构建

```powershell
cmake -B build -DCMAKE_PREFIX_PATH="<Qt 安装路径>"
cmake --build build --config Debug
```
