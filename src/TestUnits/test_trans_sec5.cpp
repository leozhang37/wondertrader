/*!
 * \file test_trans_sec5.cpp
 * \brief 历史tick转5秒线(WtDtHelper::trans_ticks_to_sec5)的测试
 *
 * 核心是对账：同一批tick喂给 WtDataWriter 实盘落盘，
 * 再用离线工具从它落下的 his/ticks 重新生成，两边的 his/sec5 必须逐字节一致。
 */
#include <stdio.h>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#include "mock_datasink.hpp"
#include "tick_maker.hpp"
#include "storage_loader.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/fmtlib.h"
#include "gtest/gtest/gtest.h"

#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

USING_NS_WTP;

namespace
{
	const char* EXCHG = "TEST";

	typedef void(*FuncLog)(const char* message);
	typedef uint32_t(*FuncTransSec5)(const char*, const char*, const char*, const char*,
		const char*, uint32_t, uint32_t, const char*, FuncLog);

	void print_log(const char* message)
	{
		if (getenv("WT_TEST_VERBOSE") != NULL)
			printf("[dthelper] %s\n", message);
	}

	//WtDtHelper 输出在 bin/WtDtPorter 下，和其他存储插件一样走 dlopen
	FuncTransSec5 get_trans_func()
	{
		static FuncTransSec5 func = NULL;
		if (func != NULL)
			return func;

		std::string dir = storage_loader::lib_dir();
		std::string path = dir + "WtDtPorter/" + DLLHelper::wrap_module("WtDtHelper");
		DllHandle h = DLLHelper::load_library(path.c_str());
		if (h == NULL)
		{
			path = dir + DLLHelper::wrap_module("WtDtHelper");
			h = DLLHelper::load_library(path.c_str());
		}
		if (h == NULL)
		{
			printf("[loader] failed to load WtDtHelper\n");
			return NULL;
		}

		func = (FuncTransSec5)DLLHelper::get_symbol(h, "trans_ticks_to_sec5");
		return func;
	}

	std::string temp_root(const char* tag)
	{
		std::string dir = fmtutil::format("./ut_trans_sec5_{}/", tag);
		if (fs::exists(dir))
			fs::remove_all(dir);
		fs::create_directories(dir);
		return dir;
	}

	//TD：日盘两节；TN：夜盘跨午夜，offset=300 与 dist/common 里的 FN0230 同构
	WTSSessionInfo* make_day_sess()
	{
		WTSSessionInfo* s = WTSSessionInfo::create("TD", "TD", 0);
		s->addTradingSection(900, 1015);
		s->addTradingSection(1030, 1130);
		return s;
	}

	WTSSessionInfo* make_night_sess()
	{
		WTSSessionInfo* s = WTSSessionInfo::create("TN", "TN", 300);
		s->addTradingSection(2100, 230);
		s->addTradingSection(900, 1015);
		return s;
	}

