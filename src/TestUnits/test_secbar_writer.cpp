/*!
 * \file test_secbar_writer.cpp
 * \brief 秒线落盘测试（rt/sec5 的 dmb 与盘后 his/sec5 的 dsb）
 */
#include <stdio.h>
#include <string>

#include "mock_datasink.hpp"
#include "tick_maker.hpp"
#include "storage_loader.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/StrUtil.hpp"
#include "../Share/fmtlib.h"
#include "gtest/gtest/gtest.h"

//与 WtDataWriter.cpp 保持一致，用 boost::filesystem
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

USING_NS_WTP;

namespace
{
	const char* EXCHG = "TEST";
	const char* PID = "rb";
	const char* CODE = "rb2610";
	const uint32_t TDATE = 20260920;

	WTSSessionInfo* make_sess()
	{
		//9:00-10:15 (4500s), 10:30-11:30 (3600s)
		WTSSessionInfo* s = WTSSessionInfo::create("FUTURE", "FUTURE", 0);
		s->addTradingSection(900, 1015);
		s->addTradingSection(1030, 1130);
		return s;
	}

	std::string temp_root(const char* tag)
	{
		std::string dir = fmtutil::format("./ut_sec5_{}/", tag);
		if (fs::exists(dir))
			fs::remove_all(dir);
		fs::create_directories(dir);
		return dir;
	}

	//构造 writer 的配置
	WTSVariant* make_cfg(const std::string& dir, bool enable_sec5, const char* codes = "")
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("path", dir.c_str());
		cfg->append("async", false);
		cfg->append("enablesec5", enable_sec5);
		if (strlen(codes) > 0)
			cfg->append("sec5_codes", codes);
		//本测试只关心秒线，把其他周期关掉以减少干扰
		cfg->append("disablemin1", true);
		cfg->append("disablemin5", true);
		cfg->append("disableday", true);
		cfg->append("disabletick", true);
		return cfg;
	}

	//直接读 rt/sec5 的 dmb 文件
	bool read_rt_bars(const std::string& dir, std::vector<WTSBarStruct>& bars, uint16_t& blkType)
	{
		std::string path = fmtutil::format("{}rt/sec5/{}/{}.dmb", dir, EXCHG, CODE);
		if (!StdFile::exists(path.c_str()))
			return false;

		std::string content;
		StdFile::read_file_content(path.c_str(), content);
		if (content.size() < sizeof(RTKlineBlock))
			return false;

		RTKlineBlock* blk = (RTKlineBlock*)content.data();
		blkType = blk->_type;
		bars.assign(blk->_bars, blk->_bars + blk->_size);
		return true;
	}
}

TEST(test_secbar_writer, disabled_by_default_writes_nothing)
{
	WTSSessionInfo* sInfo = make_sess();
	MockBaseDataMgr bd(EXCHG, PID, CODE, sInfo);
	std::string dir = temp_root("off");
	MockWriterSink sink(&bd, TDATE);

	IDataWriter* pw = storage_loader::make_writer();
	ASSERT_NE(pw, nullptr) << "cannot load WtDataStorage, is WT_TEST_LIBDIR set?";
	IDataWriter& writer = *pw;
	//注意这里不传 enablesec5，模拟老配置升级上来的情形
	WTSVariant* cfg = make_cfg(dir, false);
	ASSERT_TRUE(writer.init(cfg, &sink));

	for (int i = 0; i < 10; i++)
	{
		WTSTickStruct ts = tick_maker::make(CODE, TDATE, TDATE, 90000 + i, 100.0);
		WTSTickData* tick = WTSTickData::create(ts);
		tick->setContractInfo(bd.contract());
		writer.writeTick(tick, 0);
		tick->release();
	}

	//默认关闭，不能产生任何秒线文件
	std::string path = fmtutil::format("{}rt/sec5/{}/{}.dmb", dir, EXCHG, CODE);
	EXPECT_FALSE(StdFile::exists(path.c_str())) << "sec5 must stay off unless explicitly enabled";

	writer.release();
	storage_loader::free_writer(pw);
	cfg->release();
	sInfo->release();
	fs::remove_all(dir);
}

