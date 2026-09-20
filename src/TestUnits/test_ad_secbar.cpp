/*!
 * \file test_ad_secbar.cpp
 * \brief AD(LMDB)存储的秒线读写闭环
 *
 * 验证 writer 写进 LMDB 的 sec5 能被 reader 原样读回来。
 * 重点在 key：秒线的时间戳是 yyyyMMddHHmmss(约2e13)，
 * 必须走 LMDBSecBarKey(uint64)，用 LMDBBarKey 的 uint32 会丢高位。
 */
#include <stdio.h>
#include <string>
#include <vector>

#include "mock_datasink.hpp"
#include "storage_loader.hpp"
#include "../WTSUtils/WtLMDB.hpp"
#include "../WtDataStorageAD/LMDBKeys.h"
#include "tick_maker.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/fmtlib.h"
#include "gtest/gtest/gtest.h"

#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

USING_NS_WTP;

namespace
{
	const char* A_EXCHG = "TEST";
	const char* A_PID = "rb";
	const char* A_CODE = "rb2610";
	const char* A_STDCODE = "TEST.rb.2610";
	const uint32_t A_TDATE = 20260920;

	WTSSessionInfo* a_make_sess()
	{
		WTSSessionInfo* s = WTSSessionInfo::create("FUTURE", "FUTURE", 0);
		s->addTradingSection(900, 1015);
		s->addTradingSection(1030, 1130);
		return s;
	}

	std::string a_temp_root(const char* tag)
	{
		std::string dir = fmtutil::format("./ut_ad_{}/", tag);
		if (fs::exists(dir)) fs::remove_all(dir);
		fs::create_directories(dir);
		return dir;
	}
}

/*
 *	writer 写 -> reader 读，秒线必须能完整回来
 */
TEST(test_ad_secbar, sec5_write_then_read_back)
{
	IDataWriter* pw = storage_loader::make_ad_writer();
	ASSERT_NE(pw, nullptr) << "cannot load WtDataStorageAD, is WT_TEST_LIBDIR set?";

	WTSSessionInfo* sInfo = a_make_sess();
	MockBaseDataMgr bd(A_EXCHG, A_PID, A_CODE, sInfo, A_TDATE);
	std::string dir = a_temp_root("rw");
	MockWriterSink wsink(&bd, A_TDATE);

	WTSVariant* wcfg = WTSVariant::createObject();
	wcfg->append("path", dir.c_str());
	wcfg->append("async", false);
	wcfg->append("enablesec5", true);
	//只测秒线，其他周期关掉
	wcfg->append("disablemin1", true);
	wcfg->append("disablemin5", true);
	wcfg->append("disableday", true);
	ASSERT_TRUE(pw->init(wcfg, &wsink));

	/*
	 *	喂 40 秒的tick，每秒一笔 -> 8 根 s5。
	 *	注意 AD 的写入模式是"跨bar时把上一根写库"，
	 *	所以最后一根还留在内存缓存里，库里只有前面几根
	 */
	const uint32_t TICKS = 40;
	double hi = 0, lo = 0;
	for (uint32_t i = 0; i < TICKS; i++)
	{
		double px = 100.0 + (i % 7);
		hi = (i == 0) ? px : std::max(hi, px);
		lo = (i == 0) ? px : std::min(lo, px);

		WTSTickStruct ts = tick_maker::make(A_CODE, A_TDATE, A_TDATE, 90000 + i, px, 1, 100, 0, hi, lo);
		WTSTickData* tick = WTSTickData::create(ts);
		tick->setContractInfo(bd.contract());
		pw->writeTick(tick, 0);
		tick->release();
	}

	pw->release();
	storage_loader::free_writer(pw);

	//LMDB 的库文件必须已生成
	std::string dbdir = fmtutil::format("{}sec5/{}/", dir, A_EXCHG);
	EXPECT_TRUE(fs::exists(dbdir)) << "sec5 lmdb dir not created: " << dbdir;

	/*
	 *	直接开库数一下究竟写进去多少条。
	 *	这一步不只是诊断：它能区分"writer没写"和"reader没读出来"，
	 *	而且正是这条断言暴露了 get_k_db 里 shared_ptr 被误 move 的缺陷
	 *	(修复前库里只有2条，之后的落库全被静默跳过)
	 */
	{
		WtLMDB db(true);
		ASSERT_TRUE(db.open(dbdir.c_str())) << "cannot open sec5 lmdb";

		WtLMDBQuery q(db);
		LMDBSecBarKey lk(A_EXCHG, A_CODE, 0);
		LMDBSecBarKey rk(A_EXCHG, A_CODE, 0xFFFFFFFFFFFFFFFFULL);
		uint64_t prevInDb = 0;
		int n = q.get_range(std::string((const char*)&lk, sizeof(lk)),
			std::string((const char*)&rk, sizeof(rk)),
			[&prevInDb](const ValueArray& ks, const ValueArray& vs) {
				for (std::size_t i = 0; i < vs.size(); i++)
				{
					const WTSBarStruct* b = (const WTSBarStruct*)vs[i].data();
					//范围查询的结果必须按时间递增，这依赖key的大端编码
					EXPECT_GT(b->time, prevInDb) << "rows from lmdb are not in time order";
					prevInDb = b->time;
				}
			});

		printf("[ad] rows in lmdb = %d\n", n);
		fflush(stdout);

		/*
		 *	40秒的tick按5秒切是8根，最后一根还留在内存缓存里没落库，
		 *	所以库里应该正好7根。
		 *	如果只有2根，说明 get_k_db 又把 map 里的 shared_ptr 移空了
		 */
		EXPECT_EQ(n, 7)
			<< "expected exactly 7 rows in lmdb; "
			<< "getting 2 means get_k_db is moving the shared_ptr out of the map again";
	}

	//---- 读回来 ----
	IDataReader* prd = storage_loader::make_ad_reader();
	ASSERT_NE(prd, nullptr);

	MockReaderSink rsink(&bd);
	rsink.set_time(A_TDATE, 1100, 0);

	WTSVariant* rcfg = WTSVariant::createObject();
	rcfg->append("path", dir.c_str());
	prd->init(rcfg, &rsink);

	WTSKlineSlice* slice = prd->readKlineSlice(A_STDCODE, KP_Sec5, 100, 0);
	ASSERT_NE(slice, nullptr) << "readKlineSlice returned null for sec5";

	printf("[ad] sec5 bars read back = %d\n", (int)slice->size());
	fflush(stdout);

	//库里7根，reader还会把缓存里那根未闭合的追加上来，所以是7或8
	EXPECT_GE(slice->size(), 7) << "sec5 bars lost between write and read";
	EXPECT_LE(slice->size(), 8);

	//时间戳必须是 yyyyMMddHHmmss，且严格递增
	uint64_t prev = 0;
	for (int32_t i = 0; i < (int32_t)slice->size(); i++)
	{
		const WTSBarStruct* b = slice->at(i);
		EXPECT_GT(b->time, prev) << "bar time not increasing at " << i;
		prev = b->time;

		EXPECT_GT(b->time, 20000000000000ULL) << "bar time is not in yyyyMMddHHmmss form";
		uint32_t hms = TimeUtils::secBarToTime(b->time);
		EXPECT_TRUE(hms >= 90000 && hms <= 90045) << "unexpected bar time " << hms;
	}

	slice->release();
	storage_loader::free_reader(prd);
	rcfg->release();
	wcfg->release();
	sInfo->release();
	fs::remove_all(dir);
}

