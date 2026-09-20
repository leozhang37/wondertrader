/*!
 * \file test_session_secs.cpp
 * \brief 会话秒级换算测试（秒K线的对齐规则基础）
 *
 * 固化 WTSSessionInfo 的 timeToSeconds / secondsToTime 行为，
 * 秒线的bar边界完全建立在这两个函数之上
 */
#include "../Includes/WTSSessionInfo.hpp"
#include "gtest/gtest/gtest.h"

USING_NS_WTP;

namespace
{
	//商品期货典型会话：9:00-10:15, 10:30-11:30, 13:30-15:00
	WTSSessionInfo* make_future_sess()
	{
		WTSSessionInfo* s = WTSSessionInfo::create("FUTURE", "FUTURE", 0);
		s->addTradingSection(900, 1015);
		s->addTradingSection(1030, 1130);
		s->addTradingSection(1330, 1500);
		return s;
	}

	//股票会话：9:30-11:30, 13:00-15:00
	WTSSessionInfo* make_stock_sess()
	{
		WTSSessionInfo* s = WTSSessionInfo::create("STOCK", "STOCK", 0);
		s->addTradingSection(930, 1130);
		s->addTradingSection(1300, 1500);
		return s;
	}

	//夜盘会话：21:00-次日2:30，offset 300分钟
	WTSSessionInfo* make_night_sess()
	{
		WTSSessionInfo* s = WTSSessionInfo::create("NIGHT", "NIGHT", 300);
		s->addTradingSection(2100, 230);
		return s;
	}
}

TEST(test_session_secs, future_secs_roundtrip)
{
	WTSSessionInfo* s = make_future_sess();

	//开盘第一秒
	EXPECT_EQ(s->timeToSeconds(90000), 0u);
	//开盘后1秒、1分钟
	EXPECT_EQ(s->timeToSeconds(90001), 1u);
	EXPECT_EQ(s->timeToSeconds(90100), 60u);

	//往返
	EXPECT_EQ(s->secondsToTime(0), 90000u);
	EXPECT_EQ(s->secondsToTime(1), 90001u);
	EXPECT_EQ(s->secondsToTime(60), 90100u);

	s->release();
}

/*
 *	小节边界归属
 *	timeToSeconds 内部对 seconds == stopSecs 做了 offset--，
 *	所以落在小节结束时刻的tick归该小节最后一秒，
 *	与 min1 的 isLastOfSection -> minutes-- 处理一致
 */
TEST(test_session_secs, section_boundary_attribution)
{
	WTSSessionInfo* s = make_future_sess();

	//第一小节 9:00-10:15 共 4500 秒
	uint32_t sec_at_1015 = s->timeToSeconds(101500);
	EXPECT_EQ(sec_at_1015, 4499u) << "10:15:00 should belong to the last second of section 1";

	//第二小节起点 10:30:00 紧接 4500
	EXPECT_EQ(s->timeToSeconds(103000), 4500u);

	//第二小节 10:30-11:30 共 3600 秒，累计 8100
	EXPECT_EQ(s->timeToSeconds(113000), 8099u);
	//第三小节起点
	EXPECT_EQ(s->timeToSeconds(133000), 8100u);

	s->release();
}

/*
 *	5秒对齐：每小节最后一根必须正好收在小节结束时刻，不跨小节
 *	这是选定 s5 作基础周期的前提
 */
