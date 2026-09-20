/*!
 * \file test_bt_baseline.cpp
 * \brief 回测的 min1 回归基线
 *
 * P4 要改 HisDataReplayer 的时间轴，那是整个秒线改造里耦合最深的一处。
 * 这个测试的作用是安全网：用固定的数据跑一遍 min1 回测，
 * 把 trades/closes/funds 的输出固定下来。
 * 改造之后再跑，输出必须逐字节一致——只要 min1 的行为有任何漂移就会被发现。
 */
#include <stdio.h>

#include "bt_rig.hpp"
#include "gtest/gtest/gtest.h"

USING_NS_WTP;

namespace
{
	const char* B_EXCHG = "TEST";
	const char* B_PID = "rb";
	const char* B_CODE = "rb2610";
	const char* B_STDCODE = "TEST.rb.2610";
	const uint32_t B_SDATE = 20260901;
	const uint32_t B_DAYS = 12;

	struct BtResult
	{
		bool		ran = false;
		uint32_t	bars = 0;
		std::string	trades;
		std::string	closes;
		std::string	funds;
		std::string	signals;
		uint32_t	calls = 0;
	};

	/*
	 *	跑一遍 min1 回测，把输出收集回来
	 *	@tag	输出目录后缀，用于区分多次运行
	 */
	BtResult run_min1_backtest(const char* tag)
	{
		using namespace bt_rig;
		BtResult ret;

		std::string dir = fmtutil::format("./ut_bt_{}/", tag);
		if (bfs::exists(dir))
			bfs::remove_all(dir);
		bfs::create_directories(dir);

		write_basefiles(dir, B_EXCHG, B_PID, B_CODE);
		ret.bars = write_min1_bars(dir, B_EXCHG, B_CODE, B_SDATE, B_DAYS);

		std::string outDir = dir + "outputs/";
		bfs::create_directories(outDir);
		WtHelper::setOutputDir(outDir.c_str());
		WtHelper::setInstDir(lib_dir().c_str());

		//回测配置
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", "bin");
		cfg->append("path", dir.c_str());
		//跨度覆盖全部数据
		cfg->append("stime", (uint64_t)B_SDATE * 10000 + 900);
		cfg->append("etime", (uint64_t)(B_SDATE + B_DAYS) * 10000 + 1500);
		cfg->append("tick", false);
		cfg->append("align_by_section", false);

		WTSVariant* bf = WTSVariant::createObject();
		bf->append("session", (dir + "sessions.json").c_str());
		bf->append("commodity", (dir + "commodities.json").c_str());
		bf->append("contract", (dir + "contracts.json").c_str());
		cfg->append("basefiles", bf);

		HisDataReplayer replayer;
		if (!replayer.init(cfg))
		{
			printf("[bt] replayer init failed\n");
			cfg->release();
			return ret;
		}

		/*
		 *	注入测试策略。不走 init_cta_factory(dlopen策略工厂)，
		 *	基线要锁的是回测框架的行为，策略逻辑放在测试里才可控
		 */
		InjectableMocker* mocker = new InjectableMocker(&replayer, "dt_base");
		BtTestStrategy* stra = new BtTestStrategy("dt_base", B_STDCODE, "m1", 50);
		mocker->inject(stra);

		replayer.register_sink(mocker, "dt_base");
		replayer.prepare();
		replayer.run(false);

		//收集输出
		ret.trades = read_output(outDir, "dt_base", "trades.csv");
		ret.closes = read_output(outDir, "dt_base", "closes.csv");
		ret.funds = read_output(outDir, "dt_base", "funds.csv");
		ret.signals = read_output(outDir, "dt_base", "signals.csv");
		ret.ran = true;

		ret.calls = stra->calls();

		delete mocker;
		delete stra;
		cfg->release();
		//输出目录留着不删，方便失败时人工查看
		return ret;
	}
}

/*
 *	先隔离验证回测框架本身：不加载任何策略，只让 replayer 把数据回放完。
 *	如果这一步就崩，问题在框架或数据格式，和策略无关
 */
TEST(test_bt_baseline, replayer_runs_without_strategy)
{
	using namespace bt_rig;

	std::string dir = "./ut_bt_noStra/";
	if (bfs::exists(dir)) bfs::remove_all(dir);
	bfs::create_directories(dir);

	write_basefiles(dir, B_EXCHG, B_PID, B_CODE);
	uint32_t bars = write_min1_bars(dir, B_EXCHG, B_CODE, B_SDATE, B_DAYS);
	ASSERT_GT(bars, 1000u);

	std::string outDir = dir + "outputs/";
	bfs::create_directories(outDir);
	WtHelper::setOutputDir(outDir.c_str());
	WtHelper::setInstDir(lib_dir().c_str());

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("mode", "bin");
	cfg->append("path", dir.c_str());
	cfg->append("stime", (uint64_t)B_SDATE * 10000 + 900);
	cfg->append("etime", (uint64_t)(B_SDATE + B_DAYS) * 10000 + 1500);
	cfg->append("tick", false);
	cfg->append("align_by_section", false);

	WTSVariant* bf = WTSVariant::createObject();
	bf->append("session", (dir + "sessions.json").c_str());
	bf->append("commodity", (dir + "commodities.json").c_str());
	bf->append("contract", (dir + "contracts.json").c_str());
	cfg->append("basefiles", bf);

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg)) << "replayer init failed";

	//mocker 不加载策略工厂，_strategy 为 NULL
	CtaMocker* mocker = new CtaMocker(&replayer, "nostra", 0, false, NULL, false);
	replayer.register_sink(mocker, "nostra");

	printf("[bt] prepare...\n"); fflush(stdout);
	replayer.prepare();
	printf("[bt] run...\n"); fflush(stdout);
	replayer.run(false);
	printf("[bt] done\n"); fflush(stdout);

	delete mocker;
	cfg->release();
	bfs::remove_all(dir);
}