	//写工具要用的 sessions.json 和 commodities.json，内容与上面两个会话一致
	void write_base_files(const std::string& dir)
	{
		std::string sess = R"({
	"TD": {"name":"TD","offset":0,"sections":[{"from":900,"to":1015},{"from":1030,"to":1130}]},
	"TN": {"name":"TN","offset":300,"sections":[{"from":2100,"to":230},{"from":900,"to":1015}]}
})";
		std::string comm = R"({
	"TEST": {
		"rb": {"name":"rb","session":"TD","holiday":"","pricetick":1,"volscale":10,"category":1,"covermode":0,"pricemode":0},
		"hc": {"name":"hc","session":"TD","holiday":"","pricetick":1,"volscale":10,"category":1,"covermode":0,"pricemode":0},
		"jm": {"name":"jm","session":"TN","holiday":"","pricetick":1,"volscale":10,"category":1,"covermode":0,"pricemode":0}
	},
	"CZCE": {
		"AP": {"name":"AP","session":"TD","holiday":"","pricetick":1,"volscale":10,"category":1,"covermode":0,"pricemode":0}
	}
})";
		StdFile::write_file_content((dir + "sessions.json").c_str(), sess);
		StdFile::write_file_content((dir + "commodities.json").c_str(), comm);
	}

	/*
	 *	造一条"行情快照"式的tick：high/low/total_volume 都是当日累计值，
	 *	这样 updateCache 的新高新低判断和成交量校验才会按真实行情的方式工作
	 */
	struct TickSeq
	{
		std::string	_exchg;
		std::string	_code;
		uint32_t	_tdate;
		double		_hi = 0, _lo = 0, _tvol = 0, _tturn = 0;
		std::vector<WTSTickStruct>	_ticks;

		TickSeq(const char* exchg, const char* code, uint32_t tdate) : _exchg(exchg), _code(code), _tdate(tdate) {}

		void add(uint32_t adate, uint32_t hms, double price, double vol = 1, uint32_t msec = 0)
		{
			WTSTickStruct ts;
			memset(&ts, 0, sizeof(ts));
			strcpy(ts.exchg, _exchg.c_str());
			strcpy(ts.code, _code.c_str());
			ts.trading_date = _tdate;
			ts.action_date = adate;
			ts.action_time = hms * 1000 + msec;
			ts.price = price;

			//price为0的异常tick不参与当日高低
			if (price > 0)
			{
				_hi = (_hi == 0) ? price : std::max(_hi, price);
				_lo = (_lo == 0) ? price : std::min(_lo, price);
			}
			ts.open = price;
			ts.high = _hi;
			ts.low = _lo;

			double turn = vol * price * 10;
			_tvol += vol;
			_tturn += turn;
			ts.volume = vol;
			ts.turn_over = turn;
			ts.total_volume = _tvol;
			ts.total_turnover = _tturn;
			ts.open_interest = 10000 + _tvol;
			ts.diff_interest = vol;
			ts.bid_prices[0] = price - 1;
			ts.ask_prices[0] = price + 1;
			_ticks.emplace_back(ts);
		}
	};

	//写 his/ticks/<exchg>/<date>/<code>.dsb，未压缩格式
	void write_tick_file(const std::string& tickRoot, const TickSeq& seq)
	{
		std::string dir = fmtutil::format("{}{}/{}/", tickRoot, seq._exchg, seq._tdate);
		fs::create_directories(dir);

		BlockHeader header;
		memset(&header, 0, sizeof(header));
		strcpy(header._blk_flag, BLK_FLAG);
		header._type = BT_HIS_Ticks;
		header._version = BLOCK_VERSION_RAW_V2;

		std::string content((const char*)&header, sizeof(header));
		content.append((const char*)seq._ticks.data(), sizeof(WTSTickStruct)*seq._ticks.size());
		StdFile::write_file_content((dir + seq._code + ".dsb").c_str(), content);
	}

	//读 sec5 历史文件，顺带校验文件头
	bool read_sec5(const std::string& filename, std::vector<WTSBarStruct>& bars)
	{
		bars.clear();
		if (!StdFile::exists(filename.c_str()))
			return false;

		std::string content;
		StdFile::read_file_content(filename.c_str(), content);
		if (content.size() < BLOCK_HEADER_SIZE)
			return false;

		BlockHeader* hdr = (BlockHeader*)content.data();
		EXPECT_STREQ(hdr->_blk_flag, BLK_FLAG);
		EXPECT_EQ(hdr->_type, (uint16_t)BT_HIS_Sec5);
		EXPECT_EQ(hdr->_version, (uint16_t)BLOCK_VERSION_RAW_V2);
		EXPECT_FALSE(hdr->is_compressed());

		size_t payload = content.size() - BLOCK_HEADER_SIZE;
		EXPECT_EQ(payload % sizeof(WTSBarStruct), 0u);
		const WTSBarStruct* first = (const WTSBarStruct*)(content.data() + BLOCK_HEADER_SIZE);
		bars.assign(first, first + payload / sizeof(WTSBarStruct));
		return true;
	}

	void write_sec5(const std::string& filename, const std::vector<WTSBarStruct>& bars)
	{
		fs::create_directories(fs::path(filename).parent_path());
		BlockHeader header;
		memset(&header, 0, sizeof(header));
		strcpy(header._blk_flag, BLK_FLAG);
		header._type = BT_HIS_Sec5;
		header._version = BLOCK_VERSION_RAW_V2;
		std::string content((const char*)&header, sizeof(header));
		content.append((const char*)bars.data(), sizeof(WTSBarStruct)*bars.size());
		StdFile::write_file_content(filename.c_str(), content);
	}

	uint32_t run_trans(const std::string& dir, const char* filter, uint32_t sDate = 0, uint32_t eDate = 0, const char* options = "")
	{
		FuncTransSec5 f = get_trans_func();
		if (f == NULL)
		{
			ADD_FAILURE() << "cannot load WtDtHelper, is WT_TEST_LIBDIR set?";
			return 0;
		}
		std::string tickRoot = dir + "his/ticks/";
		std::string outRoot = dir + "out/";
		std::string comm = dir + "commodities.json";
		std::string sess = dir + "sessions.json";
		return f(tickRoot.c_str(), outRoot.c_str(), comm.c_str(), sess.c_str(), filter, sDate, eDate, options, print_log);
	}

	std::set<uint32_t> bar_dates(const std::vector<WTSBarStruct>& bars)
	{
		std::set<uint32_t> ret;
		for (const WTSBarStruct& bar : bars)
			ret.insert(bar.date);
		return ret;
	}

	/*
	 *	对账：tick 先经 WtDataWriter 落盘(得到 his/ticks 和 his/sec5)，
	 *	再用工具从 his/ticks 重新生成，与 writer 的 his/sec5 逐字节比较
	 */
	void check_parity(const char* tag, const char* pid, const char* code, WTSSessionInfo* sInfo,
		const TickSeq& seq, bool skipNoTradeTick, uint32_t minPriceMode)
	{
		MockBaseDataMgr bd(EXCHG, pid, code, sInfo);
		std::string dir = temp_root(tag);
		write_base_files(dir);
		MockWriterSink sink(&bd, seq._tdate);

		IDataWriter* pw = storage_loader::make_writer();
		ASSERT_NE(pw, nullptr) << "cannot load WtDataStorage, is WT_TEST_LIBDIR set?";

		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("path", dir.c_str());
		cfg->append("async", false);
		cfg->append("enablesec5", true);
		cfg->append("disablemin1", true);
		cfg->append("disablemin5", true);
		cfg->append("disableday", true);
		cfg->append("skip_notrade_tick", skipNoTradeTick);
		cfg->append("minbar_price_mode", minPriceMode);
		ASSERT_TRUE(pw->init(cfg, &sink));

		for (WTSTickStruct ts : seq._ticks)
		{
			WTSTickData* tick = WTSTickData::create(ts);
			tick->setContractInfo(bd.contract());
			pw->writeTick(tick, 0);
			tick->release();
		}

		pw->transHisData(sInfo->id());

		std::string writerSec5 = fmtutil::format("{}his/sec5/{}/{}.dsb", dir, EXCHG, code);
		std::string tickFile = fmtutil::format("{}his/ticks/{}/{}/{}.dsb", dir, EXCHG, seq._tdate, code);
		for (int i = 0; i < 100 && !(StdFile::exists(writerSec5.c_str()) && StdFile::exists(tickFile.c_str())); i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		//文件出现后再给一点时间写完
		std::this_thread::sleep_for(std::chrono::milliseconds(100));

		pw->release();
		storage_loader::free_writer(pw);
		cfg->release();

		ASSERT_TRUE(StdFile::exists(tickFile.c_str())) << "writer did not dump ticks: " << tickFile;

		std::vector<WTSBarStruct> expected;
		ASSERT_TRUE(read_sec5(writerSec5, expected)) << "writer did not dump sec5: " << writerSec5;
		ASSERT_GT(expected.size(), 0u);

		std::string options = fmtutil::format(R"({{"skip_notrade_tick":{},"minbar_price_mode":{}}})",
			skipNoTradeTick ? "true" : "false", minPriceMode);
		uint32_t cnt = run_trans(dir, "", 0, 0, options.c_str());
		EXPECT_EQ(cnt, (uint32_t)expected.size());

		std::vector<WTSBarStruct> actual;
		ASSERT_TRUE(read_sec5(fmtutil::format("{}out/{}/{}.dsb", dir, EXCHG, code), actual));
		ASSERT_EQ(actual.size(), expected.size());
		for (size_t i = 0; i < expected.size(); i++)
		{
			EXPECT_EQ(memcmp(&actual[i], &expected[i], sizeof(WTSBarStruct)), 0)
				<< "bar " << i << " differs: time " << actual[i].time << " vs " << expected[i].time
				<< ", o/h/l/c " << actual[i].open << "/" << actual[i].high << "/" << actual[i].low << "/" << actual[i].close
				<< " vs " << expected[i].open << "/" << expected[i].high << "/" << expected[i].low << "/" << expected[i].close
				<< ", vol " << actual[i].vol << " vs " << expected[i].vol
				<< ", money " << actual[i].money << " vs " << expected[i].money
				<< ", hold " << actual[i].hold << " vs " << expected[i].hold
				<< ", add " << actual[i].add << " vs " << expected[i].add
				<< ", settle " << actual[i].settle << " vs " << expected[i].settle
				<< ", reserve " << actual[i].reserve_ << " vs " << expected[i].reserve_;
		}

		fs::remove_all(dir);
	}

	//日盘：覆盖小节边界、超时tick、零成交、price为0、同一时间戳、倒序、时段外
	TickSeq make_day_ticks(const char* code, uint32_t tdate)
	{
		TickSeq seq(EXCHG, code, tdate);
		seq.add(tdate, 85900, 100);					//集合竞价
		const double px[] = { 100, 101, 99, 102, 98, 100, 103, 97, 100, 101 };
		for (uint32_t i = 0; i < 40; i++)
			seq.add(tdate, tick_maker::advance_hms(90000, i), px[i % 10], (i % 7 == 3) ? 0 : 1);	//夹杂零成交
		seq.add(tdate, 90040, 0);						//price为0
		seq.add(tdate, 90041, 104);
		seq.add(tdate, 90041, 105);						//同一时间戳，writer会+200ms
		seq.add(tdate, 90039, 96);						//时间倒序
		for (uint32_t i = 0; i < 20; i++)
			seq.add(tdate, tick_maker::advance_hms(101450, i), 100 + (i % 3));
		seq.add(tdate, 101500, 106, 1, 500);			//小节末尾超时
		seq.add(tdate, 101700, 100);					//小节间休
		for (uint32_t i = 0; i < 15; i++)
			seq.add(tdate, tick_maker::advance_hms(103000, i * 2), 100 - (i % 4));
		seq.add(tdate, 113000, 95, 1, 500);				//收盘超时
		seq.add(tdate, 113500, 95);						//收盘后
		return seq;
	}
}

