
#1. 确定CMake的最低版本需求
CMAKE_MINIMUM_REQUIRED(VERSION 3.0.0)

#2. 确定工程名
PROJECT(TestUnits LANGUAGES CXX)
SET(CMAKE_CXX_STANDARD 17)

SET(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/TestUnits)

#7. 添加源码
file(GLOB SRCS *.cpp ./gtest/*.cc)

# 被测的存储层源码直接编进来。
# WtDataStorage 是 SHARED 目标且设了 CXX_VISIBILITY_PRESET hidden，
# 只导出 createDataReader 之类的工厂函数，WtDataWriter 这样的内部类
# 无法从外部链接，所以这里把源文件纳入本工程一起编译。
LIST(APPEND SRCS ../WtDataStorage/WtDataWriter.cpp)

SET(LIBS
    WTSTools
	WTSUtils
    WtShareHelper)
IF (MSVC)
ELSE(GNUCC)
    # WtDataWriter 依赖 boost_filesystem
    LIST(APPEND LIBS boost_filesystem)
ENDIF()
IF (MSVC)
ELSE(GNUCC)
    LIST(APPEND LIBS pthread stdc++fs)
    FIX_PLATFORM_LIBS(LIBS)
	IF(WIN32)
		LIST(APPEND LIBS iconv)
	ENDIF()
ENDIF()

LIST(APPEND INCS "./gtest")

INCLUDE_DIRECTORIES(${INCS})
LINK_DIRECTORIES(${LNKS})

ADD_EXECUTABLE(TestUnits ${SRCS})
TARGET_LINK_LIBRARIES(TestUnits ${LIBS})

IF (MSVC)
ELSE (GNUCC)
	SET_STRIP_PROPS(TestUnits)
ENDIF ()