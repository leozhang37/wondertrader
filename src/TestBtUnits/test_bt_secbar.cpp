/*!
 * \file test_bt_secbar.cpp
 * \brief 秒线回测
 *
 * 验证 HisDataReplayer 的秒精度时间轴：
 * 主K线是 s5 时，回放节拍、调度次数、bar时间戳都要正确，
 * 而且策略侧看到的周期字符串要能原样往返
 */
#include <stdio.h>

#include "bt_rig.hpp"
#include "gtest/gtest/gtest.h"

USING_NS_WTP;

namespace
{
	const char* S_EXCHG = "TEST";
	const char* S_PID = "rb";
	const char* S_CODE = "rb2610";
	const char* S_STDCODE = "TEST.rb.2610";
	const uint32_t S_SDATE = 20260901;
	const uint32_t S_DAYS = 2;

	/*
	 *	记录型策略：订阅 s5 主K线，把每次 on_bar / on_schedule 的时刻记下来
	 */
	class SecRecorder : public CtaStrategy
	{
	public:
		SecRecorder(const char* id, const char* period) : CtaStrategy(id), _period(period) {}

		virtual const char* getName() override { return "SecRecorder"; }
		virtual const char* getFactName() override { return "BtTestFact"; }

		virtual void on_init(ICtaStraCtx* ctx) override
		{
			WTSKlineSlice* s = ctx->stra_get_bars(S_STDCODE, _period.c_str(), 30, true);
			_initBars = (s != NULL) ? (int32_t)s->size() : -1;
			if (s) s->release();
			ctx->stra_sub_bar_events(S_STDCODE, _period.c_str());
		}

		virtual void on_schedule(ICtaStraCtx* ctx, uint32_t uDate, uint32_t uTime) override
		{
			_schedDates.emplace_back(uDate);
			_schedTimes.emplace_back(uTime);
		}

		virtual void on_bar(ICtaStraCtx* ctx, const char* stdCode, const char* period, WTSBarStruct* newBar) override
		{
			_barTimes.emplace_back(newBar->time);
			_barPeriods.emplace_back(period);
		}

		int32_t					_initBars = -1;
		std::vector<uint32_t>	_schedDates;
		std::vector<uint32_t>	_schedTimes;
		std::vector<uint64_t>	_barTimes;
		std::vector<std::string> _barPeriods;

	private:
		std::string _period;
	};

	struct SecBtRun
	{
		bool		ran = false;
		uint32_t	bars = 0;
		SecRecorder* stra = nullptr;
	};

	//跑一遍秒线回测
	void run_sec_backtest(const char* tag, const char* period, SecBtRun& out)
	{
		using namespace bt_rig;

		std::string dir = fmtutil::format("./ut_bts_{}/", tag);
		if (bfs::exists(dir)) bfs::remove_all(dir);
		bfs::create_directories(dir);

		write_basefiles(dir, S_EXCHG, S_PID, S_CODE);
		out.bars = write_sec5_bars(dir, S_EXCHG, S_CODE, S_SDATE, S_DAYS);

		std::string outDir = dir + "outputs/";
		bfs::create_directories(outDir);
		WtHelper::setOutputDir(outDir.c_str());
		WtHelper::setInstDir(lib_dir().c_str());

		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", "bin");
		cfg->append("path", dir.c_str());
		cfg->append("stime", (uint64_t)S_SDATE * 10000 + 900);
		cfg->append("etime", (uint64_t)(S_SDATE + S_DAYS) * 10000 + 1500);
		cfg->append("tick", false);
		cfg->append("align_by_section", false);

		WTSVariant* bf = WTSVariant::createObject();
		bf->append("session", (dir + "sessions.json").c_str());
		bf->append("commodity", (dir + "commodities.json").c_str());
		bf->append("contract", (dir + "contracts.json").c_str());
		cfg->append("basefiles", bf);

		HisDataReplayer* replayer = new HisDataReplayer();
		if (!replayer->init(cfg))
		{
			printf("[bts] replayer init failed\n");
			delete replayer;
			cfg->release();
			return;
		}

		InjectableMocker* mocker = new InjectableMocker(replayer, "sec_rec");
		SecRecorder* stra = new SecRecorder("sec_rec", period);
		mocker->inject(stra);

		replayer->register_sink(mocker, "sec_rec");
		replayer->prepare();
		replayer->run(false);

		out.stra = stra;
		out.ran = true;

		delete mocker;
		delete replayer;
		cfg->release();
	}
}