TEST(test_trans_sec5, parity_with_writer_day_session)
{
	WTSSessionInfo* sInfo = make_day_sess();
	check_parity("parity_day", "rb", "rb2610", sInfo, make_day_ticks("rb2610", 20260921), false, 0);
	sInfo->release();
}

TEST(test_trans_sec5, parity_with_writer_skip_notrade_and_bidask)
{
	//skipnotradetick + minpricemode=1 这两个选项下也要一致
	WTSSessionInfo* sInfo = make_day_sess();
	check_parity("parity_opt", "rb", "rb2610", sInfo, make_day_ticks("rb2610", 20260921), true, 1);
	sInfo->release();
}

TEST(test_trans_sec5, parity_with_writer_night_session)
{
	WTSSessionInfo* sInfo = make_night_sess();
	const uint32_t tdate = 20260922;	//周一夜盘归周二交易日
	TickSeq seq(EXCHG, "jm2601", tdate);
	seq.add(20260921, 205900, 1000);						//集合竞价
	for (uint32_t i = 0; i < 20; i++)
		seq.add(20260921, tick_maker::advance_hms(210000, i), 1000 + (i % 5));
	for (uint32_t i = 0; i < 20; i++)						//跨午夜
		seq.add(i < 10 ? 20260921 : 20260922, tick_maker::advance_hms(235950, i), 1010 - (i % 6));
	for (uint32_t i = 0; i < 10; i++)
		seq.add(20260922, tick_maker::advance_hms(22955, i), 1005);
	seq.add(20260922, 23000, 1003, 1, 500);				//夜盘收盘超时
	for (uint32_t i = 0; i < 10; i++)
		seq.add(20260922, tick_maker::advance_hms(90000, i), 1020 + i);

	check_parity("parity_night", "jm", "jm2601", sInfo, seq, false, 0);
	sInfo->release();
}

