/*!
 * \file test_cond_entrust.cpp
 * \brief 条件单等价判定测试
 *
 * CondEntrust::same_as 是 mark & sweep 的正确性基础：
 * 判太松会把不同的挂单错认成同一条(策略改了价格却没生效)，
 * 判太严会让每次重挂都新建对象(退化成原来的清空+重建)
 */
#include "../WtCore/CtaStraBaseCtx.h"
#include "gtest/gtest/gtest.h"

USING_NS_WTP;

namespace
{
	CondEntrust make_cond(const char* code, char action, double target, double qty, const char* tag)
	{
		CondEntrust ce;
		wt_strcpy(ce._code, code);
		wt_strcpy(ce._usertag, tag);
		ce._field = WCF_NEWPRICE;
		ce._alg = WCT_LargerOrEqual;
		ce._action = action;
		ce._target = target;
		ce._qty = qty;
		return ce;
	}
}

TEST(test_cond_entrust, identical_conditions_are_same)
{
	CondEntrust a = make_cond("SHFE.rb.2610", COND_ACTION_OL, 3500.0, 2, "entry");
	CondEntrust b = make_cond("SHFE.rb.2610", COND_ACTION_OL, 3500.0, 2, "entry");

	EXPECT_TRUE(a.same_as(b));
	EXPECT_TRUE(b.same_as(a));
}

TEST(test_cond_entrust, stale_flag_does_not_affect_equality)
{
	CondEntrust a = make_cond("SHFE.rb.2610", COND_ACTION_OL, 3500.0, 2, "entry");
	CondEntrust b = a;

	a._stale = true;
	b._stale = false;

	//stale只是清理用的标记，不参与等价判定，
	//否则打了标记的单永远匹配不上策略重挂的单
	EXPECT_TRUE(a.same_as(b));
}

TEST(test_cond_entrust, different_target_is_not_same)
{
	CondEntrust a = make_cond("SHFE.rb.2610", COND_ACTION_OL, 3500.0, 2, "entry");
	CondEntrust b = make_cond("SHFE.rb.2610", COND_ACTION_OL, 3501.0, 2, "entry");

	//价格变了必须视为不同，否则策略调整挂单价会静默失效
	EXPECT_FALSE(a.same_as(b));
}

TEST(test_cond_entrust, different_qty_is_not_same)
{
	CondEntrust a = make_cond("SHFE.rb.2610", COND_ACTION_OL, 3500.0, 2, "entry");
	CondEntrust b = make_cond("SHFE.rb.2610", COND_ACTION_OL, 3500.0, 3, "entry");
	EXPECT_FALSE(a.same_as(b));
}

TEST(test_cond_entrust, different_action_is_not_same)
{
	//同价同量但方向不同（开多 vs 开空），绝不能认成同一条
	CondEntrust a = make_cond("SHFE.rb.2610", COND_ACTION_OL, 3500.0, 2, "entry");
	CondEntrust b = make_cond("SHFE.rb.2610", COND_ACTION_OS, 3500.0, 2, "entry");
	EXPECT_FALSE(a.same_as(b));
}

TEST(test_cond_entrust, different_usertag_is_not_same)
{
	//usertag是策略用来标记这笔单用途的，不同用途要分开跟踪
	CondEntrust a = make_cond("SHFE.rb.2610", COND_ACTION_OL, 3500.0, 2, "stop");
	CondEntrust b = make_cond("SHFE.rb.2610", COND_ACTION_OL, 3500.0, 2, "profit");
	EXPECT_FALSE(a.same_as(b));
}

TEST(test_cond_entrust, different_code_is_not_same)
{
	CondEntrust a = make_cond("SHFE.rb.2610", COND_ACTION_OL, 3500.0, 2, "entry");
	CondEntrust b = make_cond("SHFE.hc.2610", COND_ACTION_OL, 3500.0, 2, "entry");
	EXPECT_FALSE(a.same_as(b));
}

TEST(test_cond_entrust, different_alg_is_not_same)
{
	//止损和止盈可能同价同量，只有比较方向不同
	CondEntrust a = make_cond("SHFE.rb.2610", COND_ACTION_CL, 3500.0, 2, "exit");
	CondEntrust b = a;
	b._alg = WCT_SmallerOrEqual;

	EXPECT_FALSE(a.same_as(b))
		<< "a stop and a target at the same price must not collapse into one condition";
}

/*
 *	浮点比较要走decimal，不能用==
 *	否则 3500.0 和 3500.0000000001 会被判成不同，每次重挂都新建对象
 */
TEST(test_cond_entrust, target_uses_decimal_comparison)
{
	CondEntrust a = make_cond("SHFE.rb.2610", COND_ACTION_OL, 3500.0, 2, "entry");
	CondEntrust b = make_cond("SHFE.rb.2610", COND_ACTION_OL, 3500.0 + 1e-12, 2, "entry");

	EXPECT_TRUE(a.same_as(b)) << "tiny float noise must not be treated as a different condition";
}