/*
 *	默认不开秒线时不能产生 sec5 的库
 */
TEST(test_ad_secbar, sec5_disabled_by_default)
{
	IDataWriter* pw = storage_loader::make_ad_writer();
	ASSERT_NE(pw, nullptr);

	WTSSessionInfo* sInfo = a_make_sess();
	MockBaseDataMgr bd(A_EXCHG, A_PID, A_CODE, sInfo, A_TDATE);
	std::string dir = a_temp_root("off");
	MockWriterSink wsink(&bd, A_TDATE);

	WTSVariant* wcfg = WTSVariant::createObject();
	wcfg->append("path", dir.c_str());
	wcfg->append("async", false);
	//不传 enablesec5，模拟老配置
	wcfg->append("disablemin1", true);
	wcfg->append("disablemin5", true);
	wcfg->append("disableday", true);
	ASSERT_TRUE(pw->init(wcfg, &wsink));

	for (uint32_t i = 0; i < 20; i++)
	{
		WTSTickStruct ts = tick_maker::make(A_CODE, A_TDATE, A_TDATE, 90000 + i, 100.0, 1, 100, 0, 100.0, 100.0);
		WTSTickData* tick = WTSTickData::create(ts);
		tick->setContractInfo(bd.contract());
		pw->writeTick(tick, 0);
		tick->release();
	}

	pw->release();
	storage_loader::free_writer(pw);

	EXPECT_FALSE(fs::exists(std::string(fmtutil::format("{}sec5/", dir))))
		<< "sec5 must stay off unless explicitly enabled";
	//缓存文件也不该建
	EXPECT_FALSE(StdFile::exists(fmtutil::format("{}cache_s5.dmb", dir)));

	wcfg->release();
	sInfo->release();
	fs::remove_all(dir);
}