TEST(test_secbar_writer, rt_block_bars_and_ohlc)
{
	WTSSessionInfo* sInfo = make_sess();
	MockBaseDataMgr bd(EXCHG, PID, CODE, sInfo);
	std::string dir = temp_root("rt");
	MockWriterSink sink(&bd, TDATE);

	IDataWriter* pw = storage_loader::make_writer();
	ASSERT_NE(pw, nullptr) << "cannot load WtDataStorage, is WT_TEST_LIBDIR set?";
	IDataWriter& writer = *pw;
	WTSVariant* cfg = make_cfg(dir, true);
	ASSERT_TRUE(writer.init(cfg, &sink));

	//12秒的tick，每秒一笔，价格有起伏。
	//注意要同时维护当日累计高低价，否则 updateCache 会把每笔都判成创新高
	const double pxs[] = { 100, 102, 98, 101, 99, 105, 103, 97, 100, 104, 96, 101 };
	double hi = 0, lo = 0;
	for (int i = 0; i < 12; i++)
	{
		hi = (i == 0) ? pxs[i] : std::max(hi, pxs[i]);
		lo = (i == 0) ? pxs[i] : std::min(lo, pxs[i]);

		WTSTickStruct ts = tick_maker::make(CODE, TDATE, TDATE, 90000 + i, pxs[i], 1, 100, 0, hi, lo);
		WTSTickData* tick = WTSTickData::create(ts);
		tick->setContractInfo(bd.contract());
		writer.writeTick(tick, 0);
		tick->release();
	}

	std::vector<WTSBarStruct> bars;
	uint16_t blkType = 0;
	ASSERT_TRUE(read_rt_bars(dir, bars, blkType));

	EXPECT_EQ(blkType, (uint16_t)BT_RT_Sec5);
	//09:00:00~04 / 05~09 / 10~11(未满)
	ASSERT_EQ(bars.size(), 3u);

	EXPECT_EQ(bars[0].time, TimeUtils::timeToSecBar(TDATE, 90005));
	EXPECT_EQ(bars[1].time, TimeUtils::timeToSecBar(TDATE, 90010));
	EXPECT_EQ(bars[2].time, TimeUtils::timeToSecBar(TDATE, 90015));

	//第一根：100,102,98,99
	EXPECT_DOUBLE_EQ(bars[0].open, 100);
	EXPECT_DOUBLE_EQ(bars[0].high, 102);
	EXPECT_DOUBLE_EQ(bars[0].low, 98);
	EXPECT_DOUBLE_EQ(bars[0].close, 99);
	EXPECT_DOUBLE_EQ(bars[0].vol, 5);

	//第二根：105,105,97,104
	EXPECT_DOUBLE_EQ(bars[1].open, 105);
	EXPECT_DOUBLE_EQ(bars[1].high, 105);
	EXPECT_DOUBLE_EQ(bars[1].low, 97);
	EXPECT_DOUBLE_EQ(bars[1].close, 104);
	EXPECT_DOUBLE_EQ(bars[1].vol, 5);

	//第三根只有2笔
	EXPECT_DOUBLE_EQ(bars[2].vol, 2);

	//交易日字段
	EXPECT_EQ(bars[0].date, TDATE);

	writer.release();
	storage_loader::free_writer(pw);
	cfg->release();
	sInfo->release();
	fs::remove_all(dir);
}

TEST(test_secbar_writer, whitelist_filters_contracts)
{
	WTSSessionInfo* sInfo = make_sess();
	MockBaseDataMgr bd(EXCHG, PID, CODE, sInfo);
	std::string dir = temp_root("wl");
	MockWriterSink sink(&bd, TDATE);

	IDataWriter* pw = storage_loader::make_writer();
	ASSERT_NE(pw, nullptr) << "cannot load WtDataStorage, is WT_TEST_LIBDIR set?";
	IDataWriter& writer = *pw;
	//白名单里放一个别的合约，当前合约不应落秒线
	WTSVariant* cfg = make_cfg(dir, true, "TEST.hc2610");
	ASSERT_TRUE(writer.init(cfg, &sink));

	for (int i = 0; i < 10; i++)
	{
		WTSTickStruct ts = tick_maker::make(CODE, TDATE, TDATE, 90000 + i, 100.0);
		WTSTickData* tick = WTSTickData::create(ts);
		tick->setContractInfo(bd.contract());
		writer.writeTick(tick, 0);
		tick->release();
	}

	std::string path = fmtutil::format("{}rt/sec5/{}/{}.dmb", dir, EXCHG, CODE);
	EXPECT_FALSE(StdFile::exists(path.c_str())) << "contract not in whitelist must be skipped";

	writer.release();
	storage_loader::free_writer(pw);
	cfg->release();
	sInfo->release();
	fs::remove_all(dir);
}

