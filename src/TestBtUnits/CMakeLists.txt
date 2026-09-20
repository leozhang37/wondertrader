
#1. 确定CMake的最低版本需求
CMAKE_MINIMUM_REQUIRED(VERSION 3.0.0)

#2. 确定工程名
PROJECT(TestBtUnits LANGUAGES CXX)
SET(CMAKE_CXX_STANDARD 17)

SET(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/TestBtUnits)

# 回测的单测必须和实盘的单测分开成两个可执行文件：
# WtCore 和 WtBtCore 各自带了一份 WtHelper 和 EventNotifier，
# 同时链接会有一堆 duplicate symbol。
# 这也符合实际形态——WtRunner 和 WtBtRunner 本来就是两个独立程序。
file(GLOB SRCS *.cpp ../TestUnits/gtest/*.cc)

SET(LIBS
    WtBtCore
    WTSTools
    WTSUtils)

IF (MSVC)
ELSE(GNUCC)
    LIST(APPEND LIBS dl pthread boost_filesystem boost_thread stdc++fs)
    FIX_PLATFORM_LIBS(LIBS)
    IF(WIN32)
        LIST(APPEND LIBS iconv)
    ENDIF()
ENDIF()

# gtest 需要两层include路径配合：
#   我们的源文件写 #include "gtest/gtest/gtest.h" -> 需要 ../TestUnits
#   gtest.h 内部写 #include "gtest/internal/..."  -> 需要 ../TestUnits/gtest
LIST(APPEND INCS "../TestUnits")
LIST(APPEND INCS "../TestUnits/gtest")

INCLUDE_DIRECTORIES(${INCS})
LINK_DIRECTORIES(${LNKS})

ADD_EXECUTABLE(TestBtUnits ${SRCS})
TARGET_LINK_LIBRARIES(TestBtUnits ${LIBS})

IF (MSVC)
ELSE (GNUCC)
	SET_STRIP_PROPS(TestBtUnits)
ENDIF ()
