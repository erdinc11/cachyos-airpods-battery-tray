QT += widgets
CONFIG += c++17
TEMPLATE = app
TARGET = podpower-tray
SOURCES += main.cpp
LIBS += -lbluetooth -pthread