TEST(test_session_secs, five_sec_alignment_closes_on_section_end)
{
	WTSSessionInfo* s = make_future_sess();

	//各小节秒数必须能被5整除，否则最后一根会跨小节
	const std::vector<uint32_t>& secMins = s->getSecMinList();
	uint32_t prev = 0;
	for (uint32_t cum : secMins)
	{
		uint32_t seclen = (cum - prev) * 60;
		EXPECT_EQ(seclen % 5, 0u) << "section length " << seclen << "s not divisible by 5";
		prev = cum;
	}

	//第一小节 4500 秒 / 5 = 900 根，最后一根收于 10:15:00
	uint32_t lastSecs = 4500;
	EXPECT_EQ(s->secondsToTime(lastSecs), 101500u);

	//开盘第一根 s5：09:00:00~09:00:04 的tick都归收于 09:00:05 这根
	for (uint32_t hms = 90000; hms <= 90004; hms++)
	{
		uint32_t cur = s->timeToSeconds(hms);
		uint32_t bar = (cur / 5) * 5 + 5;
		EXPECT_EQ(bar, 5u) << "hms " << hms;
		EXPECT_EQ(s->secondsToTime(bar), 90005u);
	}
	//第6秒进入下一根
	EXPECT_EQ(((s->timeToSeconds(90005) / 5) * 5 + 5), 10u);

	s->release();
}

TEST(test_session_secs, stock_session_divisible)
{
	WTSSessionInfo* s = make_stock_sess();

	//9:30-11:30 = 7200s, 13:00-15:00 = 7200s
	EXPECT_EQ(s->getTradingSeconds(), 14400u);
	EXPECT_EQ(s->timeToSeconds(93000), 0u);
	EXPECT_EQ(s->timeToSeconds(113000), 7199u);
	EXPECT_EQ(s->timeToSeconds(130000), 7200u);

	const std::vector<uint32_t>& secMins = s->getSecMinList();
	uint32_t prev = 0;
	for (uint32_t cum : secMins)
	{
		EXPECT_EQ(((cum - prev) * 60) % 5, 0u);
		prev = cum;
	}

	s->release();
}

TEST(test_session_secs, night_session_wraps_midnight)
{
	WTSSessionInfo* s = make_night_sess();

	//21:00-次日2:30 共 5.5 小时
	EXPECT_EQ(s->getTradingSeconds(), 5 * 3600 + 1800u);

	EXPECT_EQ(s->timeToSeconds(210000), 0u);
	EXPECT_EQ(s->timeToSeconds(210001), 1u);

	//跨零点后继续累计：23:59:59 -> 00:00:00 必须连续
	uint32_t before = s->timeToSeconds(235959);
	uint32_t after = s->timeToSeconds(0);
	EXPECT_EQ(after, before + 1) << "seconds must be continuous across midnight";

	//收盘时刻归最后一秒
	EXPECT_EQ(s->timeToSeconds(23000), s->getTradingSeconds() - 1);

	s->release();
}

TEST(test_session_secs, allday_wraps_86400)
{
	WTSSessionInfo* s = WTSSessionInfo::create("ALLDAY", "ALLDAY", 0);
	s->addTradingSection(0, 2400);

	EXPECT_EQ(s->getTradingSeconds(), 86400u);
	EXPECT_EQ(s->timeToSeconds(0), 0u);
	EXPECT_EQ(s->timeToSeconds(120000), 12 * 3600u);
	//一日最后一秒
	EXPECT_EQ(s->timeToSeconds(235959), 86399u);

	EXPECT_EQ(s->secondsToTime(0), 0u);
	EXPECT_EQ(s->secondsToTime(86399), 235959u);

	s->release();
}

/*
 *	集合竞价的行为固化
 *	timeToSeconds 里 isInAuctionTime 直接 return 0，
 *	意味着所有竞价tick都算第0秒，会全部落进第一根s5（收于09:00:05）
 *	这里把该行为写成测试，即规格；若将来要改为独立成一根，这个测试会先失败
 */
TEST(test_session_secs, auction_ticks_all_map_to_second_zero)
{
	WTSSessionInfo* s = make_future_sess();
	s->setAuctionTime(855, 900);

	EXPECT_TRUE(s->isInAuctionTime(856));

	//竞价区间内的时刻全部返回0
	EXPECT_EQ(s->timeToSeconds(85600), 0u);
	EXPECT_EQ(s->timeToSeconds(85930), 0u);
	//和开盘第一秒撞在同一个秒序号上
	EXPECT_EQ(s->timeToSeconds(90000), 0u);

	s->release();
}
