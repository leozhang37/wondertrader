/*!
 * \file test_lmdb_keys.cpp
 * \brief LMDB key 的字节序与容量测试
 *
 * AD 存储用 LMDB，key 是按字节比较的，所以数值字段都要转成大端，
 * 否则 memcmp 的顺序和数值顺序不一致，范围查询(get_range/get_uppers)
 * 会静默返回错误的结果集——这种错误不会崩，只会让数据看起来"少了几根"。
 */
#include <string.h>
#include <vector>

#include "../WtDataStorageAD/LMDBKeys.h"
#include "../Share/TimeUtils.hpp"
#include "gtest/gtest/gtest.h"

//LMDBKeys.h 的结构体和 TimeUtils 都在全局命名空间，不需要 USING_NS_WTP

namespace
{
	//按 LMDB 的方式比较两个key
	int key_cmp(const void* a, std::size_t la, const void* b, std::size_t lb)
	{
		std::size_t n = (la < lb) ? la : lb;
		int r = memcmp(a, b, n);
		if (r != 0) return r;
		return (int)(la - lb);
	}
}

TEST(test_lmdb_keys, reverse_endian_u64_roundtrip)
{
	const uint64_t vals[] = {
		0ULL, 1ULL, 0xFFULL, 0x1234ULL,
		20260920090005ULL, 20991231235959ULL, 0xFFFFFFFFFFFFFFFFULL
	};
	for (uint64_t v : vals)
	{
		//翻转两次必须回到原值
		EXPECT_EQ(reverseEndian(reverseEndian(v)), v) << "roundtrip failed for " << v;
	}
}

/*
 *	最关键的一条：转成大端之后，memcmp 的顺序必须和数值顺序一致。
 *	这是 LMDB 范围查询能工作的前提
 */
TEST(test_lmdb_keys, sec_key_byte_order_matches_numeric_order)
{
	const char* exchg = "TEST";
	const char* code = "rb2610";

	//一串递增的秒线时间戳，跨日、跨月
	std::vector<uint64_t> times = {
		TimeUtils::timeToSecBar(20260920, 90005),
		TimeUtils::timeToSecBar(20260920, 90010),
		TimeUtils::timeToSecBar(20260920, 113000),
		TimeUtils::timeToSecBar(20260921, 90005),
		TimeUtils::timeToSecBar(20261001, 90005),
		TimeUtils::timeToSecBar(20270101, 90005),
	};

	std::vector<LMDBSecBarKey> keys;
	for (uint64_t t : times)
		keys.emplace_back(LMDBSecBarKey(exchg, code, t));

	for (std::size_t i = 1; i < keys.size(); i++)
	{
		ASSERT_LT(times[i-1], times[i]) << "test data itself is not increasing";

		int c = key_cmp(&keys[i-1], sizeof(LMDBSecBarKey), &keys[i], sizeof(LMDBSecBarKey));
		EXPECT_LT(c, 0)
			<< "byte order does not follow numeric order between "
			<< times[i-1] << " and " << times[i]
			<< " -- LMDB range queries would silently return wrong results";
	}
}

//分钟线的key也验一下，确认原有行为没被破坏
TEST(test_lmdb_keys, min_key_byte_order_matches_numeric_order)
{
	const char* exchg = "TEST";
	const char* code = "rb2610";

	std::vector<uint32_t> times = {
		(uint32_t)TimeUtils::timeToMinBar(20260920, 901),
		(uint32_t)TimeUtils::timeToMinBar(20260920, 902),
		(uint32_t)TimeUtils::timeToMinBar(20260920, 1130),
		(uint32_t)TimeUtils::timeToMinBar(20260921, 901),
		(uint32_t)TimeUtils::timeToMinBar(20270101, 901),
	};

	std::vector<LMDBBarKey> keys;
	for (uint32_t t : times)
		keys.emplace_back(LMDBBarKey(exchg, code, t));

	for (std::size_t i = 1; i < keys.size(); i++)
	{
		ASSERT_LT(times[i-1], times[i]);
		int c = key_cmp(&keys[i-1], sizeof(LMDBBarKey), &keys[i], sizeof(LMDBBarKey));
		EXPECT_LT(c, 0) << "min bar key order broken between " << times[i-1] << " and " << times[i];
	}
}

/*
 *	为什么秒线必须用独立的key结构
 *
 *	准确地说：截断到 uint32 一定会丢信息(秒线时间戳约2e13，远超uint32)，
 *	至于"两个不同时刻撞成同一个key"，需要两者相差 2^32 的整数倍。
 *	在实际的日期范围内这种碰撞很罕见——回绕点间隔4.3e9，
 *	而相邻交易日的时间戳只跳1e6左右，大约4300天才跨越一次。
 *	所以这里不去断言"任意一对都会出错"(那是夸大)，
 *	而是证明两件确定的事：信息一定丢失，且构造得出真实的碰撞
 */
TEST(test_lmdb_keys, u32_key_cannot_hold_sec_timestamps)
{
	uint64_t t = TimeUtils::timeToSecBar(20260920, 90005);

	//秒线时间戳本身就超出了uint32的范围
	EXPECT_GT(t, (uint64_t)0xFFFFFFFFULL);

	//截断一定丢信息
	EXPECT_NE((uint64_t)(uint32_t)t, t)
		<< "truncation must lose information for sec timestamps";

	/*
	 *	构造一对相差正好 2^32 的时间戳：
	 *	它们是两个不同的uint64，但截断进uint32后完全相同。
	 *	这里不要求日期合法，考察的是key结构的容量而非日期语义
	 */
	uint64_t a = t;
	uint64_t b = t + (1ULL << 32);
	ASSERT_NE(a, b);
	EXPECT_EQ((uint32_t)a, (uint32_t)b);

	LMDBBarKey k1("TEST", "rb2610", (uint32_t)a);
	LMDBBarKey k2("TEST", "rb2610", (uint32_t)b);
	EXPECT_EQ(memcmp(&k1, &k2, sizeof(LMDBBarKey)), 0)
		<< "two distinct timestamps collapsed into the same uint32 key";

	//用uint64的key结构就能区分
	LMDBSecBarKey s1("TEST", "rb2610", a);
	LMDBSecBarKey s2("TEST", "rb2610", b);
	EXPECT_NE(memcmp(&s1, &s2, sizeof(LMDBSecBarKey)), 0);
	EXPECT_LT(key_cmp(&s1, sizeof(s1), &s2, sizeof(s2)), 0);
}

TEST(test_lmdb_keys, sec_key_layout_is_packed)
{
	//key 会被整块memcpy进LMDB，中间不能有编译器填充
	EXPECT_EQ(sizeof(LMDBSecBarKey),
		(std::size_t)(MAX_EXCHANGE_LENGTH + MAX_INSTRUMENT_LENGTH + sizeof(uint64_t)));
}
