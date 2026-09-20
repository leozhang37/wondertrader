#include "WtBtDtReaderAD.h"
#include "LMDBKeys.h"

#include "../Includes/WTSStruct.h"
#include "../Includes/WTSVariant.hpp"
#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"

USING_NS_WTP;

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
template<typename... Args>
inline void pipe_btreader_log(IBtDtReaderSink* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	static thread_local char buffer[512] = { 0 };
	memset(buffer, 0, 512);
	fmt::format_to(buffer, format, args...);

	sink->reader_log(ll, buffer);
}

extern "C"
{
	EXPORT_FLAG IBtDtReader* createBtDtReader()
	{
		IBtDtReader* ret = new WtBtDtReaderAD();
		return ret;
	}

	EXPORT_FLAG void deleteBtDtReader(IBtDtReader* reader)
	{
		if (reader != NULL)
			delete reader;
	}
};

WtBtDtReaderAD::WtBtDtReaderAD()
{
}


WtBtDtReaderAD::~WtBtDtReaderAD()
{

}

void WtBtDtReaderAD::init(WTSVariant* cfg, IBtDtReaderSink* sink)
{
	_sink = sink;

	if (cfg == NULL)
		return;

	_base_dir = cfg->getCString("path");
	_base_dir = StrUtil::standardisePath(_base_dir);

	pipe_btreader_log(_sink, LL_INFO, "WtBtDtReaderAD initialized, root data dir is {}", _base_dir);
}

bool WtBtDtReaderAD::read_raw_bars(const char* exchg, const char* code, WTSKlinePeriod period, std::string& buffer)
{
	//直接从LMDB读取
	WtLMDBPtr db = get_k_db(exchg, period);
	if (db == NULL)
		return false;

	pipe_btreader_log(_sink, LL_DEBUG, "Reading back {} bars of {}.{}...", PERIOD_NAME[period], exchg, code);
	WtLMDBQuery query(*db);
	LMDBBarKey rKey(exchg, code, 0xffffffff);
	LMDBBarKey lKey(exchg, code, 0);
	int cnt = query.get_range(std::string((const char*)&lKey, sizeof(lKey)), std::string((const char*)&rKey, sizeof(rKey)),
		[this, &buffer, &lKey](const ValueArray& ayKeys, const ValueArray& ayVals) {
		if (ayVals.empty())
			return;

		std::size_t cnt = ayVals.size();
		auto szUnit = sizeof(WTSBarStruct);
		buffer.resize(szUnit*cnt);
		char* cursor = (char*)buffer.data();
		for(const std::string& item : ayVals)
		{
			memcpy(cursor, item.data(), szUnit);
			cursor += szUnit;
		}
	});

	return true;
}

bool WtBtDtReaderAD::read_raw_ticks(const char* exchg, const char* code, uint32_t uDate, std::string& buffer)
{
	//直接从LMDB读取
	WtLMDBPtr db = get_t_db(exchg, code);
	if (db == NULL)
		return false;

	pipe_btreader_log(_sink, LL_DEBUG, "Reading back ticks on {} of {}.{}...", uDate, exchg, code);
	WtLMDBQuery query(*db);
	LMDBHftKey rKey(exchg, code, uDate, 240000000);
	LMDBHftKey lKey(exchg, code, uDate, 0);
	int cnt = query.get_range(std::string((const char*)&lKey, sizeof(lKey)), std::string((const char*)&rKey, sizeof(rKey)),
		[this, &buffer, &lKey](const ValueArray& ayKeys, const ValueArray& ayVals) {
		if (ayVals.empty())
			return;

		std::size_t cnt = ayVals.size();
		auto szUnit = sizeof(WTSTickStruct);
		buffer.resize(szUnit*cnt);
		char* cursor = (char*)buffer.data();
		for (const std::string& item : ayVals)
		{
			memcpy(cursor, item.data(), szUnit);
			cursor += szUnit;
		}
	});

	return true;
}

WtBtDtReaderAD::WtLMDBPtr WtBtDtReaderAD::get_k_db(const char* exchg, WTSKlinePeriod period)
{
	WtLMDBMap* the_map = NULL;
	std::string subdir;
	if (period == KP_Minute1)
	{
		the_map = &_exchg_m1_dbs;
		subdir = "min1";
	}
	else if (period == KP_Minute5)
	{
		the_map = &_exchg_m5_dbs;
		subdir = "min5";
	}
	else if (period == KP_DAY)
	{
		the_map = &_exchg_d1_dbs;
		subdir = "day";
	}
	else
		return std::move(WtLMDBPtr());

	auto it = the_map->find(exchg);
	if (it != the_map->end())
		/*
		 *	By 秒K线支持 @ 2026.09.20
		 *	这里原先是 return std::move(it->second)，
		 *	把 map 里存的 shared_ptr 直接移走了：第2次调用取到值的同时
		 *	把容器里的置空，第3次及以后 find 命中但拿到的是空指针，
		 *	调用方 if(db) 为假就静默跳过——既不写库也不报错。
		 *	实际表现是 AD 存储的K线从第3根开始全部丢失。
		 *	shared_ptr 拷贝只是加一次引用计数，这里不该 move
		 */
		return it->second;

	WtLMDBPtr dbPtr(new WtLMDB(true));
	std::string path = fmtutil::format("{}{}/{}/", _base_dir.c_str(), subdir.c_str(), exchg);
	if (!StdFile::exists(path.c_str()))
		return std::move(WtLMDBPtr());

	if (!dbPtr->open(path.c_str()))
	{
		pipe_btreader_log(_sink, LL_ERROR, "Opening {} db if {} failed: {}", subdir, exchg, dbPtr->errmsg());
		return std::move(WtLMDBPtr());
	}
	else
	{
		pipe_btreader_log(_sink, LL_DEBUG, "{} db of {} opened", subdir, exchg);
	}

	(*the_map)[exchg] = dbPtr;
	return std::move(dbPtr);
}

WtBtDtReaderAD::WtLMDBPtr WtBtDtReaderAD::get_t_db(const char* exchg, const char* code)
{
	std::string key = fmtutil::format<64>("{}.{}", exchg, code);
	auto it = _tick_dbs.find(key);
	if (it != _tick_dbs.end())
		/*
		 *	By 秒K线支持 @ 2026.09.20
		 *	这里原先是 return std::move(it->second)，
		 *	把 map 里存的 shared_ptr 直接移走了：第2次调用取到值的同时
		 *	把容器里的置空，第3次及以后 find 命中但拿到的是空指针，
		 *	调用方 if(db) 为假就静默跳过——既不写库也不报错。
		 *	实际表现是 AD 存储的K线从第3根开始全部丢失。
		 *	shared_ptr 拷贝只是加一次引用计数，这里不该 move
		 */
		return it->second;

	WtLMDBPtr dbPtr(new WtLMDB(true));
	std::string path = fmtutil::format("{}ticks/{}/{}", _base_dir.c_str(), exchg, code);
	if (!StdFile::exists(path.c_str()))
		return std::move(WtLMDBPtr());

	if (!dbPtr->open(path.c_str()))
	{
		pipe_btreader_log(_sink, LL_ERROR, "Opening tick db of {}.{} failed: {}", exchg, code, dbPtr->errmsg());
		return std::move(WtLMDBPtr());
	}
	else
	{
		pipe_btreader_log(_sink, LL_DEBUG, "Tick db of {}.{} opened", exchg, code);
	}

	_tick_dbs[exchg] = dbPtr;
	return std::move(dbPtr);
}