TEST(test_trans_sec5, filter_by_product_and_code)
{
	std::string dir = temp_root("filter");
	write_base_files(dir);
	std::string tickRoot = dir + "his/ticks/";
	for (const char* code : { "rb2610", "rb2701", "hc2610" })
		write_tick_file(tickRoot, make_day_ticks(code, 20260921));

	//按品种：rb 的两个月份都转，hc 不转
	EXPECT_GT(run_trans(dir, "TEST.rb"), 0u);
	std::vector<WTSBarStruct> bars;
	EXPECT_TRUE(read_sec5(dir + "out/TEST/rb2610.dsb", bars));
	EXPECT_TRUE(read_sec5(dir + "out/TEST/rb2701.dsb", bars));
	EXPECT_FALSE(StdFile::exists((dir + "out/TEST/hc2610.dsb").c_str()));

	//按合约：只转一个
	fs::remove_all(dir + "out/");
	EXPECT_GT(run_trans(dir, "TEST.rb2610, TEST.hc"), 0u);
	EXPECT_TRUE(StdFile::exists((dir + "out/TEST/rb2610.dsb").c_str()));
	EXPECT_FALSE(StdFile::exists((dir + "out/TEST/rb2701.dsb").c_str()));
	EXPECT_TRUE(StdFile::exists((dir + "out/TEST/hc2610.dsb").c_str()));

	//前缀和缺交易所的写法都不能命中
	fs::remove_all(dir + "out/");
	EXPECT_EQ(run_trans(dir, "TEST.r, rb, TEST.rb26"), 0u);
	EXPECT_FALSE(fs::exists(dir + "out/TEST/"));

	fs::remove_all(dir);
}