/*
 *	基线本身：min1 回测必须能跑出成交
 *	如果这里 trades 是空的，说明数据或策略参数没配好，
 *	后面的逐字节比对就没有意义了
 */
TEST(test_bt_baseline, min1_backtest_produces_trades)
{
	BtResult r = run_min1_backtest("base");

	ASSERT_TRUE(r.ran) << "backtest did not run, cannot establish a baseline";
	EXPECT_GT(r.bars, 1000u) << "not enough bars generated";

	printf("[bt] bars=%u trades=%zu closes=%zu funds=%zu signals=%zu\n",
		r.bars, r.trades.size(), r.closes.size(), r.funds.size(), r.signals.size());
	fflush(stdout);

	//至少要有资金曲线
	EXPECT_NE(r.funds, "<missing>") << "funds.csv missing";

	/*
	 *	必须真的产生了成交。
	 *	csv有表头，所以只看长度不够，要看有没有数据行
	 */
	uint32_t tradeLines = 0;
	for (char c : r.trades)
		if (c == '\n') tradeLines++;
	printf("[bt] trade lines (incl header) = %u\n", tradeLines);
	EXPECT_GT(tradeLines, 1u)
		<< "baseline has no trades -- the comparison later would be vacuous";
}

/*
 *	同一份数据跑两遍，输出必须完全一致。
 *
 *	这条先验证回测本身是确定性的：如果它自己每次跑都不一样(比如受
 *	真实时钟、遍历顺序、未初始化内存影响)，那"改造前后逐字节对比"
 *	这个方法根本就不成立，得先把不确定性找出来
 */
TEST(test_bt_baseline, backtest_is_deterministic)
{
	BtResult a = run_min1_backtest("det1");
	BtResult b = run_min1_backtest("det2");

	ASSERT_TRUE(a.ran && b.ran);

	EXPECT_EQ(a.bars, b.bars);
	EXPECT_EQ(a.trades, b.trades) << "trades.csv differs between two identical runs";
	EXPECT_EQ(a.closes, b.closes) << "closes.csv differs between two identical runs";
	EXPECT_EQ(a.signals, b.signals) << "signals.csv differs between two identical runs";

	/*
	 *	funds.csv 里带有落地时间之类的字段，可能天然不同，
	 *	所以只比行数，不比内容
	 */
	uint32_t la = 0, lb = 0;
	for (char c : a.funds) if (c == '\n') la++;
	for (char c : b.funds) if (c == '\n') lb++;
	EXPECT_EQ(la, lb) << "funds.csv line count differs between two identical runs";
}

/*
 *	逐字节回归：min1 回测的输出必须和固化的基线完全一致
 *
 *	这是 P4 改造 HisDataReplayer 时间轴时唯一的安全网。
 *	基线文件在 TestBtUnits/baseline/ 下，生成方式见那里的 README。
 *	如果输出确实应该变化，用 WT_UPDATE_BASELINE=1 重新生成，
 *	并在提交信息里说明为什么变
 */
TEST(test_bt_baseline, min1_output_matches_baseline)
{
	using namespace bt_rig;

	BtResult r = run_min1_backtest("golden");
	ASSERT_TRUE(r.ran);

	//基线文件的位置：相对源码目录，通过环境变量给出
	const char* bdir = getenv("WT_BASELINE_DIR");
	std::string baseDir = (bdir != NULL && strlen(bdir) > 0)
		? StrUtil::standardisePath(std::string(bdir))
		: std::string("");

	if (baseDir.empty())
	{
		//这份gtest比较老，没有 GTEST_SKIP，用提示+返回代替
		printf("[bt] WT_BASELINE_DIR not set, skipping golden comparison\n");
		return;
	}

	struct Item { const char* file; const std::string* actual; };
	Item items[] = {
		{ "min1_trades.csv",  &r.trades  },
		{ "min1_closes.csv",  &r.closes  },
		{ "min1_signals.csv", &r.signals },
		{ "min1_funds.csv",   &r.funds   },
	};

	bool bUpdate = (getenv("WT_UPDATE_BASELINE") != NULL);

	for (const Item& it : items)
	{
		std::string p = baseDir + it.file;

		if (bUpdate)
		{
			StdFile::write_file_content(p.c_str(), *it.actual);
			printf("[bt] baseline updated: %s\n", it.file);
			continue;
		}

		ASSERT_TRUE(StdFile::exists(p.c_str())) << "baseline file missing: " << p;

		std::string expect;
		StdFile::read_file_content(p.c_str(), expect);

		EXPECT_EQ(*it.actual, expect)
			<< it.file << " diverged from the baseline. "
			<< "If this change is intended, regenerate with WT_UPDATE_BASELINE=1 "
			<< "and explain why in the commit message.";
	}
}
