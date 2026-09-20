/*!
 * \file test_rdm_secbar.cpp
 * \brief 随机读取(DtServo查询)的秒线支持
 *
 * DtServo 的 get_bars_by_range / by_count / by_date 最终落到
 * WtRdmDtReader::readKlineSliceByRange / ByCount。
 * 查询接口给的时间边界只到分钟(yyyyMMddHHmm)，而秒线的bar时间戳是
 * yyyyMMddHHmmss，两者差5个数量级，所以边界必须按周期换编码，
 * 而且上界要补到该分钟的第59秒，否则那一分钟内的秒线会被整分钟切掉。
 */
#include <stdio.h>
#include <string>
#include <vector>

#include "mock_datasink.hpp"
#include "storage_loader.hpp"
#include "tick_maker.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/BoostFile.hpp"
#include "../Share/fmtlib.h"
#include "gtest/gtest/gtest.h"

#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

USING_NS_WTP;

namespace
{
	const char* R2_EXCHG = "TEST";
	const char* R2_PID = "rb";
	const char* R2_CODE = "rb2610";
	const char* R2_STDCODE = "TEST.rb.2610";
	const uint32_t R2_TDATE = 20260920;

	WTSSessionInfo* r2_make_sess()
	{
		WTSSessionInfo* s = WTSSessionInfo::create("FUTURE", "FUTURE", 0);
		s->addTradingSection(900, 1015);
		s->addTradingSection(1030, 1130);
		return s;
	}

	//造 his/sec5 历史文件，09:00:05 起每5秒一根
	uint32_t r2_write_his(const std::string& dir, uint32_t count)
	{
		std::string path = fmtutil::format("{}his/sec5/{}/", dir, R2_EXCHG);
		fs::create_directories(path);

		std::vector<WTSBarStruct> bars;
		bars.resize(count);
		for (uint32_t i = 0; i < count; i++)
		{
			uint32_t h = 9, m = 0, s = 5 + i * 5;
			m += s / 60; s %= 60;
			h += m / 60; m %= 60;
			uint32_t hms = h * 10000 + m * 100 + s;

			bars[i].date = R2_TDATE;
			bars[i].time = TimeUtils::timeToSecBar(R2_TDATE, hms);
			bars[i].open = bars[i].high = bars[i].low = bars[i].close = 100.0 + i;
			bars[i].vol = 10;
		}

		BlockHeader hdr;
		memset(&hdr, 0, sizeof(hdr));
		strcpy(hdr._blk_flag, BLK_FLAG);
		hdr._type = BT_HIS_Sec5;
		hdr._version = BLOCK_VERSION_RAW_V2;

		BoostFile f;
		f.create_new_file(fmtutil::format("{}{}.dsb", path, R2_CODE));
		f.write_file(&hdr, sizeof(hdr));
		f.write_file(bars.data(), sizeof(WTSBarStruct) * count);
		f.close_file();
		return count;
	}

	std::string r2_temp(const char* tag)
	{
		std::string dir = fmtutil::format("./ut_rdm_{}/", tag);
		if (fs::exists(dir)) fs::remove_all(dir);
		fs::create_directories(dir);
		return dir;
	}
}

