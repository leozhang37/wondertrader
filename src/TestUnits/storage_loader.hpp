/*!
 * \file storage_loader.hpp
 * \brief 通过 dlopen 加载 WtDataStorage 插件
 *
 * 存储层的测试必须走这条路，不能把 WtDataWriter.cpp / WtDataReader.cpp
 * 直接编进 TestUnits：
 *   WtDtMgr 内部是用 dlopen + createDataReader 拿 reader 的，
 *   如果可执行文件里也链接了同一份源码，同一个类就有两份实现和两份vtable。
 *   dyld 解析弱符号(vtable)时可能让 dylib 里 new 出来的对象
 *   指向可执行文件里的vtable，虚调用直接跳到错误地址。
 *   这类 ODR 违规在 Release 下表现为随机的 SIGBUS/SIGSEGV，很难排查。
 *
 * 而且这两个类都导出了工厂函数、测试只用到接口方法，
 * 走接口反而更接近真实的部署形态。
 */
#pragma once
#include <string>

//DataDefine.h 只有块结构体的定义，没有实现代码，直接include不涉及ODR
#include "../WtDataStorage/DataDefine.h"
#include "../Includes/IDataReader.h"
#include "../Includes/IDataWriter.h"
#include "../Share/DLLHelper.hpp"
#include "../Share/StrUtil.hpp"

USING_NS_WTP;

namespace storage_loader
{
	//测试用的库目录由环境变量给出，方便在不同构建目录间切换
	inline std::string lib_dir()
	{
		const char* d = getenv("WT_TEST_LIBDIR");
		if (d == NULL || strlen(d) == 0)
			return "";
		return StrUtil::standardisePath(std::string(d));
	}

	inline DllHandle load()
	{
		static DllHandle s_handle = NULL;
		if (s_handle != NULL)
			return s_handle;

		std::string path = lib_dir() + DLLHelper::wrap_module("WtDataStorage");
		s_handle = DLLHelper::load_library(path.c_str());
		if (s_handle == NULL)
			printf("[loader] failed to load %s\n", path.c_str());
		return s_handle;
	}

	inline IDataWriter* make_writer()
	{
		DllHandle h = load();
		if (h == NULL) return NULL;

		typedef IDataWriter* (*FuncCreateWriter)();
		FuncCreateWriter f = (FuncCreateWriter)DLLHelper::get_symbol(h, "createWriter");
		if (f == NULL)
		{
			printf("[loader] createWriter not found\n");
			return NULL;
		}
		return f();
	}

	inline void free_writer(IDataWriter* w)
	{
		if (w == NULL) return;
		DllHandle h = load();
		typedef void (*FuncDeleteWriter)(IDataWriter*&);
		FuncDeleteWriter f = (FuncDeleteWriter)DLLHelper::get_symbol(h, "deleteWriter");
		if (f != NULL)
			f(w);
	}

	inline IDataReader* make_reader()
	{
		DllHandle h = load();
		if (h == NULL) return NULL;

		FuncCreateDataReader f = (FuncCreateDataReader)DLLHelper::get_symbol(h, "createDataReader");
		if (f == NULL)
		{
			printf("[loader] createDataReader not found\n");
			return NULL;
		}
		return f();
	}

	inline void free_reader(IDataReader* r)
	{
		if (r == NULL) return;
		DllHandle h = load();
		FuncDeleteDataReader f = (FuncDeleteDataReader)DLLHelper::get_symbol(h, "deleteDataReader");
		if (f != NULL)
			f(r);
	}
}
