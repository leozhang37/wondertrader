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
#include <map>

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

	inline DllHandle load(const char* module = "WtDataStorage")
	{
		//同一个模块只加载一次
		static std::map<std::string, DllHandle> s_handles;
		auto it = s_handles.find(module);
		if (it != s_handles.end())
			return it->second;

		std::string path = lib_dir() + DLLHelper::wrap_module(module);
		DllHandle h = DLLHelper::load_library(path.c_str());
		if (h == NULL)
			printf("[loader] failed to load %s\n", path.c_str());
		s_handles[module] = h;
		return h;
	}

	//AD(LMDB)存储引擎
	inline IDataWriter* make_ad_writer()
	{
		DllHandle h = load("WtDataStorageAD");
		if (h == NULL) return NULL;
		typedef IDataWriter* (*FuncCreateWriter)();
		FuncCreateWriter f = (FuncCreateWriter)DLLHelper::get_symbol(h, "createWriter");
		return (f != NULL) ? f() : NULL;
	}

	inline IDataReader* make_ad_reader()
	{
		DllHandle h = load("WtDataStorageAD");
		if (h == NULL) return NULL;
		FuncCreateDataReader f = (FuncCreateDataReader)DLLHelper::get_symbol(h, "createDataReader");
		return (f != NULL) ? f() : NULL;
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
