/*!
 * \file test_diff_executer.cpp
 *
 * 差量执行器(WtDiffExecuter)的集成测试
 * 使用真实的 WTSBaseDataMgr 和 WtExeFact 中的 WtDiffMinImpactExeUnit, 不连接交易通道(不下单)
 *
 * 需要环境变量 WT_TEST_LIBDIR 指向包含 executer/ 子目录(内有 WtExeFact 动态库)的目录,
 * 例如 build_macos/build_x64/Release/bin/WtPorter/
 */
#include <stdio.h>
#include <string>

#include "../WtCore/WtDiffExecuter.h"
#include "../WtCore/WtExecuterFactory.h"
#include "../WtCore/WtHelper.h"
#include "../WTSTools/WTSBaseDataMgr.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Share/StrUtil.hpp"
#include "../Share/fmtlib.h"
#include "gtest/gtest/gtest.h"

#include <rapidjson/document.h>
#include <boost/filesystem.hpp>

namespace fs = boost::filesystem;
namespace rj = rapidjson;

USING_NS_WTP;

namespace
{
	const char* DX_CODE = "SHFE.rb.2610";

	void write_file(const std::string& path, const std::string& content)
	{
		FILE* f = fopen(path.c_str(), "wb");
		fwrite(content.data(), 1, content.size(), f);
		fclose(f);
	}

	std::string read_file(const std::string& path)
	{
		std::string ret;
		FILE* f = fopen(path.c_str(), "rb");
		if (f == NULL)
			return ret;
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			ret.append(buf, n);
		fclose(f);
		return ret;
	}

	/*
	 *	最小化的基础数据: 一个品种(SHFE.rb)、一个合约(rb2610)、一个交易时段
	 */
	struct DxEnv
	{
		std::string			dir;
		WTSBaseDataMgr		bd;
		WtExecuterFactory	fact;
		bool				ready = false;

		DxEnv()
		{
			dir = "./ut_diffexec/";
			if (fs::exists(dir))
				fs::remove_all(dir);
			fs::create_directories(dir);
			WtHelper::setGenerateDir(dir.c_str());

			write_file(dir + "sessions.json", R"({"FN2300":{"name":"FN2300","offset":300,"auction":{"from":2059,"to":2100},)"
				R"("sections":[{"from":2100,"to":2300},{"from":900,"to":1015},{"from":1030,"to":1130},{"from":1330,"to":1500}]}})");
			write_file(dir + "commodities.json", R"({"SHFE":{"rb":{"covermode":1,"pricemode":1,"category":1,"trademode":0,)"
				R"("precision":0,"pricetick":1.0,"volscale":10,"name":"rb","exchg":"SHFE","session":"FN2300","holiday":"CHINA"}}})");
			write_file(dir + "contracts.json", R"({"SHFE":{"rb2610":{"name":"rb2610","code":"rb2610","exchg":"SHFE","product":"rb",)"
				R"("maxlimitqty":500,"maxmarketqty":60,"minlimitqty":1,"minmarketqty":1,"opendate":20251016,"expiredate":20261015}}})");

			bd.loadSessions((dir + "sessions.json").c_str());
			bd.loadCommodities((dir + "commodities.json").c_str());
			bd.loadContracts((dir + "contracts.json").c_str());

			const char* libdir = getenv("WT_TEST_LIBDIR");
			if (libdir != NULL && strlen(libdir) > 0)
			{
				std::string exeDir = StrUtil::standardisePath(std::string(libdir)) + "executer/";
				ready = fact.loadFactories(exeDir.c_str());
			}
		}
	};

	/*
	 *	执行器通过引擎提供的 stub 取品种和交易时段信息, 这里直接从基础数据取
	 */
	class DxStub : public IExecuterStub
	{
	public:
		DxStub(WTSBaseDataMgr* bd) : _bd(bd) {}
		uint64_t get_real_time() override { return 0; }
		WTSCommodityInfo* get_comm_info(const char* stdCode) override { return _bd->getCommodity("SHFE", "rb"); }
		WTSSessionInfo* get_sess_info(const char* stdCode) override { return _bd->getSession("FN2300"); }
		IHotMgr* get_hot_mon() override { return NULL; }
		uint32_t get_trading_day() override { return 20260924; }
	private:
		WTSBaseDataMgr* _bd;
	};

	DxEnv& env()
	{
		//有意不释放: 进程退出时析构已加载的执行器工厂会卡住, 测试环境随进程结束即可
		static DxEnv* e = new DxEnv();
		return *e;
	}

	WTSVariant* make_cfg()
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("scale", 1.0);
		WTSVariant* policy = WTSVariant::createObject();
		WTSVariant* def = WTSVariant::createObject();
		def->append("name", "WtExeFact.WtDiffMinImpactExeUnit");
		def->append("offset", (int32_t)0);
		def->append("expire", (uint32_t)5);
		def->append("pricemode", (int32_t)1);
		def->append("span", (uint32_t)500);
		def->append("byrate", false);
		def->append("lots", 1.0);
		def->append("rate", 0.0);
		policy->append("default", def, false);
		cfg->append("policy", policy, false);
		return cfg;
	}

	std::string data_file(const char* name)
	{
		return std::string(WtHelper::getExecDataDir()) + name + ".json";
	}

	//从执行器落地的数据文件中读取某个合约的理论目标和差量
	void read_state(const char* name, const char* code, double& target, double& diff)
	{
		target = diff = 0;
		rj::Document root;
		root.Parse(read_file(data_file(name)).c_str());
		ASSERT_FALSE(root.HasParseError());
		for (auto& it : root["targets"].GetArray())
			if (strcmp(it["code"].GetString(), code) == 0)
				target = it["target"].GetDouble();
		for (auto& it : root["diffs"].GetArray())
			if (strcmp(it["code"].GetString(), code) == 0)
				diff = it["diff"].GetDouble();
	}

	wt_hashmap<std::string, double> targets_of(double qty)
	{
		wt_hashmap<std::string, double> ret;
		ret[DX_CODE] = qty;
		return ret;
	}
}