TEST(test_trans_sec5, date_range_inclusive)
{
	std::string dir = temp_root("range");
	write_base_files(dir);
	std::string tickRoot = dir + "his/ticks/";
	for (uint32_t d : { 20260918u, 20260921u, 20260922u })
		write_tick_file(tickRoot, make_day_ticks("rb2610", d));

	EXPECT_GT(run_trans(dir, "TEST.rb", 20260921, 20260922), 0u);

	std::vector<WTSBarStruct> bars;
	ASSERT_TRUE(read_sec5(dir + "out/TEST/rb2610.dsb", bars));
	EXPECT_EQ(bar_dates(bars), (std::set<uint32_t>{ 20260921, 20260922 }));

	//按时间升序
	for (size_t i = 1; i < bars.size(); i++)
		EXPECT_LT(bars[i - 1].time, bars[i].time);

	fs::remove_all(dir);
}

TEST(test_trans_sec5, merge_keeps_existing_days_by_default)
{
	std::string dir = temp_root("merge_skip");
	write_base_files(dir);
	std::string tickRoot = dir + "his/ticks/";
	for (uint32_t d : { 20260921u, 20260922u })
		write_tick_file(tickRoot, make_day_ticks("rb2610", d));

	//已有文件里放一根 20260921 的"实盘"bar，用 close=999 做标记
	WTSBarStruct marker;
	marker.date = 20260921;
	marker.time = TimeUtils::timeToSecBar(20260921, 90005);
	marker.close = 999;
	std::string outFile = dir + "out/TEST/rb2610.dsb";
	write_sec5(outFile, { marker });

	EXPECT_GT(run_trans(dir, "TEST.rb"), 0u);

	std::vector<WTSBarStruct> bars;
	ASSERT_TRUE(read_sec5(outFile, bars));
	EXPECT_EQ(bar_dates(bars), (std::set<uint32_t>{ 20260921, 20260922 }));

	uint32_t day1 = 0;
	for (const WTSBarStruct& bar : bars)
	{
		if (bar.date == 20260921)
		{
			day1++;
			EXPECT_DOUBLE_EQ(bar.close, 999.0) << "existing day must be kept as-is";
		}
	}
	EXPECT_EQ(day1, 1u);

	fs::remove_all(dir);
}

