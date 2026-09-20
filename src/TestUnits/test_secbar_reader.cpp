/*!
 * \file test_secbar_reader.cpp
 * \brief 秒线读取测试
 *
 * 重点覆盖方案里标注的两个阻塞问题：
 * 1、etime / rt块定位的时间编码必须是秒精度（分钟编码会定位到当天最早或最晚）
 * 2、历史文件按尾部加载，不能整文件读进内存
 */
#include <stdio.h>
#include <string>
#include <vector>

#include "mock_datasink.hpp"
#include "tick_maker.hpp"
#include "../WtDataStorage/WtDataWriter.h"
#include "../WtDataStorage/WtDataReader.h"
#include "../Includes/WTSVariant.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/BoostFile.hpp"
#include "../Share/fmtlib.h"
#include "../WTSUtils/WTSCmpHelper.hpp"
#include "gtest/gtest/gtest.h"

#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

USING_NS_WTP;

namespace
{
	const char* R_EXCHG = "TEST";
	const char* R_PID = "rb";
	const char* R_CODE = "rb2610";
	//readKlineSlice 走 CodeHelper::extractStdCode，必须是 exchg.product.month 格式；
	//它对没有点号的输入会按 npos 长度做 wt_strcpy，直接溢出
	const char* R_STDCODE = "TEST.rb.2610";
	const uint32_t R_TDATE = 20260920;

	WTSSessionInfo* r_make_sess()
	{
		WTSSessionInfo* s = WTSSessionInfo::create("FUTURE", "FUTURE", 0);
		s->addTradingSection(900, 1015);
		s->addTradingSection(1030, 1130);
		return s;
	}

	std::string r_temp_root(const char* tag)
	{
		std::string dir = fmtutil::format("./ut_rd_{}/", tag);
		if (fs::exists(dir))
			fs::remove_all(dir);
		fs::create_directories(dir);
		return dir;
	}

	/*
	 *	直接写一个未压缩的 his/sec5 dsb 文件，省去跑writer的开销
	 *	@start_hms	第一根bar的收盘时刻
	 */
	void write_his_sec5(const std::string& dir, uint32_t count, uint32_t start_hms = 90005)
	{
		std::string path = fmtutil::format("{}his/sec5/{}/", dir, R_EXCHG);
		fs::create_directories(path);
		std::string file = fmtutil::format("{}{}.dsb", path, R_CODE);

		std::vector<WTSBarStruct> bars;
		bars.resize(count);
		for (uint32_t i = 0; i < count; i++)
		{
			bars[i].date = R_TDATE;
			bars[i].time = TimeUtils::timeToSecBar(R_TDATE, tick_maker::advance_hms(start_hms, i * 5));
			bars[i].open = 100.0 + i;
			bars[i].high = 100.5 + i;
			bars[i].low = 99.5 + i;
			bars[i].close = 100.0 + i;
			bars[i].vol = 10;
		}

		BlockHeader hdr;
		memset(&hdr, 0, sizeof(hdr));
		strcpy(hdr._blk_flag, BLK_FLAG);
		hdr._type = BT_HIS_Sec5;
		hdr._version = BLOCK_VERSION_RAW_V2;

		BoostFile f;
		f.create_new_file(file.c_str());
		f.write_file(&hdr, sizeof(hdr));
		f.write_file(bars.data(), sizeof(WTSBarStruct) * count);
		f.close_file();
	}

	WTSVariant* r_make_cfg(const std::string& dir)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("path", dir.c_str());
		return cfg;
	}
}

TEST(test_secbar_reader, read_from_history_only)
{
	WTSSessionInfo* sInfo = r_make_sess();
	MockBaseDataMgr bd(R_EXCHG, R_PID, R_CODE, sInfo, R_TDATE);
	std::string dir = r_temp_root("his");
	write_his_sec5(dir, 500);

	MockReaderSink sink(&bd);
	//把当前时刻设在历史数据之后的另一天，确保 bHasToday 为 false
	sink.set_time(20260921, 1000, 0);

	WtDataReader reader;
	WTSVariant* cfg = r_make_cfg(dir);
	reader.init(cfg, &sink);

	WTSKlineSlice* slice = reader.readKlineSlice(R_STDCODE, KP_Sec5, 150, 0);
	ASSERT_NE(slice, nullptr);
	EXPECT_EQ(slice->size(), 150) << "should get exactly the requested count from history";

	//最后一根必须是历史里最新的那根
	uint64_t expectLast = TimeUtils::timeToSecBar(R_TDATE, tick_maker::advance_hms(90005, 499 * 5));
	EXPECT_EQ(slice->at(-1)->time, expectLast);

	slice->release();
	cfg->release();
	sInfo->release();
	fs::remove_all(dir);
}

/*
 *	尾部加载：文件里有很多条，但只应驻留请求量级的数据
 */
