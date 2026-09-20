/*!
 * \file test_secbar_time.cpp
 * \brief 秒线bar时间戳编码测试
 *
 * 秒线编码：yyyyMMddHHmmss（不减19900000偏移）
 * 分钟线编码：(date-19900000)*10000+HHMM
 * 两者刻意相差5个数量级，一旦被误比较会立刻暴露
 */
#include "../Share/TimeUtils.hpp"
#include "../Includes/WTSTypes.h"
#include "gtest/gtest/gtest.h"

USING_NS_WTP;

TEST(test_secbar_time, encode_roundtrip)
{
	//普通时刻
	uint64_t t = TimeUtils::timeToSecBar(20260920, 143025);
	EXPECT_EQ(t, 20260920143025ULL);
	EXPECT_EQ(TimeUtils::secBarToDate(t), 20260920u);
	EXPECT_EQ(TimeUtils::secBarToTime(t), 143025u);

	//零点整
	t = TimeUtils::timeToSecBar(20260920, 0);
	EXPECT_EQ(t, 20260920000000ULL);
	EXPECT_EQ(TimeUtils::secBarToDate(t), 20260920u);
	EXPECT_EQ(TimeUtils::secBarToTime(t), 0u);

	//一日最后一秒
	t = TimeUtils::timeToSecBar(20260920, 235959);
	EXPECT_EQ(TimeUtils::secBarToDate(t), 20260920u);
	EXPECT_EQ(TimeUtils::secBarToTime(t), 235959u);
}

TEST(test_secbar_time, encode_boundary_dates)
{
	//下界：框架里各处用19900000做基准
	uint64_t lo = TimeUtils::timeToSecBar(19900101, 0);
	EXPECT_EQ(TimeUtils::secBarToDate(lo), 19900101u);
	EXPECT_EQ(TimeUtils::secBarToTime(lo), 0u);

	//上界：确认不溢出uint64
	uint64_t hi = TimeUtils::timeToSecBar(20991231, 235959);
	EXPECT_EQ(hi, 20991231235959ULL);
	EXPECT_EQ(TimeUtils::secBarToDate(hi), 20991231u);
	EXPECT_EQ(TimeUtils::secBarToTime(hi), 235959u);

	//约2.1e13，离uint64上限(1.8e19)还有6个数量级余量
	EXPECT_LT(hi, 100000000000000ULL);
}

TEST(test_secbar_time, monotonic_within_day)
{
	uint64_t prev = 0;
	//同日内逐秒递增，编码必须严格单调
	const uint32_t hmss[] = { 90000, 90001, 90059, 90100, 95959, 100000, 133000, 145959, 150000 };
	for (uint32_t hms : hmss)
	{
		uint64_t cur = TimeUtils::timeToSecBar(20260920, hms);
		EXPECT_GT(cur, prev) << "not monotonic at " << hms;
		prev = cur;
	}
}

TEST(test_secbar_time, monotonic_across_day)
{
	//跨日递增：夜盘 20260920 21:00 -> 次日 02:30
	uint64_t night = TimeUtils::timeToSecBar(20260920, 210000);
	uint64_t nextEarly = TimeUtils::timeToSecBar(20260921, 23000);
	EXPECT_GT(nextEarly, night);

	//跨月、跨年
	EXPECT_GT(TimeUtils::timeToSecBar(20261001, 0), TimeUtils::timeToSecBar(20260930, 235959));
	EXPECT_GT(TimeUtils::timeToSecBar(20270101, 0), TimeUtils::timeToSecBar(20261231, 235959));
}

/*
 *	秒编码与分钟编码必须不重叠
 *	这是设计上的安全网：两种编码混用时，量级差会让断言立刻失败，
 *	而不是静默算出一个看似合理的错误结果
 */
TEST(test_secbar_time, magnitude_disjoint_from_minbar)
{
	uint64_t minbar_max = TimeUtils::timeToMinBar(20991231, 2359);
	uint64_t secbar_min = TimeUtils::timeToSecBar(19900101, 0);

	//分钟线最大值必须远小于秒线最小值
	EXPECT_LT(minbar_max, secbar_min);

	//量级差至少3个数量级
	EXPECT_GT(secbar_min / (minbar_max + 1), 1000ULL);
}

TEST(test_secbar_time, period_name_matches_enum)
{
	//PERIOD_NAME 用 period-KP_Tick 做下标，KP_Sec5 追加在末尾必须对齐
	EXPECT_STREQ(PERIOD_NAME[KP_Tick - KP_Tick], "tick");
	EXPECT_STREQ(PERIOD_NAME[KP_Minute1 - KP_Tick], "min1");
	EXPECT_STREQ(PERIOD_NAME[KP_Minute5 - KP_Tick], "min5");
	EXPECT_STREQ(PERIOD_NAME[KP_DAY - KP_Tick], "day");
	EXPECT_STREQ(PERIOD_NAME[KP_Hour - KP_Tick], "hour");
	EXPECT_STREQ(PERIOD_NAME[KP_Half - KP_Tick], "half");
	EXPECT_STREQ(PERIOD_NAME[KP_Sec5 - KP_Tick], "sec5");
}

/*
 *	既有枚举数值不能变
 *	一旦变化，已落盘的AD库名、外部loader、wtpy侧的period数值都会错位
 */
TEST(test_secbar_time, enum_values_frozen)
{
	EXPECT_EQ((uint32_t)KP_Tick, 0u);
	EXPECT_EQ((uint32_t)KP_Minute1, 1u);
	EXPECT_EQ((uint32_t)KP_Minute5, 2u);
	EXPECT_EQ((uint32_t)KP_DAY, 3u);
	EXPECT_EQ((uint32_t)KP_Hour, 4u);
	EXPECT_EQ((uint32_t)KP_Half, 5u);
	//新增的必须在末尾
	EXPECT_EQ((uint32_t)KP_Sec5, 6u);
}