TEST(test_rdm_secbar, sec5_by_count)
{
	IRdmDtReader* rd = storage_loader::make_rdm_reader();
	ASSERT_NE(rd, nullptr) << "cannot load WtDataStorage, is WT_TEST_LIBDIR set?";

	WTSSessionInfo* sInfo = r2_make_sess();
	MockBaseDataMgr bd(R2_EXCHG, R2_PID, R2_CODE, sInfo, R2_TDATE);
	std::string dir = r2_temp("cnt");
	//09:00:05 起 300 根，覆盖到 09:25:00
	r2_write_his(dir, 300);

	MockRdmSink sink(&bd);
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", dir.c_str());
	rd->init(cfg, &sink);

	//查到 09:20 为止的最后50根
	uint64_t etime = (uint64_t)R2_TDATE * 10000 + 920;
	WTSKlineSlice* slice = rd->readKlineSliceByCount(R2_STDCODE, KP_Sec5, 50, etime);
	ASSERT_NE(slice, nullptr) << "readKlineSliceByCount returned null for sec5";

	printf("[rdm] by_count got %d bars\n", (int)slice->size());
	fflush(stdout);

	ASSERT_GT(slice->size(), 0);
	EXPECT_EQ(slice->size(), 50);

	//最后一根不能超过 09:20:59（上界补到该分钟末秒）
	uint64_t upper = TimeUtils::timeToSecBar(R2_TDATE, 92059);
	EXPECT_LE(slice->at(-1)->time, upper);

	//时间戳递增且都是秒编码
	uint64_t prev = 0;
	for (int32_t i = 0; i < (int32_t)slice->size(); i++)
	{
		EXPECT_GT(slice->at(i)->time, prev);
		prev = slice->at(i)->time;
		EXPECT_GT(slice->at(i)->time, 20000000000000ULL) << "not a sec-encoded timestamp";
	}

	slice->release();
	cfg->release();
	storage_loader::free_rdm_reader(rd);
	sInfo->release();
	fs::remove_all(dir);
}

TEST(test_rdm_secbar, sec5_by_range)
{
	IRdmDtReader* rd = storage_loader::make_rdm_reader();
	ASSERT_NE(rd, nullptr);

	WTSSessionInfo* sInfo = r2_make_sess();
	MockBaseDataMgr bd(R2_EXCHG, R2_PID, R2_CODE, sInfo, R2_TDATE);
	std::string dir = r2_temp("rng");
	r2_write_his(dir, 300);

	MockRdmSink sink(&bd);
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", dir.c_str());
	rd->init(cfg, &sink);

	//查 09:05 ~ 09:10 这段
	uint64_t stime = (uint64_t)R2_TDATE * 10000 + 905;
	uint64_t etime = (uint64_t)R2_TDATE * 10000 + 910;
	WTSKlineSlice* slice = rd->readKlineSliceByRange(R2_STDCODE, KP_Sec5, stime, etime);
	ASSERT_NE(slice, nullptr) << "readKlineSliceByRange returned null for sec5";

	printf("[rdm] by_range got %d bars\n", (int)slice->size());
	fflush(stdout);

	ASSERT_GT(slice->size(), 0);

	/*
	 *	09:05:00 ~ 09:10:59 之间每5秒一根。
	 *	下界补到该分钟第0秒、上界补到第59秒，
	 *	所以 09:05:05..09:10:55 共 (6*60)/5 = 72 根左右。
	 *	这里不写死条数(受首根对齐影响)，只校验边界严格落在范围内
	 */
	uint64_t lower = TimeUtils::timeToSecBar(R2_TDATE, 90500);
	uint64_t upper = TimeUtils::timeToSecBar(R2_TDATE, 91059);

	for (int32_t i = 0; i < (int32_t)slice->size(); i++)
	{
		uint64_t t = slice->at(i)->time;
		EXPECT_GE(t, lower) << "bar before the lower bound at " << i;
		EXPECT_LE(t, upper) << "bar beyond the upper bound at " << i;
	}

	/*
	 *	上界必须包含 09:10 这一分钟内的秒线。
	 *	如果边界没有补到第59秒，09:10:05..09:10:55 这些都会被切掉，
	 *	最后一根会停在 09:09:55
	 */
	uint64_t lastT = slice->at(-1)->time;
	uint32_t lastHms = TimeUtils::secBarToTime(lastT);
	EXPECT_GE(lastHms, 91005u)
		<< "the upper bound did not cover the 09:10 minute; "
		<< "last bar is " << lastHms << ", looks like the bound was truncated to whole minutes";

	slice->release();
	cfg->release();
	storage_loader::free_rdm_reader(rd);
	sInfo->release();
	fs::remove_all(dir);
}
