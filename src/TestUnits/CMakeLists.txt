
#1. 确定CMake的最低版本需求
CMAKE_MINIMUM_REQUIRED(VERSION 3.0.0)

#2. 确定工程名
PROJECT(TestUnits LANGUAGES CXX)
SET(CMAKE_CXX_STANDARD 17)

SET(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/TestUnits)

#7. 添加源码
file(GLOB SRCS *.cpp ./gtest/*.cc)

# 存储层不能把源码编进来：WtDtMgr 内部是 dlopen 加载 WtDataStorage 的，
# 可执行文件里若也有同一份源码，同一个类就有两份实现和两份vtable，
# dyld 解析弱符号时会让 dylib 里的对象指向错误的vtable(ODR违规)，
# Release 下表现为随机的 SIGBUS。
# 测试统一通过 storage_loader.hpp 走 dlopen + 导出的工厂函数。

# 条件单结构体的等价判定是纯头文件逻辑，直接测
INCLUDE_DIRECTORIES(../WtCore)

SET(LIBS
    WTSTools
	WTSUtils
    WtShareHelper
    # 秒级调度的集成测试要拉起真实的 WtCtaEngine，WtCore 是静态库可以直接链接
    WtCore)
IF (MSVC)
ELSE(GNUCC)
    # WtDataWriter 依赖 boost_filesystem；
    # WtCore 的线程池(boost::threadpool)依赖 boost_thread，参照 WtRunner 的链接配置
    LIST(APPEND LIBS boost_filesystem boost_thread)
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