TEST(test_secbar_reader, tail_load_does_not_read_whole_file)
{
	WTSSessionInfo* sInfo = r_make_sess();
	MockBaseDataMgr bd(R_EXCHG, R_PID, R_CODE, sInfo, R_TDATE);
	std::string dir = r_temp_root("tail");

	//1小节约900根，这里写5万根，全量读入是 5万*88 = 4.4MB
	const uint32_t TOTAL = 50000;
	write_his_sec5(dir, TOTAL);

	MockReaderSink sink(&bd);
	sink.set_time(20260921, 1000, 0);

	WtDataReader reader;
	WTSVariant* cfg = r_make_cfg(dir);
	reader.init(cfg, &sink);

	WTSKlineSlice* slice = reader.readKlineSlice(R_STDCODE, KP_Sec5, 150, 0);
	ASSERT_NE(slice, nullptr);
	EXPECT_EQ(slice->size(), 150);

	//必须走的是尾部加载，而不是把5万根整个读进来
	EXPECT_TRUE(sink.log_contains("cached from tail"))
		<< "sec5 history must be loaded from the file tail, not in full";

	//尾部加载的日志里会带上实际读取条数，应该是 150*2+16=316 而不是 TOTAL
	EXPECT_TRUE(sink.log_contains("316 items"))
		<< "expected to read only the requested tail (316), full log: "
		<< (sink.logs().empty() ? std::string("<empty>") : sink.logs().back());

	//最后一根仍然是文件里最新的
	uint64_t expectLast = TimeUtils::timeToSecBar(R_TDATE, tick_maker::advance_hms(90005, (TOTAL - 1) * 5));
	EXPECT_EQ(slice->at(-1)->time, expectLast);

	slice->release();
	cfg->release();
	sInfo->release();
	fs::remove_all(dir);
}

/*
 *	请求条数超过上次尾部加载量时，要能自动多读一些
 */
TEST(test_secbar_reader, reload_when_more_bars_requested)
{
	WTSSessionInfo* sInfo = r_make_sess();
	MockBaseDataMgr bd(R_EXCHG, R_PID, R_CODE, sInfo, R_TDATE);
	std::string dir = r_temp_root("reload");
	write_his_sec5(dir, 5000);

	MockReaderSink sink(&bd);
	sink.set_time(20260921, 1000, 0);

	WtDataReader reader;
	WTSVariant* cfg = r_make_cfg(dir);
	reader.init(cfg, &sink);

	//先要少量
	WTSKlineSlice* s1 = reader.readKlineSlice(R_STDCODE, KP_Sec5, 50, 0);
	ASSERT_NE(s1, nullptr);
	EXPECT_EQ(s1->size(), 50);
	s1->release();

	//再要更多，必须能拿满
	WTSKlineSlice* s2 = reader.readKlineSlice(R_STDCODE, KP_Sec5, 1200, 0);
	ASSERT_NE(s2, nullptr);
	EXPECT_EQ(s2->size(), 1200) << "reader should reload more bars from file when asked for more";
	s2->release();

	cfg->release();
	sInfo->release();
	fs::remove_all(dir);
}

/*
 *	rt块定位用秒精度：方案里阻塞问题2的回归
 *
 *	readKlineSlice 会造一个临时bar去rt块里做lower_bound定位"当前时刻"。
 *	如果这个bar的时间戳用了分钟编码((date-19900000)*10000+HHMMSS，约3.6e9)，
 *	而块里的bar是秒编码(约2e13)，就会比所有bar都小，定位到第一根之后
 *	再做 pBar-- / idx-- 造成下溢，取到的数据整体跑偏。
 *
 *	注意：纯历史场景下etime不参与裁剪，这是框架既有行为(min1也一样)，
 *	etime只在有当日实时数据时用于rt块定位，所以这里必须造出rt块。
 */
