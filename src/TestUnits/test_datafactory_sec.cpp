/*!
 * \file test_datafactory_sec.cpp
 * \brief 秒线聚合与重采样测试
 *
 * 重点：
 * 1、updateSecData 两个缺陷的回归（缺appendBar / new不delete）
 * 2、s5 -> s15 重采样 与 直接从tick聚合成s15 的交叉验证
 */
#include "tick_maker.hpp"
#include "../WTSTools/WTSDataFactory.h"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "gtest/gtest/gtest.h"

USING_NS_WTP;

namespace
{
	WTSDataFactory g_fact;

	WTSSessionInfo* make_sess()
	{
		//9:00-10:15, 10:30-11:30
		WTSSessionInfo* s = WTSSessionInfo::create("FUTURE", "FUTURE", 0);
		s->addTradingSection(900, 1015);
		s->addTradingSection(1030, 1130);
		return s;
	}

	const char* CODE = "TEST.rb2610";
	const uint32_t TDATE = 20260920;
}

/*
 *	updateSecData 的核心回归
 *
 *	改造前的实现是：
 *		WTSBarStruct* day = new WTSBarStruct; ...; return day;
 *	既没有 appendBar 也没有 delete，后果：
 *	  - klineData 永远不增长（size 一直是0）
 *	  - 于是每个tick的 time(size-1) 都返回 INVALID_UINT32，都被判成"新bar"
 *	  - OHLC 累积分支永远走不到，且每个tick泄漏一个 WTSBarStruct
 *	这个用例在修复前必然失败
 */
TEST(test_datafactory_sec, updateSecData_appends_and_accumulates)
{
	WTSSessionInfo* sInfo = make_sess();

	WTSKlineData* kline = WTSKlineData::create(CODE, 0);
	kline->setPeriod(KP_Tick, 5);	//KP_Tick路径：times直接是秒数

	//同一根s5内的5个tick：09:00:00 ~ 09:00:04
	const double pxs[] = { 100.0, 102.0, 98.0, 101.0, 99.0 };
	for (int i = 0; i < 5; i++)
	{
		WTSTickStruct ts = tick_maker::make(CODE, TDATE, TDATE, 90000 + i, pxs[i]);
		WTSTickData* tick = tick_maker::wrap(ts);
		g_fact.updateKlineData(kline, tick, sInfo, false);
		tick->release();
	}

	//必须只有一根bar，且OHLC正确累积
	ASSERT_EQ(kline->size(), 1u) << "bars were not appended into the container";

	WTSBarStruct* bar = kline->at(0);
	EXPECT_DOUBLE_EQ(bar->open, 100.0);
	EXPECT_DOUBLE_EQ(bar->high, 102.0);
	EXPECT_DOUBLE_EQ(bar->low, 98.0);
	EXPECT_DOUBLE_EQ(bar->close, 99.0);
	EXPECT_DOUBLE_EQ(bar->vol, 5.0);
	EXPECT_EQ(bar->date, TDATE);
	//收于 09:00:05
	EXPECT_EQ(bar->time, TimeUtils::timeToSecBar(TDATE, 90005));

	kline->release();
	sInfo->release();
}

TEST(test_datafactory_sec, updateSecData_rolls_to_next_bar)
{
	WTSSessionInfo* sInfo = make_sess();

	WTSKlineData* kline = WTSKlineData::create(CODE, 0);
	kline->setPeriod(KP_Tick, 5);

	//12个tick跨越3根s5
	for (int i = 0; i < 12; i++)
	{
		WTSTickStruct ts = tick_maker::make(CODE, TDATE, TDATE, 90000 + i, 100.0 + i);
		WTSTickData* tick = tick_maker::wrap(ts);
		g_fact.updateKlineData(kline, tick, sInfo, false);
		tick->release();
	}

	//09:00:00~04 / 05~09 / 10~11(未满)
	ASSERT_EQ(kline->size(), 3u);
	EXPECT_EQ(kline->at(0)->time, TimeUtils::timeToSecBar(TDATE, 90005));
	EXPECT_EQ(kline->at(1)->time, TimeUtils::timeToSecBar(TDATE, 90010));
	EXPECT_EQ(kline->at(2)->time, TimeUtils::timeToSecBar(TDATE, 90015));

	//每根的成交量：5 + 5 + 2
	EXPECT_DOUBLE_EQ(kline->at(0)->vol, 5.0);
	EXPECT_DOUBLE_EQ(kline->at(1)->vol, 5.0);
	EXPECT_DOUBLE_EQ(kline->at(2)->vol, 2.0);

	kline->release();
	sInfo->release();
}