TEST(test_trans_sec5, merge_overwrite_replaces_days)
{
	std::string dir = temp_root("merge_ow");
	write_base_files(dir);
	std::string tickRoot = dir + "his/ticks/";
	write_tick_file(tickRoot, make_day_ticks("rb2610", 20260921));

	WTSBarStruct marker;
	marker.date = 20260921;
	marker.time = TimeUtils::timeToSecBar(20260921, 90005);
	marker.close = 999;
	//另一个交易日的数据不在这次转换范围内，必须保留
	WTSBarStruct other;
	other.date = 20260918;
	other.time = TimeUtils::timeToSecBar(20260918, 90005);
	other.close = 888;
	std::string outFile = dir + "out/TEST/rb2610.dsb";
	write_sec5(outFile, { other, marker });

	EXPECT_GT(run_trans(dir, "TEST.rb", 0, 0, R"({"overwrite":true})"), 0u);

	std::vector<WTSBarStruct> bars;
	ASSERT_TRUE(read_sec5(outFile, bars));
	EXPECT_EQ(bar_dates(bars), (std::set<uint32_t>{ 20260918, 20260921 }));
	ASSERT_FALSE(bars.empty());
	EXPECT_DOUBLE_EQ(bars.front().close, 888.0);
	for (const WTSBarStruct& bar : bars)
		EXPECT_NE(bar.close, 999.0) << "overwritten day must not keep old bars";

	fs::remove_all(dir);
}

TEST(test_trans_sec5, czce_alt_code_unified_to_new_code)
{
	std::string dir = temp_root("czce");
	write_base_files(dir);
	std::string tickRoot = dir + "his/ticks/";

	//老的3位代码文件 AP601 和新的4位代码文件 AP2601 是同一个合约
	TickSeq oldSeq = make_day_ticks("AP601", 20251110);
	oldSeq._exchg = "CZCE";
	write_tick_file(tickRoot, oldSeq);
	TickSeq newSeq = make_day_ticks("AP2601", 20251111);
	newSeq._exchg = "CZCE";
	write_tick_file(tickRoot, newSeq);

	//输出目录里还有一个老代码的sec5文件，要并进新文件并删掉
	WTSBarStruct oldBar;
	oldBar.date = 20251031;
	oldBar.time = TimeUtils::timeToSecBar(20251031, 90005);
	oldBar.close = 777;
	write_sec5(dir + "out/CZCE/AP601.dsb", { oldBar });

	EXPECT_GT(run_trans(dir, "CZCE.AP"), 0u);

	std::vector<WTSBarStruct> bars;
	ASSERT_TRUE(read_sec5(dir + "out/CZCE/AP2601.dsb", bars));
	EXPECT_EQ(bar_dates(bars), (std::set<uint32_t>{ 20251031, 20251110, 20251111 }));
	EXPECT_FALSE(StdFile::exists((dir + "out/CZCE/AP601.dsb").c_str()));

	//按老代码写过滤条件也能命中
	fs::remove_all(dir + "out/");
	EXPECT_GT(run_trans(dir, "CZCE.AP601"), 0u);
	EXPECT_TRUE(StdFile::exists((dir + "out/CZCE/AP2601.dsb").c_str()));

	fs::remove_all(dir);
}

TEST(test_trans_sec5, czce_decade_inference)
{
	//2029年交易的 AP001 是2030年的合约，不能推成2020
	std::string dir = temp_root("czce_decade");
	write_base_files(dir);
	TickSeq seq = make_day_ticks("AP001", 20291112);
	seq._exchg = "CZCE";
	write_tick_file(dir + "his/ticks/", seq);

	EXPECT_GT(run_trans(dir, "CZCE.AP"), 0u);
	EXPECT_TRUE(StdFile::exists((dir + "out/CZCE/AP3001.dsb").c_str()));
	EXPECT_FALSE(StdFile::exists((dir + "out/CZCE/AP2001.dsb").c_str()));

	fs::remove_all(dir);
}

TEST(test_trans_sec5, bad_inputs_return_zero)
{
	std::string dir = temp_root("bad");
	write_base_files(dir);
	write_tick_file(dir + "his/ticks/", make_day_ticks("rb2610", 20260921));

	//选项不是合法JSON
	EXPECT_EQ(run_trans(dir, "", 0, 0, "{bad json"), 0u);
	//配置文件不存在
	FuncTransSec5 f = get_trans_func();
	ASSERT_NE(f, nullptr);
	EXPECT_EQ(f((dir + "his/ticks/").c_str(), (dir + "out/").c_str(), "nope.json", "nope.json", "", 0, 0, "", print_log), 0u);
	//品种没有配置
	write_tick_file(dir + "his/ticks/", make_day_ticks("zz2610", 20260921));
	EXPECT_EQ(run_trans(dir, "TEST.zz"), 0u);
	EXPECT_FALSE(StdFile::exists((dir + "out/TEST/zz2610.dsb").c_str()));

	fs::remove_all(dir);
}
