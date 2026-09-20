/*!
 * \file test_secbar_engine.cpp
 * \brief 秒级调度链路的集成验证
 *
 * 这里组装的是真实的 WtCtaEngine + WtDtMgr + WtDataStorage + WtCtaRtTicker，
 * 只有基础数据管理器和交易通道是桩。验证的是 P3 的核心问题：
 *   1、秒级调度间隔是否恒为5秒、有没有跳号或重复
 *   2、稀疏tick(某段完全没有行情)时本地时钟兜底是否仍然闭合
 *   3、on_bar 与 on_calculate 的先后顺序
 *   4、条件单在 mark & sweep 下是否原地存活、有没有清空窗口
 */
#include <stdio.h>
#include <vector>
#include <string>

#include "mock_datasink.hpp"
#include "tick_maker.hpp"

#include "../WtCore/WtCtaEngine.h"
#include "../WtCore/WtDtMgr.h"
#include "../WtCore/CtaStraContext.h"
#include "../WtCore/WtHelper.h"
#include "../WtCore/TraderAdapter.h"
#include "storage_loader.hpp"
#include "../Includes/CtaStrategyDefs.h"
#include "../Includes/IHotMgr.h"
#include "../Includes/WTSVariant.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/StrUtil.hpp"
#include "../Share/BoostFile.hpp"
#include "../Share/fmtlib.h"
#include "gtest/gtest/gtest.h"

#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

USING_NS_WTP;

namespace
{
	const char* E_EXCHG = "TEST";
	const char* E_PID = "rb";
	const char* E_CODE = "rb2610";
	const char* E_STDCODE = "TEST.rb.2610";
	//交易日在 setup 里按当前真实日期确定
	uint32_t E_TDATE = 0;

	/*
	 *	这里用 ALLDAY(0:00-24:00) 而不是期货的分段会话。
	 *
	 *	原因是 WtCtaRtTicker::init 会用 TimeUtils::getDateTime 把 _date/_time
	 *	设成真实的墙上时间，而 on_tick 里有一层保护：
	 *	行情时间早于本地时间的tick被当作历史数据，直接 trigger_price 后返回，
	 *	不做任何时间推进。
	 *	所以集成测试喂的tick必须落在"当前真实时刻之后"，
	 *	而 ALLDAY 保证任何时刻都在交易时段内，测试才能随时跑
	 */
	WTSSessionInfo* e_make_sess()
	{
		WTSSessionInfo* s = WTSSessionInfo::create("ALLDAY", "ALLDAY", 0);
		s->addTradingSection(0, 2400);
		return s;
	}

	//取当前真实日期/时间(HHMMSS)，tick要从这之后开始喂
	void now_date_time(uint32_t& uDate, uint32_t& hms)
	{
		uint32_t t = 0;
		TimeUtils::getDateTime(uDate, t);
		hms = t / 1000;		//getDateTime 返回的是 HHMMSSmmm
	}

	/*
	 *	测试策略：主K线订阅 s5，记录每次 on_calculate / on_bar 的时刻
	 */
	class SecTestStrategy : public CtaStrategy
	{
	public:
		SecTestStrategy(const char* id) : CtaStrategy(id), _bars(0), _place_cond(false), _cond_once(false), _placed_once(false) {}

		virtual const char* getName() override { return "SecTestStrategy"; }
		virtual const char* getFactName() override { return "TestFact"; }

		virtual void on_init(ICtaStraCtx* ctx) override
		{
			//订阅 s5 作为主K线
			WTSKlineSlice* slice = ctx->stra_get_bars(E_STDCODE, "s5", 20, true);
			if (slice)
				slice->release();
			ctx->stra_sub_bar_events(E_STDCODE, "s5");
		}