TEST(test_datafactory_sec, updateSecBar_rejects_out_of_session_tick)
{
	WTSSessionInfo* sInfo = make_sess();

	WTSKlineData* kline = WTSKlineData::create(CODE, 0);
	kline->setPeriod(KP_Tick, 5);

	//先来一笔正常tick
	WTSTickStruct ok = tick_maker::make(CODE, TDATE, TDATE, 90000, 100.0);
	WTSTickData* t1 = tick_maker::wrap(ok);
	g_fact.updateKlineData(kline, t1, sInfo, false);
	t1->release();
	ASSERT_EQ(kline->size(), 1u);

	//非交易时间的tick（12:00:00 处于午休）不能污染已有bar
	WTSTickStruct bad = tick_maker::make(CODE, TDATE, TDATE, 120000, 999.0);
	WTSTickData* t2 = tick_maker::wrap(bad);
	g_fact.updateKlineData(kline, t2, sInfo, false);
	t2->release();

	EXPECT_EQ(kline->size(), 1u);
	EXPECT_DOUBLE_EQ(kline->at(0)->close, 100.0) << "out-of-session tick must not touch the bar";
	EXPECT_DOUBLE_EQ(kline->at(0)->high, 100.0);

	kline->release();
	sInfo->release();
}

TEST(test_datafactory_sec, updateSecBar_ignores_reversed_tick)
{
	WTSSessionInfo* sInfo = make_sess();

	WTSKlineData* kline = WTSKlineData::create(CODE, 0);
	kline->setPeriod(KP_Tick, 5);

	//推进到第二根
	for (int i = 0; i < 7; i++)
	{
		WTSTickStruct ts = tick_maker::make(CODE, TDATE, TDATE, 90000 + i, 100.0);
		WTSTickData* tick = tick_maker::wrap(ts);
		g_fact.updateKlineData(kline, tick, sInfo, false);
		tick->release();
	}
	ASSERT_EQ(kline->size(), 2u);
	double closeOfBar2 = kline->at(1)->close;

	//倒序tick（回到第一根的时间）不能回写已闭合的bar
	WTSTickStruct back = tick_maker::make(CODE, TDATE, TDATE, 90001, 555.0);
	WTSTickData* tb = tick_maker::wrap(back);
	g_fact.updateKlineData(kline, tb, sInfo, false);
	tb->release();

	EXPECT_EQ(kline->size(), 2u);
	EXPECT_DOUBLE_EQ(kline->at(1)->close, closeOfBar2);
	EXPECT_DOUBLE_EQ(kline->at(0)->close, 100.0) << "closed bar must not be rewritten";

	kline->release();
	sInfo->release();
}

TEST(test_datafactory_sec, extract_s5_from_ticks)
{
	WTSSessionInfo* sInfo = make_sess();

	//10个tick，间隔1秒
	std::vector<WTSTickStruct> ticks = tick_maker::series(CODE, TDATE, TDATE, 90000, 10, 1, 100.0);
	WTSTickSlice* slice = WTSTickSlice::create(CODE, ticks.data(), (uint32_t)ticks.size());

	WTSKlineData* kline = g_fact.extractKlineData(slice, 5, sInfo, false, false);
	ASSERT_NE(kline, nullptr);

	//10秒 -> 2根s5
	ASSERT_EQ(kline->size(), 2u);
	EXPECT_EQ(kline->at(0)->time, TimeUtils::timeToSecBar(TDATE, 90005));
	EXPECT_EQ(kline->at(1)->time, TimeUtils::timeToSecBar(TDATE, 90010));

	kline->release();
	slice->release();
	sInfo->release();
}

/*
 *	交叉验证：两条路径生成s15必须一致
 *	  路径A：tick --(15秒)--> s15
 *	  路径B：tick --(5秒)--> s5 --(3倍)--> s15
 *	这是秒线重采样正确性最强的一条断言
 */