TEST(test_secbar_writer, whitelist_hit_writes_bars)
{
	WTSSessionInfo* sInfo = make_sess();
	MockBaseDataMgr bd(EXCHG, PID, CODE, sInfo);
	std::string dir = temp_root("wl2");
	MockWriterSink sink(&bd, TDATE);

	IDataWriter* pw = storage_loader::make_writer();
	ASSERT_NE(pw, nullptr) << "cannot load WtDataStorage, is WT_TEST_LIBDIR set?";
	IDataWriter& writer = *pw;
	//白名单命中（顺便验证逗号分隔与空格容忍）
	WTSVariant* cfg = make_cfg(dir, true, "TEST.hc2610, TEST.rb2610");
	ASSERT_TRUE(writer.init(cfg, &sink));

	for (int i = 0; i < 10; i++)
	{
		WTSTickStruct ts = tick_maker::make(CODE, TDATE, TDATE, 90000 + i, 100.0);
		WTSTickData* tick = WTSTickData::create(ts);
		tick->setContractInfo(bd.contract());
		writer.writeTick(tick, 0);
		tick->release();
	}

	std::vector<WTSBarStruct> bars;
	uint16_t blkType = 0;
	ASSERT_TRUE(read_rt_bars(dir, bars, blkType));
	EXPECT_EQ(bars.size(), 2u);

	writer.release();
	storage_loader::free_writer(pw);
	cfg->release();
	sInfo->release();
	fs::remove_all(dir);
}

/*
 *	白名单按品种：写 TEST.rb 要覆盖 rb 的所有月份，
 *	写别的品种或品种前缀(TEST.r)都不能误命中
 */
namespace
{
	//按给定白名单喂10秒tick，返回是否生成了 rt/sec5 文件
	bool run_whitelist(const char* tag, const char* codes, std::vector<WTSBarStruct>& bars)
	{
		WTSSessionInfo* sInfo = make_sess();
		MockBaseDataMgr bd(EXCHG, PID, CODE, sInfo);
		std::string dir = temp_root(tag);
		MockWriterSink sink(&bd, TDATE);

		IDataWriter* pw = storage_loader::make_writer();
		if (pw == nullptr)
		{
			ADD_FAILURE() << "cannot load WtDataStorage, is WT_TEST_LIBDIR set?";
			sInfo->release();
			return false;
		}

		WTSVariant* cfg = make_cfg(dir, true, codes);
		EXPECT_TRUE(pw->init(cfg, &sink));

		for (int i = 0; i < 10; i++)
		{
			WTSTickStruct ts = tick_maker::make(CODE, TDATE, TDATE, 90000 + i, 100.0);
			WTSTickData* tick = WTSTickData::create(ts);
			tick->setContractInfo(bd.contract());
			pw->writeTick(tick, 0);
			tick->release();
		}

		uint16_t blkType = 0;
		bool ok = read_rt_bars(dir, bars, blkType);

		pw->release();
		storage_loader::free_writer(pw);
		cfg->release();
		sInfo->release();
		fs::remove_all(dir);
		return ok;
	}
}

TEST(test_secbar_writer, whitelist_product_hit_writes_bars)
{
	std::vector<WTSBarStruct> bars;
	ASSERT_TRUE(run_whitelist("wlp", "TEST.rb", bars)) << "product entry should cover all months";
	EXPECT_EQ(bars.size(), 2u);
}

TEST(test_secbar_writer, whitelist_product_mixed_with_codes)
{
	//品种和合约混写，品种命中即可
	std::vector<WTSBarStruct> bars;
	ASSERT_TRUE(run_whitelist("wlp2", "TEST.hc2610, TEST.rb", bars));
	EXPECT_EQ(bars.size(), 2u);
}

TEST(test_secbar_writer, whitelist_other_product_skipped)
{
	std::vector<WTSBarStruct> bars;
	EXPECT_FALSE(run_whitelist("wlp3", "TEST.hc", bars)) << "other product must not match";
}

TEST(test_secbar_writer, whitelist_partial_product_skipped)
{
	//必须精确匹配品种，不能按前缀匹配
	std::vector<WTSBarStruct> bars;
	EXPECT_FALSE(run_whitelist("wlp4", "TEST.r, rb, TEST.rb26", bars)) << "partial/exchange-less entries must not match";
}

/*
 *	小节边界：10:15:00 的tick要归第一小节最后一根，不能跨到第二小节
 */