TEST(test_secbar_reader, etime_locates_rt_block_by_second)
{
	WTSSessionInfo* sInfo = r_make_sess();
	MockBaseDataMgr bd(R_EXCHG, R_PID, R_CODE, sInfo, R_TDATE);
	std::string dir = r_temp_root("etime");

	//造当日的 rt/sec5：09:00:00 起 20 根 s5
	const uint32_t BARS = 20;
	{
		MockWriterSink wsink(&bd, R_TDATE);
		WtDataWriter writer;
		WTSVariant* wcfg = WTSVariant::createObject();
		wcfg->append("path", dir.c_str());
		wcfg->append("async", false);
		wcfg->append("enablesec5", true);
		wcfg->append("disablemin1", true);
		wcfg->append("disablemin5", true);
		wcfg->append("disableday", true);
		wcfg->append("disabletick", true);
		ASSERT_TRUE(writer.init(wcfg, &wsink));

		double hi = 0, lo = 0;
		for (uint32_t i = 0; i < BARS; i++)
		{
			double px = 100.0 + i;
			hi = (i == 0) ? px : std::max(hi, px);
			lo = (i == 0) ? px : std::min(lo, px);
			uint32_t hms = tick_maker::advance_hms(90000, i * 5);
			WTSTickStruct ts = tick_maker::make(R_CODE, R_TDATE, R_TDATE, hms, px, 1, 100, 0, hi, lo);
			WTSTickData* tick = WTSTickData::create(ts);
			tick->setContractInfo(bd.contract());
			writer.writeTick(tick, 0);
			tick->release();
		}
		writer.release();
		wcfg->release();
	}

	MockReaderSink sink(&bd);
	sink.set_time(R_TDATE, 900, 0);

	WtDataReader reader;
	WTSVariant* cfg = r_make_cfg(dir);
	reader.init(cfg, &sink);

	//第8根bar的收盘时刻(09:00:40)，以它作为etime
	uint32_t hms8 = tick_maker::advance_hms(90005, 7 * 5);
	uint64_t etime = TimeUtils::timeToSecBar(R_TDATE, hms8);

	WTSKlineSlice* slice = reader.readKlineSlice(R_STDCODE, KP_Sec5, 5, etime);
	ASSERT_NE(slice, nullptr);
	ASSERT_GT(slice->size(), 0);

	//定位正确的话，最后一根就是etime那根，绝不能超过
	EXPECT_LE(slice->at(-1)->time, etime) << "last bar must not exceed etime";
	EXPECT_EQ(slice->at(-1)->time, etime) << "rt block lookup should land exactly on the etime bar";

	//整个slice的时间戳都要落在etime之前且递增
	uint64_t prev = 0;
	for (int32_t i = 0; i < (int32_t)slice->size(); i++)
	{
		const WTSBarStruct* b = slice->at(i);
		EXPECT_GT(b->time, prev);
		EXPECT_LE(b->time, etime);
		prev = b->time;
	}

	slice->release();
	cfg->release();
	sInfo->release();
	fs::remove_all(dir);
}

/*
 *	纯历史场景下etime不裁剪历史段，这是框架既有行为，不是秒线的缺陷。
 *	写成测试是为了把这个语义固定下来，避免以后被当成bug"修"掉。
 */
TEST(test_secbar_reader, etime_does_not_trim_history_only_reads)
{
	WTSSessionInfo* sInfo = r_make_sess();
	MockBaseDataMgr bd(R_EXCHG, R_PID, R_CODE, sInfo, R_TDATE);
	std::string dir = r_temp_root("etime2");
	write_his_sec5(dir, 500);

	MockReaderSink sink(&bd);
	//当前时刻在另一天，没有当日实时数据
	sink.set_time(20260921, 1000, 0);

	WtDataReader reader;
	WTSVariant* cfg = r_make_cfg(dir);
	reader.init(cfg, &sink);

	uint32_t hms100 = tick_maker::advance_hms(90005, 99 * 5);
	uint64_t etime = TimeUtils::timeToSecBar(R_TDATE, hms100);

	WTSKlineSlice* slice = reader.readKlineSlice(R_STDCODE, KP_Sec5, 10, etime);
	ASSERT_NE(slice, nullptr);
	ASSERT_GT(slice->size(), 0);

	//历史段取的是末尾count条，与etime无关
	uint64_t lastOfFile = TimeUtils::timeToSecBar(R_TDATE, tick_maker::advance_hms(90005, 499 * 5));
	EXPECT_EQ(slice->at(-1)->time, lastOfFile);

	slice->release();
	cfg->release();
	sInfo->release();
	fs::remove_all(dir);
}

/*
 *	onSecondEnd 的闭合回调：次数、顺序、不重不漏
 */