TEST(test_datafactory_sec, resample_s5_to_s15_matches_direct_from_ticks)
{
	WTSSessionInfo* sInfo = make_sess();

	//90个tick，间隔1秒，价格有起伏以便校验高低点
	std::vector<double> pxseq = { 100.0, 103.0, 99.0, 101.0, 97.0, 105.0, 100.5 };
	std::vector<WTSTickStruct> ticks = tick_maker::series(CODE, TDATE, TDATE, 90000, 90, 1, 100.0, pxseq);

	//路径A
	WTSTickSlice* tslice = WTSTickSlice::create(CODE, ticks.data(), (uint32_t)ticks.size());
	WTSKlineData* direct = g_fact.extractKlineData(tslice, 15, sInfo, false, false);
	ASSERT_NE(direct, nullptr);

	//路径B：先聚成s5
	WTSTickSlice* tslice2 = WTSTickSlice::create(CODE, ticks.data(), (uint32_t)ticks.size());
	WTSKlineData* s5 = g_fact.extractKlineData(tslice2, 5, sInfo, false, false);
	ASSERT_NE(s5, nullptr);

	//再从s5重采样到s15（times=3，即3*5=15秒）
	WTSKlineSlice* bslice = WTSKlineSlice::create(CODE, KP_Sec5, 1, s5->at(0), (int32_t)s5->size());
	WTSKlineData* resampled = g_fact.extractKlineData(bslice, KP_Sec5, 3, sInfo, true, false);
	ASSERT_NE(resampled, nullptr);

	//条数一致
	ASSERT_EQ(resampled->size(), direct->size()) << "bar count mismatch between two paths";

	//逐根比对时间戳与OHLCV
	for (uint32_t i = 0; i < direct->size(); i++)
	{
		WTSBarStruct* a = direct->at(i);
		WTSBarStruct* b = resampled->at(i);

		EXPECT_EQ(b->time, a->time) << "time mismatch at bar " << i;
		EXPECT_DOUBLE_EQ(b->open, a->open) << "open mismatch at bar " << i;
		EXPECT_DOUBLE_EQ(b->high, a->high) << "high mismatch at bar " << i;
		EXPECT_DOUBLE_EQ(b->low, a->low) << "low mismatch at bar " << i;
		EXPECT_DOUBLE_EQ(b->close, a->close) << "close mismatch at bar " << i;
		EXPECT_DOUBLE_EQ(b->vol, a->vol) << "vol mismatch at bar " << i;
	}

	resampled->release();
	bslice->release();
	s5->release();
	tslice2->release();
	direct->release();
	tslice->release();
	sInfo->release();
}

/*
 *	按小节对齐：跨小节时最后一根必须收在小节结束，不能跨到下一小节
 */
TEST(test_datafactory_sec, align_by_section_does_not_cross_section)
{
	WTSSessionInfo* sInfo = make_sess();

	WTSKlineData* kline = WTSKlineData::create(CODE, 0);
	kline->setPeriod(KP_Sec5, 2);	//s10

	//第一小节尾部 10:14:5x 和第二小节开头 10:30:0x
	const uint32_t hmss[] = { 101450, 101455, 101459, 103000, 103001 };
	for (uint32_t hms : hmss)
	{
		WTSTickStruct ts = tick_maker::make(CODE, TDATE, TDATE, hms, 100.0);
		WTSTickData* tick = tick_maker::wrap(ts);
		g_fact.updateKlineData(kline, tick, sInfo, true);	//bAlignSec = true
		tick->release();
	}

	ASSERT_GT(kline->size(), 0u);

	//所有bar的时间戳都必须落在交易时段内（不能出现10:20这类非交易时刻）
	for (uint32_t i = 0; i < kline->size(); i++)
	{
		uint32_t hms = TimeUtils::secBarToTime(kline->at(i)->time);
		uint32_t hm = hms / 100;
		bool inSection = (hm >= 900 && hm <= 1015) || (hm >= 1030 && hm <= 1130);
		EXPECT_TRUE(inSection) << "bar time " << hms << " falls outside trading sections";
	}

	kline->release();
	sInfo->release();
}

TEST(test_datafactory_sec, degenerate_inputs)
{
	WTSSessionInfo* sInfo = make_sess();

	//空tick切片
	WTSTickSlice* empty = WTSTickSlice::create(CODE, NULL, 0);
	EXPECT_EQ(g_fact.extractKlineData(empty, 5, sInfo, false, false), nullptr);
	empty->release();

	//单个tick
	std::vector<WTSTickStruct> one = tick_maker::series(CODE, TDATE, TDATE, 90000, 1, 1, 100.0);
	WTSTickSlice* s1 = WTSTickSlice::create(CODE, one.data(), 1);
	WTSKlineData* k1 = g_fact.extractKlineData(s1, 5, sInfo, false, false);
	ASSERT_NE(k1, nullptr);
	EXPECT_EQ(k1->size(), 1u);
	k1->release();
	s1->release();

	//NULL 保护
	EXPECT_EQ(g_fact.updateKlineData((WTSKlineData*)NULL, (WTSTickData*)NULL, sInfo, false), nullptr);

	sInfo->release();
}