/*
 *	标准代码不能直接用 getContract(code) 查询, 要拆成原始代码+交易所
 *	这是差量执行器"不在目标中的持仓自动设为0"分支原来一直走不到的原因
 */
TEST(test_diff_executer, base_data_lookup_needs_raw_code)
{
	DxEnv& e = env();
	EXPECT_EQ(e.bd.getContract(DX_CODE), nullptr);
	EXPECT_NE(e.bd.getContract("rb2610", "SHFE"), nullptr);
}

/*
 *	目标从 3 显式变成 0: 差量应为 0 - 3 = -3(卖出自己的 3 手)
 */
TEST(test_diff_executer, explicit_zero_target_sells_own_position)
{
	DxEnv& e = env();
	ASSERT_TRUE(e.ready) << "cannot load WtExeFact, is WT_TEST_LIBDIR set?";

	const char* name = "dx_explicit";
	fs::remove(data_file(name));
	WtDiffExecuter exec(&e.fact, name, NULL, &e.bd);
	DxStub stub(&e.bd);
	exec.setStub(&stub);
	WTSVariant* cfg = make_cfg();
	ASSERT_TRUE(exec.init(cfg));

	double target, diff;
	exec.set_position(targets_of(3));
	read_state(name, DX_CODE, target, diff);
	EXPECT_EQ(target, 3);
	EXPECT_EQ(diff, 3);

	exec.set_position(targets_of(0));
	read_state(name, DX_CODE, target, diff);
	EXPECT_EQ(target, 0);
	EXPECT_EQ(diff, 0);		//没有成交, 3 - 3 = 0
	cfg->release();
}

/*
 *	合约从目标中消失(路由目标里没有它/策略被过滤/换月后重启): 结果必须和显式设为 0 完全一致
 */
TEST(test_diff_executer, dropped_code_is_treated_as_zero_target)
{
	DxEnv& e = env();
	ASSERT_TRUE(e.ready) << "cannot load WtExeFact, is WT_TEST_LIBDIR set?";

	const char* name = "dx_dropped";
	fs::remove(data_file(name));
	WtDiffExecuter exec(&e.fact, name, NULL, &e.bd);
	DxStub stub(&e.bd);
	exec.setStub(&stub);
	WTSVariant* cfg = make_cfg();
	ASSERT_TRUE(exec.init(cfg));

	double target, diff;
	exec.set_position(targets_of(3));
	read_state(name, DX_CODE, target, diff);
	EXPECT_EQ(diff, 3);

	wt_hashmap<std::string, double> empty;
	exec.set_position(empty);
	read_state(name, DX_CODE, target, diff);
	EXPECT_EQ(target, 0);
	EXPECT_EQ(diff, 0);		//与显式设为0一致; 若方向写反会得到 6(再买3手), 若分支走不到会保持 3 且目标不变
	cfg->release();
}

/*
 *	只有自己发出的订单的成交才扣减差量
 *	通过数据文件预置"上次会话中自己发出的订单号", 模拟重启后的识别
 */
TEST(test_diff_executer, only_own_trades_update_diff)
{
	DxEnv& e = env();
	ASSERT_TRUE(e.ready) << "cannot load WtExeFact, is WT_TEST_LIBDIR set?";

	const char* name = "dx_owntrade";
	write_file(data_file(name), fmtutil::format(R"({{"targets":[{{"code":"{0}","target":5}}],"diffs":[{{"code":"{0}","diff":5}}],"orders":[1001]}})", DX_CODE));

	WtDiffExecuter exec(&e.fact, name, NULL, &e.bd);
	DxStub stub(&e.bd);
	exec.setStub(&stub);
	WTSVariant* cfg = make_cfg();
	ASSERT_TRUE(exec.init(cfg));
	exec.set_position(targets_of(5));	//目标不变, 只是为了创建执行单元

	double target, diff;
	exec.on_trade(2002, DX_CODE, true, 2, 3000);	//别的执行器的订单成交
	read_state(name, DX_CODE, target, diff);
	EXPECT_EQ(diff, 5);

	exec.on_trade(0, DX_CODE, true, 1, 3000);		//非 WT 订单(手工单)的成交
	read_state(name, DX_CODE, target, diff);
	EXPECT_EQ(diff, 5);

	exec.on_trade(1001, DX_CODE, true, 2, 3000);	//自己的订单成交
	read_state(name, DX_CODE, target, diff);
	EXPECT_EQ(diff, 3);

	//自己的订单号要随数据一起落地
	rj::Document root;
	root.Parse(read_file(data_file(name)).c_str());
	ASSERT_TRUE(root.HasMember("orders"));
	bool found = false;
	for (auto& v : root["orders"].GetArray())
		found = found || v.GetUint() == 1001;
	EXPECT_TRUE(found);
	cfg->release();
}
