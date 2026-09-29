QT += widgets
CONFIG += c++17
TEMPLATE = app
TARGET = cachyos-airpods-battery-tray
SOURCES += main.cpp
LIBS += -lbluetooth -pthread