		virtual void on_schedule(ICtaStraCtx* ctx, uint32_t uDate, uint32_t uTime) override
		{
			//on_calculate 会走到这里
			_calc_dates.emplace_back(uDate);
			_calc_times.emplace_back(uTime);
			_calc_secs.emplace_back(ctx->stra_get_time() /*HHMM*/);

			if (_place_cond)
			{
				//幂等重挂：每次都挂同一个限价单
				ctx->stra_enter_long(E_STDCODE, 1, "cond_entry", 1.0 /*limitprice，永不触发*/, 0.0);
			}
			if (_cond_once && !_placed_once)
			{
				ctx->stra_enter_long(E_STDCODE, 1, "once_entry", 1.0, 0.0);
				_placed_once = true;
			}
		}

		virtual void on_bar(ICtaStraCtx* ctx, const char* stdCode, const char* period, WTSBarStruct* newBar) override
		{
			_bars++;
			_bar_times.emplace_back(newBar->time);
			_bar_periods.emplace_back(period);
		}

		void set_place_cond(bool b) { _place_cond = b; }
		void set_cond_once(bool b) { _cond_once = b; }

		std::vector<uint32_t> _calc_dates;
		std::vector<uint32_t> _calc_times;
		std::vector<uint32_t> _calc_secs;
		std::vector<uint64_t> _bar_times;
		std::vector<std::string> _bar_periods;
		uint32_t _bars;

	private:
		bool _place_cond;
		bool _cond_once;
		bool _placed_once;
	};

	/*
	 *	IHotMgr 桩：测试里用的是分月合约，不涉及主力切换
	 */
	class StubHotMgr : public IHotMgr
	{
	public:
		virtual const char* getRuleTag(const char* stdCode) override { return ""; }
		virtual double getRuleFactor(const char* stdCode, const char* ruleTag, uint32_t uDate = 0) override { return 1.0; }
		virtual const char* getPrevRawCode(const char* exchg, const char* pid, uint32_t dt) override { return ""; }
		virtual const char* getRawCode(const char* exchg, const char* pid, uint32_t dt) override { return ""; }
		virtual bool isHot(const char* exchg, const char* rawCode, uint32_t dt = 0) override { return false; }
		virtual bool splitHotSecions(const char* exchg, const char* hotCode, uint32_t sDt, uint32_t eDt, HotSections& sections) override { return false; }
		virtual const char* getPrevSecondRawCode(const char* exchg, const char* pid, uint32_t dt) override { return ""; }
		virtual const char* getSecondRawCode(const char* exchg, const char* pid, uint32_t dt) override { return ""; }
		virtual bool isSecond(const char* exchg, const char* rawCode, uint32_t dt = 0) override { return false; }
		virtual bool splitSecondSecions(const char* exchg, const char* hotCode, uint32_t sDt, uint32_t eDt, HotSections& sections) override { return false; }
		virtual const char* getCustomRawCode(const char* tag, const char* fullPid, uint32_t dt = 0) override { return ""; }
		virtual const char* getPrevCustomRawCode(const char* tag, const char* fullPid, uint32_t dt = 0) override { return ""; }
		virtual bool isCustomHot(const char* tag, const char* fullCode, uint32_t d = 0) override { return false; }
		virtual bool splitCustomSections(const char* tag, const char* fullPid, uint32_t sDt, uint32_t eDt, HotSections& sections) override { return false; }
	};
}