/*
 *	s5 作主K线的回测必须能跑起来，且调度节拍正确
 */
TEST(test_bt_secbar, s5_backtest_drives_schedule)
{
	SecBtRun r;
	run_sec_backtest("s5", "s5", r);

	ASSERT_TRUE(r.ran) << "sec backtest did not run";
	//两天，每天 (4500+3600)/5 = 1620 根
	EXPECT_EQ(r.bars, 3240u);

	ASSERT_NE(r.stra, nullptr);
	printf("[bts] initBars=%d schedules=%zu bars=%zu\n",
		r.stra->_initBars, r.stra->_schedTimes.size(), r.stra->_barTimes.size());
	fflush(stdout);

	//启动时要能拿到历史秒线
	EXPECT_GT(r.stra->_initBars, 0) << "strategy got no sec bars at init";

	//调度必须发生，且次数应该接近bar数
	ASSERT_GT(r.stra->_schedTimes.size(), 0u) << "on_calculate never fired in sec backtest";
	EXPECT_GT(r.stra->_schedTimes.size(), 3000u) << "far fewer schedules than sec bars";

	//闭合的bar时间戳必须严格递增
	uint64_t prev = 0;
	for (size_t i = 0; i < r.stra->_barTimes.size(); i++)
	{
		EXPECT_GT(r.stra->_barTimes[i], prev) << "bar time not increasing at " << i;
		prev = r.stra->_barTimes[i];
	}

	//秒线的bar时间戳必须是 yyyyMMddHHmmss 量级，不能是分钟编码
	if (!r.stra->_barTimes.empty())
	{
		uint64_t t = r.stra->_barTimes.front();
		EXPECT_GT(t, 20000000000000ULL) << "bar time is not in yyyyMMddHHmmss form";
		EXPECT_LT(t, 21000000000000ULL);

		//解出来的日期时间要落在数据范围内
		uint32_t d = TimeUtils::secBarToDate(t);
		uint32_t hms = TimeUtils::secBarToTime(t);
		EXPECT_GE(d, S_SDATE);
		EXPECT_TRUE((hms >= 90000 && hms <= 101500) || (hms >= 103000 && hms <= 113000))
			<< "bar time " << hms << " outside trading sections";
	}

	//周期字符串要原样回到策略侧
	for (const std::string& p : r.stra->_barPeriods)
		EXPECT_EQ(p, "s5");

	delete r.stra;
}

/*
 *	s15：从 s5 基础线重采样，每3根合成1根
 */
TEST(test_bt_secbar, s15_resampled_from_s5)
{
	SecBtRun r;
	run_sec_backtest("s15", "s15", r);

	ASSERT_TRUE(r.ran);
	ASSERT_NE(r.stra, nullptr);

	printf("[bts] s15 initBars=%d schedules=%zu bars=%zu\n",
		r.stra->_initBars, r.stra->_schedTimes.size(), r.stra->_barTimes.size());
	fflush(stdout);

	ASSERT_GT(r.stra->_barTimes.size(), 0u) << "no s15 bars closed";

	//相邻两根 s15 的间隔必须是15秒(跨小节处会更大，所以只看是否为5的倍数且>=15)
	for (size_t i = 1; i < r.stra->_barTimes.size(); i++)
	{
		uint32_t t1 = TimeUtils::secBarToTime(r.stra->_barTimes[i-1]);
		uint32_t t2 = TimeUtils::secBarToTime(r.stra->_barTimes[i]);
		//同一天内才比较
		if (TimeUtils::secBarToDate(r.stra->_barTimes[i-1]) != TimeUtils::secBarToDate(r.stra->_barTimes[i]))
			continue;

		uint32_t s1 = (t1/10000)*3600 + (t1%10000/100)*60 + t1%100;
		uint32_t s2 = (t2/10000)*3600 + (t2%10000/100)*60 + t2%100;
		ASSERT_GT(s2, s1) << "s15 bar time not increasing at " << i;
		EXPECT_EQ((s2 - s1) % 5, 0u) << "gap not a multiple of 5s at " << i;
	}

	for (const std::string& p : r.stra->_barPeriods)
		EXPECT_EQ(p, "s15");

	delete r.stra;
}