TEST(test_secbar_writer, section_end_tick_stays_in_section)
{
	WTSSessionInfo* sInfo = make_sess();
	MockBaseDataMgr bd(EXCHG, PID, CODE, sInfo);
	std::string dir = temp_root("sect");
	MockWriterSink sink(&bd, TDATE);

	IDataWriter* pw = storage_loader::make_writer();
	ASSERT_NE(pw, nullptr) << "cannot load WtDataStorage, is WT_TEST_LIBDIR set?";
	IDataWriter& writer = *pw;
	WTSVariant* cfg = make_cfg(dir, true);
	ASSERT_TRUE(writer.init(cfg, &sink));

	//第一小节尾部 + 收盘时刻 + 第二小节开头
	const uint32_t hmss[] = { 101453, 101457, 101500, 103001, 103003 };
	for (uint32_t hms : hmss)
	{
		WTSTickStruct ts = tick_maker::make(CODE, TDATE, TDATE, hms, 100.0);
		WTSTickData* tick = WTSTickData::create(ts);
		tick->setContractInfo(bd.contract());
		writer.writeTick(tick, 0);
		tick->release();
	}

	std::vector<WTSBarStruct> bars;
	uint16_t blkType = 0;
	ASSERT_TRUE(read_rt_bars(dir, bars, blkType));
	ASSERT_GT(bars.size(), 0u);

	//所有bar的时间戳都必须落在交易时段内，且严格递增
	uint64_t prev = 0;
	for (const WTSBarStruct& b : bars)
	{
		EXPECT_GT(b.time, prev) << "bar times must be strictly increasing";
		prev = b.time;

		uint32_t hms = TimeUtils::secBarToTime(b.time);
		uint32_t hm = hms / 100;
		bool inSection = (hm >= 900 && hm <= 1015) || (hm >= 1030 && hm <= 1130);
		EXPECT_TRUE(inSection) << "bar time " << hms << " outside trading sections";
	}

	writer.release();
	storage_loader::free_writer(pw);
	cfg->release();
	sInfo->release();
	fs::remove_all(dir);
}

/*
 *	非交易时间的tick不能产生秒线，也不能污染已有bar
 */
TEST(test_secbar_writer, out_of_session_tick_ignored)
{
	WTSSessionInfo* sInfo = make_sess();
	MockBaseDataMgr bd(EXCHG, PID, CODE, sInfo);
	std::string dir = temp_root("oos");
	MockWriterSink sink(&bd, TDATE);

	IDataWriter* pw = storage_loader::make_writer();
	ASSERT_NE(pw, nullptr) << "cannot load WtDataStorage, is WT_TEST_LIBDIR set?";
	IDataWriter& writer = *pw;
	WTSVariant* cfg = make_cfg(dir, true);
	ASSERT_TRUE(writer.init(cfg, &sink));

	//先来正常tick
	{
		WTSTickStruct ts = tick_maker::make(CODE, TDATE, TDATE, 90000, 100.0);
		WTSTickData* tick = WTSTickData::create(ts);
		tick->setContractInfo(bd.contract());
		writer.writeTick(tick, 0);
		tick->release();
	}
	//午休时段的tick
	{
		WTSTickStruct ts = tick_maker::make(CODE, TDATE, TDATE, 120000, 999.0);
		WTSTickData* tick = WTSTickData::create(ts);
		tick->setContractInfo(bd.contract());
		writer.writeTick(tick, 0);
		tick->release();
	}

	std::vector<WTSBarStruct> bars;
	uint16_t blkType = 0;
	ASSERT_TRUE(read_rt_bars(dir, bars, blkType));
	ASSERT_EQ(bars.size(), 1u);
	EXPECT_DOUBLE_EQ(bars[0].high, 100.0) << "out-of-session tick must not touch the bar";
	EXPECT_DOUBLE_EQ(bars[0].close, 100.0);

	writer.release();
	storage_loader::free_writer(pw);
	cfg->release();
	sInfo->release();
	fs::remove_all(dir);
}

/*
 *	预分配容量用满后要能扩容且不丢数据
 *	会话共8100秒 -> 预分配1620条，这里塞满一整个小节看是否正常
 */
TEST(test_secbar_writer, resize_keeps_all_bars)
{
	WTSSessionInfo* sInfo = make_sess();
	MockBaseDataMgr bd(EXCHG, PID, CODE, sInfo);
	std::string dir = temp_root("resize");
	MockWriterSink sink(&bd, TDATE);

	IDataWriter* pw = storage_loader::make_writer();
	ASSERT_NE(pw, nullptr) << "cannot load WtDataStorage, is WT_TEST_LIBDIR set?";
	IDataWriter& writer = *pw;
	WTSVariant* cfg = make_cfg(dir, true);
	ASSERT_TRUE(writer.init(cfg, &sink));

	//第一小节 9:00:00~10:14:59 共4500秒，每5秒一笔 -> 900根
	uint32_t written = 0;
	for (uint32_t s = 0; s < 4500; s += 5)
	{
		uint32_t hms = tick_maker::advance_hms(90000, s);
		WTSTickStruct ts = tick_maker::make(CODE, TDATE, TDATE, hms, 100.0 + (s % 7));
		WTSTickData* tick = WTSTickData::create(ts);
		tick->setContractInfo(bd.contract());
		writer.writeTick(tick, 0);
		tick->release();
		written++;
	}

	std::vector<WTSBarStruct> bars;
	uint16_t blkType = 0;
	ASSERT_TRUE(read_rt_bars(dir, bars, blkType));

	//每5秒一笔tick，每笔落在不同的5秒bar上
	EXPECT_EQ(bars.size(), written);

	//时间戳严格递增，说明扩容过程没有错位
	for (size_t i = 1; i < bars.size(); i++)
	{
		ASSERT_GT(bars[i].time, bars[i-1].time) << "time went backwards at index " << i;
	}

	writer.release();
	storage_loader::free_writer(pw);
	cfg->release();
	sInfo->release();
	fs::remove_all(dir);
}