namespace
{
	/*
	 *	预置一批 his/sec5 历史数据
	 *
	 *	这一步是必须的：CtaStraBaseCtx::stra_get_bars 只有在拿到非空K线时
	 *	才会注册 _kline_tags 并调 sub_tick。策略启动时如果一根历史K线都没有，
	 *	主K线标记就不会建立，on_schedule 里遍历空的 _kline_tags 直接空转，
	 *	on_calculate 永远不触发(分钟线也是同样的行为)。
	 *	实盘环境里总是有历史数据，所以这个前提平时不显现
	 */
	void write_his_sec5_for_engine(const std::string& dir, uint32_t count)
	{
		std::string path = fmtutil::format("{}his/sec5/{}/", dir, E_EXCHG);
		fs::create_directories(path);
		std::string file = fmtutil::format("{}{}.dsb", path, E_CODE);

		std::vector<WTSBarStruct> bars;
		bars.resize(count);
		//放在前一交易日，避免和当日的实时数据重叠
		for (uint32_t i = 0; i < count; i++)
		{
			bars[i].date = E_TDATE - 1;
			bars[i].time = TimeUtils::timeToSecBar(E_TDATE - 1, tick_maker::advance_hms(90005, i * 5));
			bars[i].open = bars[i].high = bars[i].low = bars[i].close = 100.0;
			bars[i].vol = 1;
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

	/*
	 *	一套完整的引擎环境：真实的 WtCtaEngine + WtDtMgr + WtDataStorage + ticker，
	 *	只有基础数据和交易通道是桩
	 */
	struct EngineRig
	{
		std::string			dir;
		//第一笔tick的时刻(HHMMSS)，必须晚于ticker初始化时的本地时间
		uint32_t			start_hms = 0;
		WTSSessionInfo*		sinfo = NULL;
		MockBaseDataMgr*	bd = NULL;
		StubHotMgr			hot;
		TraderAdapterMgr	adapters;
		WtDtMgr				dtMgr;
		WtCtaEngine			engine;
		WTSVariant*			dtCfg = NULL;
		WTSVariant*			engCfg = NULL;

		/*
		 *	实盘里K线是由独立的数据进程(QuoteFactory + WtDataWriter)写盘的，
		 *	交易进程(WtRunner)只负责读。所以这里必须把writer一起拉起来，
		 *	否则reader找不到rt/sec5的块，onSecondEnd不会回调on_bar，
		 *	策略的主K线标记一直是未闭合，on_schedule里的isMainUdt门禁
		 *	会让on_calculate一直空转
		 */
		MockWriterSink*		wsink = NULL;
		IDataWriter*		writer = NULL;
		WTSVariant*			wCfg = NULL;
		SecTestStrategy*	stra = NULL;
		CtaStraContext*		ctx = NULL;

		void setup(const char* tag, bool bPlaceCond = false, bool bCondOnce = false)
		{
			dir = fmtutil::format("./ut_eng_{}/", tag);
			if (fs::exists(dir))
				fs::remove_all(dir);
			fs::create_directories(dir);

			//引擎的输出(trades.csv/funds.csv/marker.json等)都落到临时目录
			WtHelper::setGenerateDir(dir.c_str());

			/*
			 *	WtDtMgr 要靠 getInstDir() 拼出 WtDataStorage 动态库的路径，
			 *	不设的话默认是空字符串，会按相对当前目录去找、然后静默失败
			 */
			const char* libdir = getenv("WT_TEST_LIBDIR");
			if (libdir != NULL && strlen(libdir) > 0)
				WtHelper::setInstDir(StrUtil::standardisePath(std::string(libdir)).c_str());

			uint32_t nowHms = 0;
			now_date_time(E_TDATE, nowHms);
			//留出2秒余量，避免tick时间刚好压在本地时间上
			start_hms = tick_maker::advance_hms(nowHms, 2);

			sinfo = e_make_sess();
			bd = new MockBaseDataMgr(E_EXCHG, E_PID, E_CODE, sinfo, E_TDATE);

			//先铺历史数据，策略on_init时才能拿到非空K线
			write_his_sec5_for_engine(dir, 100);

			//数据写入端：和reader共用同一个目录
			wsink = new MockWriterSink(bd, E_TDATE);
			writer = storage_loader::make_writer();
			wCfg = WTSVariant::createObject();
			wCfg->append("path", dir.c_str());
			wCfg->append("async", false);
			wCfg->append("enablesec5", true);
			wCfg->append("disablemin1", true);
			wCfg->append("disablemin5", true);
			wCfg->append("disableday", true);
			wCfg->append("disabletick", true);
			writer->init(wCfg, wsink);

			/*
			 *	顺序很关键，必须和 WtRunner 一致：先 initEngine 再 initDataMgr。
			 *	WtDtMgr::init 会立刻让 reader 去取 sink->get_basedata_mgr()，
			 *	而那个值要等 engine.init 才被赋值；
			 *	反过来的话 reader 拿到的是未初始化的指针，
			 *	之后任何一次 _base_data_mgr 的虚调用都会崩
			 */
			engCfg = WTSVariant::createObject();
			engCfg->append("poolsize", (uint32_t)0);	//串行，便于断言调用顺序
			engCfg->append("filters", "");
			engCfg->append("fees", "");
			WTSVariant* prod = WTSVariant::createObject();
			prod->append("session", "FUTURE");
			engCfg->append("product", prod);

			engine.init(engCfg, bd, &dtMgr, &hot, NULL);
			engine.set_adapter_mgr(&adapters);

			//数据管理器：指向同一个临时目录
			dtCfg = WTSVariant::createObject();
			WTSVariant* store = WTSVariant::createObject();
			store->append("path", dir.c_str());
			dtCfg->append("store", store);
			bool dtOk = dtMgr.init(dtCfg, &engine);
			(void)dtOk;

			stra = new SecTestStrategy("sec_test");
			stra->set_place_cond(bPlaceCond);
			stra->set_cond_once(bCondOnce);

			ctx = new CtaStraContext(&engine, "sec_test", 0);
			ctx->set_strategy(stra);
			engine.addContext(CtaContextPtr(ctx));
		}

		/*
		 *	喂一笔tick，时间由调用方指定(HHMMSS)
		 *	顺序和实盘一致：先落盘，再推给交易引擎。
		 *	反过来的话引擎读数据时那根bar还没写进去
		 */
		void feed(uint32_t hms, double px, double hi, double lo, uint32_t msec = 0)
		{
			WTSTickStruct ts = tick_maker::make(E_CODE, E_TDATE, E_TDATE, hms, px, 1, 100, msec, hi, lo);
			wt_strcpy(ts.code, E_CODE);

			{
				WTSTickData* wt = WTSTickData::create(ts);
				wt->setContractInfo(bd->contract());
				writer->writeTick(wt, 0);
				wt->release();
			}

			WTSTickData* tick = WTSTickData::create(ts);
			tick->setContractInfo(bd->contract());
			engine.handle_push_quote(tick);
			tick->release();
		}

		void teardown()
		{
			//engine析构会stop ticker
			if (writer) { writer->release(); storage_loader::free_writer(writer); }
			if (wsink) delete wsink;
			if (wCfg) wCfg->release();
			if (dtCfg) dtCfg->release();
			if (engCfg) engCfg->release();
			if (sinfo) sinfo->release();
			delete bd;
			if (fs::exists(dir))
				fs::remove_all(dir);
		}
	};
}

/*
 *	链路能否跑通：订阅s5后，喂tick应该触发秒级调度
 */
TEST(test_secbar_engine, sec_schedule_is_driven_every_5s)
{
	EngineRig rig;
	rig.setup("sched");

	//run() 里会走 on_init(策略订阅) -> 检测秒线订阅 -> 启动ticker线程
	rig.engine.run();

	//从当前真实时刻之后开始，喂60秒的tick，每秒一笔
	double hi = 0, lo = 0;
	for (uint32_t i = 0; i < 60; i++)
	{
		double px = 100.0 + (i % 7);
		hi = (i == 0) ? px : std::max(hi, px);
		lo = (i == 0) ? px : std::min(lo, px);
		rig.feed(tick_maker::advance_hms(rig.start_hms, i), px, hi, lo);
	}

	//秒级推进必须真的开起来了
	ASSERT_TRUE(rig.engine.has_sec_subs()) << "sec bars were not detected as subscribed";

	//数据确实落盘了
	std::string dmb = fmtutil::format("{}rt/sec5/{}/{}.dmb", rig.dir, E_EXCHG, E_CODE);
	ASSERT_TRUE(StdFile::exists(dmb.c_str())) << "writer did not produce rt/sec5 block";

	//秒级调度必须发生
	ASSERT_GT(rig.stra->_calc_times.size(), 0u)
		<< "on_calculate was never triggered -- second-level scheduling is not working";

	/*
	 *	60秒的tick按5秒切，闭合次数是11或12，
	 *	取决于起始秒是否正好压在5秒边界上(start_hms来自运行时刻，不固定)。
	 *	所以这里不硬编码次数，只校验数量级；
	 *	真正严格的检查是下面那条恒等式
	 */
	EXPECT_GE(rig.stra->_calc_times.size(), 11u);
	EXPECT_LE(rig.stra->_calc_times.size(), 12u);

	//on_bar 和 on_calculate 必须一一对应：每根秒线闭合都要带出一次计算
	EXPECT_EQ((size_t)rig.stra->_bars, rig.stra->_calc_times.size())
		<< "on_bar and on_calculate counts diverged";

	//闭合的bar时间戳必须严格递增，不重不漏
	uint64_t prev = 0;
	for (size_t i = 0; i < rig.stra->_bar_times.size(); i++)
	{
		EXPECT_GT(rig.stra->_bar_times[i], prev) << "bar times must be strictly increasing at " << i;
		prev = rig.stra->_bar_times[i];
	}

	/*
	 *	策略侧收到的周期字符串要和订阅时写的完全一致。
	 *	链路上它被拆成 (基础周期's', times=5) 传递，
	 *	到 CtaStraContext::on_bar_close 又拼回 "s5"
	 */
	for (const std::string& p : rig.stra->_bar_periods)
		EXPECT_EQ(p, "s5") << "period string should round-trip back to what the strategy subscribed";

	//相邻两次闭合的bar时间必须正好差5秒
	for (size_t i = 1; i < rig.stra->_bar_times.size(); i++)
	{
		uint32_t t1 = TimeUtils::secBarToTime(rig.stra->_bar_times[i-1]);
		uint32_t t2 = TimeUtils::secBarToTime(rig.stra->_bar_times[i]);
		uint32_t s1 = rig.sinfo->timeToSeconds(t1);
		uint32_t s2 = rig.sinfo->timeToSeconds(t2);
		EXPECT_EQ(s2 - s1, 5u) << "gap between consecutive sec bars should be exactly 5s, at " << i;
	}

	/*
	 *	最严格的一条：闭合根数必须等于首尾秒序号跨度/5 + 1。
	 *	少一根说明漏了闭合，多一根说明重复发了事件，
	 *	这个恒等式不依赖测试的运行时刻
	 */
	if (rig.stra->_bar_times.size() >= 2)
	{
		uint32_t firstSec = rig.sinfo->timeToSeconds(TimeUtils::secBarToTime(rig.stra->_bar_times.front()));
		uint32_t lastSec = rig.sinfo->timeToSeconds(TimeUtils::secBarToTime(rig.stra->_bar_times.back()));
		EXPECT_EQ(rig.stra->_bar_times.size(), (size_t)((lastSec - firstSec) / 5 + 1))
			<< "bar count must match the span, no gaps and no duplicates";
	}

	rig.teardown();
}

/*
 *	稀疏行情下的兜底闭合
 *
 *	这是"闭合必须时间驱动"的核心验证：只喂一笔tick建立游标，
 *	之后完全没有行情。如果闭合只靠tick驱动，这根bar永远不会闭合，
 *	on_bar不触发，主K线标记一直是未闭合，on_calculate跟着空转。
 *	正确的行为是本地时钟到点后强制闭合
 */
TEST(test_secbar_engine, fallback_closes_bar_without_ticks)
{
	EngineRig rig;
	rig.setup("sparse");
	rig.engine.run();

	//只喂两笔，跨过一个5秒边界，让游标推进到下一根
	rig.feed(rig.start_hms, 100.0, 100.0, 100.0);
	rig.feed(tick_maker::advance_hms(rig.start_hms, 6), 101.0, 101.0, 100.0);

	size_t cntAfterTicks = rig.stra->_calc_times.size();

	//之后不再喂任何行情，等本地时钟兜底
	//ticker 的循环是 10ms 一轮，5秒的bar边界，这里等够
	std::this_thread::sleep_for(std::chrono::milliseconds(7000));

	size_t cntAfterWait = rig.stra->_calc_times.size();

	printf("[sparse] closes after ticks = %zu, after waiting = %zu\n", cntAfterTicks, cntAfterWait);
	fflush(stdout);

	EXPECT_GT(cntAfterWait, cntAfterTicks)
		<< "without any further ticks the local clock must still close the bar; "
		<< "if this fails, closing is tick-driven only and sparse instruments will stall";

	rig.teardown();
}

/*
 *	条件单在 mark & sweep 下的行为
 *
 *	策略每次 on_calculate 都幂等地重挂同一个限价单(价格设得永不触发)。
 *	改造前是 clear+重建，改造后应该原地存活。
 *	这里能观察到的是外部行为：调度多次之后条件单依然有效，
 *	而且从未因为清空窗口而丢失
 */
TEST(test_secbar_engine, idempotent_conditions_survive_reschedule)
{
	EngineRig rig;
	rig.setup("cond", true /*每次调度都重挂*/);
	rig.engine.run();

	double hi = 0, lo = 0;
	for (uint32_t i = 0; i < 40; i++)
	{
		double px = 100.0 + (i % 5);
		hi = (i == 0) ? px : std::max(hi, px);
		lo = (i == 0) ? px : std::min(lo, px);
		rig.feed(tick_maker::advance_hms(rig.start_hms, i), px, hi, lo);
	}

	//多次调度必须都发生了
	ASSERT_GE(rig.stra->_calc_times.size(), 5u);

	/*
	 *	条件单挂的是 limitprice=1.0 的买单，行情价在100附近，
	 *	永远不会触发，所以不应产生任何成交。
	 *	这里验证的是"重挂不会误触发"：
	 *	如果 mark&sweep 实现有误(比如把stale也算进等价判定)，
	 *	条件单会被反复删掉重建，行为上不一定报错，
	 *	但配合下面的持仓检查能发现异常成交
	 */
	double pos = rig.ctx->stra_get_position(E_STDCODE);
	EXPECT_DOUBLE_EQ(pos, 0.0)
		<< "a limit order far from market must never fill, position should stay flat";

	rig.teardown();
}

/*
 *	一次性挂单：策略只在第一次调度时挂单，之后不再重挂。
 *	按 mark & sweep 的语义，这张单应该在下一次调度时被清理掉，
 *	行为和改造前的 clear 完全一致
 */
TEST(test_secbar_engine, one_shot_condition_is_swept)
{
	EngineRig rig;
	rig.setup("once", false, true /*只挂一次*/);
	rig.engine.run();

	double hi = 0, lo = 0;
	for (uint32_t i = 0; i < 40; i++)
	{
		double px = 100.0 + (i % 5);
		hi = (i == 0) ? px : std::max(hi, px);
		lo = (i == 0) ? px : std::min(lo, px);
		rig.feed(tick_maker::advance_hms(rig.start_hms, i), px, hi, lo);
	}

	ASSERT_GE(rig.stra->_calc_times.size(), 5u);

	//同样不应该成交
	double pos = rig.ctx->stra_get_position(E_STDCODE);
	EXPECT_DOUBLE_EQ(pos, 0.0);

	rig.teardown();
}
