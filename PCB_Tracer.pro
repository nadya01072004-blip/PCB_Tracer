# PCB_Tracer.pro
QT += core gui concurrent widgets
CONFIG += c++11

TARGET = PCB_Tracer
TEMPLATE = app

DEFINES += QT_DEPRECATED_WARNINGS

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    trace.cpp \
    helpdialog.cpp \
    multi_trace.cpp  # Только этот файл, fixes.cpp удален

HEADERS += \
    mainwindow.h \
    trace.h \
    helpdialog.h \
    multi_trace.h

FORMS += \
    mainwindow.ui \
    helpdialog.ui