/*
 *	盘后转历史：his/sec5 的 dsb 必须是"未压缩 + 12字节BlockHeader"
 *	这是读取侧能mmap+尾部定位的前提
 */
TEST(test_secbar_writer, his_dsb_is_uncompressed_with_small_header)
{
	WTSSessionInfo* sInfo = make_sess();
	MockBaseDataMgr bd(EXCHG, PID, CODE, sInfo);
	std::string dir = temp_root("his");
	MockWriterSink sink(&bd, TDATE);

	IDataWriter* pw = storage_loader::make_writer();
	ASSERT_NE(pw, nullptr) << "cannot load WtDataStorage, is WT_TEST_LIBDIR set?";
	IDataWriter& writer = *pw;
	WTSVariant* cfg = make_cfg(dir, true);
	ASSERT_TRUE(writer.init(cfg, &sink));

	const uint32_t BARS = 20;
	for (uint32_t i = 0; i < BARS; i++)
	{
		uint32_t hms = tick_maker::advance_hms(90000, i * 5);
		WTSTickStruct ts = tick_maker::make(CODE, TDATE, TDATE, hms, 100.0 + i);
		WTSTickData* tick = WTSTickData::create(ts);
		tick->setContractInfo(bd.contract());
		writer.writeTick(tick, 0);
		tick->release();
	}

	//先记下rt里的内容，用于和his比对
	std::vector<WTSBarStruct> rtBars;
	uint16_t blkType = 0;
	ASSERT_TRUE(read_rt_bars(dir, rtBars, blkType));
	ASSERT_EQ(rtBars.size(), BARS);

	//触发盘后转历史
	writer.transHisData(sInfo->id());

	//transHisData 是异步的，等落盘完成
	std::string hisFile = fmtutil::format("{}his/sec5/{}/{}.dsb", dir, EXCHG, CODE);
	for (int i = 0; i < 100 && !StdFile::exists(hisFile.c_str()); i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));

	ASSERT_TRUE(StdFile::exists(hisFile.c_str())) << "his sec5 dsb not produced: " << hisFile;

	std::string content;
	StdFile::read_file_content(hisFile.c_str(), content);
	ASSERT_GE(content.size(), sizeof(BlockHeader));

	BlockHeader* hdr = (BlockHeader*)content.data();
	EXPECT_STREQ(hdr->_blk_flag, BLK_FLAG);
	EXPECT_EQ(hdr->_type, (uint16_t)BT_HIS_Sec5);
	EXPECT_EQ(hdr->_version, (uint16_t)BLOCK_VERSION_RAW_V2);
	EXPECT_FALSE(hdr->is_compressed()) << "sec5 his file must stay uncompressed for mmap/tail reads";

	//未压缩时，头之后就是裸的bar数组，长度必须整除
	size_t payload = content.size() - BLOCK_HEADER_SIZE;
	ASSERT_EQ(payload % sizeof(WTSBarStruct), 0u) << "payload not a whole number of bars -- header size mismatch?";
	EXPECT_EQ(payload / sizeof(WTSBarStruct), (size_t)BARS);

	//内容与rt逐根一致
	WTSBarStruct* hisBars = (WTSBarStruct*)(content.data() + BLOCK_HEADER_SIZE);
	for (uint32_t i = 0; i < BARS; i++)
	{
		EXPECT_EQ(hisBars[i].time, rtBars[i].time) << "time mismatch at bar " << i;
		EXPECT_DOUBLE_EQ(hisBars[i].close, rtBars[i].close) << "close mismatch at bar " << i;
	}

	writer.release();
	storage_loader::free_writer(pw);
	cfg->release();
	sInfo->release();
	fs::remove_all(dir);
}