TEST(test_secbar_reader, on_second_end_emits_bars_in_order)
{
	WTSSessionInfo* sInfo = r_make_sess();
	MockBaseDataMgr bd(R_EXCHG, R_PID, R_CODE, sInfo, R_TDATE);
	std::string dir = r_temp_root("onsec");

	//先用writer造出 rt/sec5 的实时块
	{
		MockWriterSink wsink(&bd, R_TDATE);
		WtDataWriter writer;
		WTSVariant* wcfg = WTSVariant::createObject();
		wcfg->append("path", dir.c_str());
		wcfg->append("async", false);
		wcfg->append("enablesec5", true);
		wcfg->append("disablemin1", true);
		wcfg->append("disablemin5", true);
		wcfg->append("disableday", true);
		wcfg->append("disabletick", true);
		ASSERT_TRUE(writer.init(wcfg, &wsink));

		//09:00:00 起 10 根 s5（每5秒一笔tick）
		double hi = 0, lo = 0;
		for (uint32_t i = 0; i < 10; i++)
		{
			double px = 100.0 + i;
			hi = (i == 0) ? px : std::max(hi, px);
			lo = (i == 0) ? px : std::min(lo, px);
			uint32_t hms = tick_maker::advance_hms(90000, i * 5);
			WTSTickStruct ts = tick_maker::make(R_CODE, R_TDATE, R_TDATE, hms, px, 1, 100, 0, hi, lo);
			WTSTickData* tick = WTSTickData::create(ts);
			tick->setContractInfo(bd.contract());
			writer.writeTick(tick, 0);
			tick->release();
		}
		writer.release();
		wcfg->release();
	}

	MockReaderSink sink(&bd);
	sink.set_time(R_TDATE, 900, 0);

	WtDataReader reader;
	WTSVariant* cfg = r_make_cfg(dir);
	reader.init(cfg, &sink);

	//先建立缓存（这一步会把_rt_cursor定位到当前时刻）
	WTSKlineSlice* slice = reader.readKlineSlice(R_STDCODE, KP_Sec5, 10, 0);
	if (slice) slice->release();

	sink.clear();

	//逐秒推进，检查闭合事件
	uint64_t prevTime = 0;
	for (uint32_t i = 0; i < 10; i++)
	{
		uint32_t hms = tick_maker::advance_hms(90005, i * 5);
		reader.onSecondEnd(R_TDATE, hms, 0);
	}

	//闭合事件的时间戳必须严格递增（不重不乱序）
	for (const auto& e : sink.bars())
	{
		EXPECT_EQ(e._period, KP_Sec5);
		EXPECT_GT(e._time, prevTime) << "bar events must be strictly increasing, no duplicates";
		prevTime = e._time;
	}

	//再重复推进同样的时刻，不应产生新的闭合事件
	size_t cntBefore = sink.bars().size();
	for (uint32_t i = 0; i < 10; i++)
	{
		uint32_t hms = tick_maker::advance_hms(90005, i * 5);
		reader.onSecondEnd(R_TDATE, hms, 0);
	}
	EXPECT_EQ(sink.bars().size(), cntBefore) << "replaying the same timestamps must not re-emit bars";

	cfg->release();
	sInfo->release();
	fs::remove_all(dir);
}

/*
 *	压缩格式的历史文件要能继续读（向后兼容）
 *	尾部加载对压缩文件会返回0，自动退回全量路径
 */
TEST(test_secbar_reader, compressed_his_file_still_readable)
{
	WTSSessionInfo* sInfo = r_make_sess();
	MockBaseDataMgr bd(R_EXCHG, R_PID, R_CODE, sInfo, R_TDATE);
	std::string dir = r_temp_root("cmp");

	//手工造一个压缩格式的 sec5 历史文件
	{
		std::string path = fmtutil::format("{}his/sec5/{}/", dir, R_EXCHG);
		fs::create_directories(path);
		std::string file = fmtutil::format("{}{}.dsb", path, R_CODE);

		const uint32_t CNT = 300;
		std::vector<WTSBarStruct> bars;
		bars.resize(CNT);
		for (uint32_t i = 0; i < CNT; i++)
		{
			bars[i].date = R_TDATE;
			bars[i].time = TimeUtils::timeToSecBar(R_TDATE, tick_maker::advance_hms(90005, i * 5));
			bars[i].close = 100.0 + i;
		}

		std::string raw((const char*)bars.data(), sizeof(WTSBarStruct) * CNT);
		std::string cmp = WTSCmpHelper::compress_data(raw.data(), raw.size());

		BlockHeaderV2 hdr;
		memset(&hdr, 0, sizeof(hdr));
		strcpy(hdr._blk_flag, BLK_FLAG);
		hdr._type = BT_HIS_Sec5;
		hdr._version = BLOCK_VERSION_CMP_V2;
		hdr._size = cmp.size();

		BoostFile f;
		f.create_new_file(file.c_str());
		f.write_file(&hdr, sizeof(hdr));
		f.write_file(cmp);
		f.close_file();
	}

	MockReaderSink sink(&bd);
	sink.set_time(20260921, 1000, 0);

	WtDataReader reader;
	WTSVariant* cfg = r_make_cfg(dir);
	reader.init(cfg, &sink);

	WTSKlineSlice* slice = reader.readKlineSlice(R_STDCODE, KP_Sec5, 100, 0);
	ASSERT_NE(slice, nullptr) << "compressed sec5 history must remain readable";
	EXPECT_EQ(slice->size(), 100);
	EXPECT_EQ(slice->at(-1)->time, TimeUtils::timeToSecBar(R_TDATE, tick_maker::advance_hms(90005, 299 * 5)));

	slice->release();
	cfg->release();
	sInfo->release();
	fs::remove_all(dir);
}
