CMAKE_MINIMUM_REQUIRED(VERSION 3.0.0)

PROJECT(WonderTrader)

#3. 环境判断
IF (WIN32)
	MESSAGE(STATUS "Operation System is windows")
ELSEIF (UNIX)
	MESSAGE(STATUS "Operation System is UNIX-like OS's")
ENDIF ()

MESSAGE(STATUS "Generator is ${CMAKE_GENERATOR}")

IF (MSVC)
	ADD_DEFINITIONS (-D_CRT_SECURE_NO_WARNINGS)
	IF(CMAKE_CL_64)
		SET(INCS $ENV{MyDepends141}/include)
		SET(LNKS $ENV{MyDepends141}/lib/x64)

		MESSAGE(STATUS "MyDepends is at $ENV{MyDepends141}")
		MESSAGE(STATUS "Platform is x64")
		SET(PLATFORM x64)
	ELSE()
		SET(INCS $ENV{MyDepends141}/include)
		SET(LNKS $ENV{MyDepends141}/lib/x86)

		MESSAGE(STATUS "MyDepends is at $ENV{MyDepends141}")
		MESSAGE(STATUS "Platform is x86")
		SET(PLATFORM x86)
	ENDIF()
	SET(PREFIX )
	SET(SUFFIX .dll)
ELSE()
	IF(WIN32)
		SET (MyDeps $ENV{MyDependsGcc})
		SET(SUFFIX .dll)

		MESSAGE(STATUS "MyDepends is at $ENV{MyDependsGcc}")
	ELSEIF(APPLE)
		SET(SUFFIX .dylib)

		IF(DEFINED ENV{MyDeps})
			SET(MyDeps $ENV{MyDeps})
		ELSE()
			EXECUTE_PROCESS(COMMAND brew --prefix OUTPUT_VARIABLE BREW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
			SET(MyDeps ${BREW_PREFIX})
		ENDIF()

		MESSAGE(STATUS "MyDepends is at ${MyDeps}")

		# macOS RPATH settings
		SET(CMAKE_MACOSX_RPATH ON)
		SET(CMAKE_INSTALL_RPATH "@executable_path;@loader_path")
		SET(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
	ELSE(UNIX)
		SET (MyDeps "/home/mydeps")
		SET(SUFFIX .so)

		MESSAGE(STATUS "MyDepends is at /home/mydeps")
	ENDIF()

	SET(INCS ${MyDeps}/include)
	SET(LNKS ${MyDeps}/lib)

	MESSAGE(STATUS "Platform is x64")
	SET(PLATFORM x64)
	SET(PREFIX lib)
	# GCC/Clang特定选项
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
    IF(CMAKE_BUILD_TYPE STREQUAL "Release")
        SET(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
    ENDIF()
ENDIF()

SET(CMAKE_CXX_STANDARD 17)

# Include platform helper macros
INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/PlatformHelper.cmake)



#basic libraries
ADD_SUBDIRECTORY(WTSUtils)
ADD_SUBDIRECTORY(WTSTools)

#contract Loader through ctp channel (requires private SDK)
ADD_SUBDIRECTORY(CTPLoader)
IF(NOT APPLE)
	
	ADD_SUBDIRECTORY(CTPOptLoader)
ENDIF()
ADD_SUBDIRECTORY(LoaderRunner)

#parser for quote data
ADD_SUBDIRECTORY(ParserCTP)
IF(NOT APPLE)
	ADD_SUBDIRECTORY(ParserCTPMini)
	ADD_SUBDIRECTORY(ParserCTPOpt)
	ADD_SUBDIRECTORY(ParserFemas)
	ADD_SUBDIRECTORY(ParserXTP)
ENDIF()
ADD_SUBDIRECTORY(ParserShm)
ADD_SUBDIRECTORY(ParserXeleSkt)
ADD_SUBDIRECTORY(WtDataStorage)
ADD_SUBDIRECTORY(WtDataStorageAD)
ADD_SUBDIRECTORY(WtIndexFactory)
ADD_SUBDIRECTORY(WtDtCore)
ADD_SUBDIRECTORY(WtDtHelper)
ADD_SUBDIRECTORY(WtDtPorter)
ADD_SUBDIRECTORY(WtDtServo)
#quote date processor
ADD_SUBDIRECTORY(QuoteFactory)

#back test core library
ADD_SUBDIRECTORY(WtBtCore)
#back test c porter for outter libraries
ADD_SUBDIRECTORY(WtBtPorter)
ADD_SUBDIRECTORY(WtBtRunner)

ADD_SUBDIRECTORY(ParserUDP)
ADD_SUBDIRECTORY(TraderCTP)
IF(NOT APPLE)
	
	ADD_SUBDIRECTORY(TraderCTPMini)
	ADD_SUBDIRECTORY(TraderCTPOpt)
	ADD_SUBDIRECTORY(TraderFemas)
ENDIF()
ADD_SUBDIRECTORY(TraderMocker)
IF(NOT APPLE)
	ADD_SUBDIRECTORY(TraderXTP)
	ADD_SUBDIRECTORY(TraderYD)
ENDIF()
ADD_SUBDIRECTORY(WtExeFact)
ADD_SUBDIRECTORY(WtRiskMonFact)

ADD_SUBDIRECTORY(WtMsgQue)
ADD_SUBDIRECTORY(WtCore)
ADD_SUBDIRECTORY(WtPorter)
ADD_SUBDIRECTORY(WtExecMon)
ADD_SUBDIRECTORY(WtRunner)
ADD_SUBDIRECTORY(WtCtaStraFact)

ADD_SUBDIRECTORY(TraderDumper)

ADD_SUBDIRECTORY(WtShareHelper)

#uft
ADD_SUBDIRECTORY(WtUftCore)
ADD_SUBDIRECTORY(WtUftStraFact)
ADD_SUBDIRECTORY(WtUftRunner)

ADD_SUBDIRECTORY(WtLatencyHFT)
ADD_SUBDIRECTORY(WtLatencyUFT)

#test projects
ADD_SUBDIRECTORY(TestBtPorter)
ADD_SUBDIRECTORY(TestDtPorter)
ADD_SUBDIRECTORY(TestExecPorter)
ADD_SUBDIRECTORY(TestPorter)
ADD_SUBDIRECTORY(TestTrader)
ADD_SUBDIRECTORY(TestParser)
ADD_SUBDIRECTORY(TestUnits)

