TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

INCLUDEPATH += $$(BOOSTHOME)
QMAKE_LIBDIR += $$(BOOSTLIBDIR)

unix:QMAKE_CXXFLAGS += -std=c++11

unix:LIBS += -lboost_system

SOURCES += main.cpp
