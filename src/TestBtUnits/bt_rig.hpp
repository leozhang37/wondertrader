/*!
 * \file bt_rig.hpp
 * \brief 回测环境搭建：HisDataReplayer + CtaMocker + 真实策略
 *
 * 用途是给 HisDataReplayer 的改造建立回归基线。
 * 这里刻意用真实的 WtCtaStraFact/DualThrust 而不是桩策略：
 * 桩策略不下单，跑出来的 trades/closes 都是空的，
 * 那样的基线对"改动有没有影响成交"毫无约束力。
 */
#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

#include "../WtBtCore/HisDataReplayer.h"
#include "../WtBtCore/CtaMocker.h"
#include "../WtBtCore/WtHelper.h"
#include "../Includes/CtaStrategyDefs.h"
#include "../Includes/ICtaStraCtx.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Share/decimal.h"
#include "../Includes/WTSVariant.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/DLLHelper.hpp"
#include "../Share/StrUtil.hpp"
#include "../Share/BoostFile.hpp"
#include "../Share/fmtlib.h"
#include "../WtDataStorage/DataDefine.h"

#include <boost/filesystem.hpp>

USING_NS_WTP;

namespace bt_rig
{
	namespace bfs = boost::filesystem;

	//HHMMSS 加秒
	inline uint32_t advance_hms_local(uint32_t hms, uint32_t secs)
	{
		uint32_t h = hms / 10000, m = hms % 10000 / 100, s = hms % 100;
		uint32_t total = h * 3600 + m * 60 + s + secs;
		total %= 86400;
		return (total / 3600) * 10000 + (total % 3600 / 60) * 100 + total % 60;
	}

	inline std::string lib_dir()
	{
		const char* d = getenv("WT_TEST_LIBDIR");
		if (d == NULL || strlen(d) == 0)
			return "";
		return StrUtil::standardisePath(std::string(d));
	}

	//----------------------------------------------------------------
	// 基础数据文件：HisDataReplayer 用自己的 WTSBaseDataMgr，
	// 只能通过 basefiles 配置喂给它，所以这里得把json造出来
	//----------------------------------------------------------------
	inline void write_basefiles(const std::string& dir, const char* exchg, const char* pid, const char* code)
	{
		//会话：上午一段、下午一段，都是5的整数倍秒数
		std::string sess = R"({
    "TESTSESS": {
        "name": "test session",
        "offset": 0,
        "sections": [
            { "from": 900, "to": 1015 },
            { "from": 1030, "to": 1130 }
        ]
    }
})";
		StdFile::write_file_content((dir + "sessions.json").c_str(), sess);

		std::string comm = fmtutil::format(R"({{
    "{}": {{
        "{}": {{
            "covermode": 0,
            "pricemode": 0,
            "category": 1,
            "precision": 1,
            "pricetick": 1,
            "volscale": 10,
            "name": "test commodity",
            "exchg": "{}",
            "session": "TESTSESS",
            "holiday": ""
        }}
    }}
}})", exchg, pid, exchg);
		StdFile::write_file_content((dir + "commodities.json").c_str(), comm);

		std::string ctr = fmtutil::format(R"({{
    "{}": {{
        "{}": {{
            "name": "test contract",
            "code": "{}",
            "exchg": "{}",
            "product": "{}",
            "maxlimitqty": 100,
            "maxmarketqty": 100
        }}
    }}
}})", exchg, code, code, exchg, pid);
		StdFile::write_file_content((dir + "contracts.json").c_str(), ctr);
	}

	/*
	 *	造 min1 历史K线
	 *	价格走一个带噪声的趋势，保证 DualThrust 能被触发出成交，
	 *	否则基线里没有交易记录，对回归没有约束力
	 */
	inline uint32_t write_min1_bars(const std::string& dir, const char* exchg, const char* code,
		uint32_t startDate, uint32_t days)
	{
		std::string path = fmtutil::format("{}his/min1/{}/", dir, exchg);
		bfs::create_directories(path);

		std::vector<WTSBarStruct> bars;

		//每天两小节：9:00-10:15(75分钟) + 10:30-11:30(60分钟) = 135根
		const uint32_t mins1 = 75, mins2 = 60;
		uint32_t date = startDate;
		double px = 1000.0;

		for (uint32_t d = 0; d < days; d++)
		{
			for (uint32_t seg = 0; seg < 2; seg++)
			{
				uint32_t startHm = (seg == 0) ? 900 : 1030;
				uint32_t cnt = (seg == 0) ? mins1 : mins2;

				for (uint32_t i = 1; i <= cnt; i++)
				{
					//HHMM 递增
					uint32_t h = startHm / 100;
					uint32_t m = startHm % 100 + i;
					h += m / 60; m %= 60;
					uint32_t hm = h * 100 + m;

					//确定性的伪随机，保证每次跑出来的数据完全一样
					uint32_t seed = d * 1000 + seg * 500 + i;
					double wave = ((seed * 1103515245u + 12345u) % 1000) / 100.0 - 5.0;
					double trend = (d % 4 < 2) ? 0.6 : -0.6;
					px += trend + wave * 0.4;
					if (px < 100.0) px = 100.0;

					WTSBarStruct b;
					b.date = date;
					b.time = TimeUtils::timeToMinBar(date, hm);
					b.open = px - 1.0;
					b.high = px + 2.0;
					b.low = px - 2.0;
					b.close = px;
					b.vol = 100 + (seed % 50);
					b.money = b.close * b.vol * 10;
					b.hold = 10000;
					b.add = 0;
					bars.emplace_back(b);
				}
			}
			date = TimeUtils::getNextDate(date);
		}

		//未压缩写出，读取端两种格式都认
		BlockHeader hdr;
		memset(&hdr, 0, sizeof(hdr));
		strcpy(hdr._blk_flag, BLK_FLAG);
		hdr._type = BT_HIS_Minute1;
		hdr._version = BLOCK_VERSION_RAW_V2;

		std::string file = fmtutil::format("{}{}.dsb", path, code);
		BoostFile f;
		f.create_new_file(file.c_str());
		f.write_file(&hdr, sizeof(hdr));
		f.write_file(bars.data(), sizeof(WTSBarStruct) * bars.size());
		f.close_file();

		return (uint32_t)bars.size();
	}

	/*
	 *	测试用策略：订阅 m1 主K线，按固定规律下单
	 *
	 *	刻意不用 WtCtaStraFact 里的 DualThrust：
	 *	基线要锁住的是"回测框架的行为"，而不是某个策略的实现。
	 *	策略逻辑写在这里，参数和下单节奏完全可控，
	 *	同时又真的走了下单/成交/平仓的完整路径，输出里才会有 trades 和 closes
	 */
	class BtTestStrategy : public CtaStrategy
	{
	public:
		BtTestStrategy(const char* id, const char* code, const char* period, uint32_t count)
			: CtaStrategy(id), _code(code), _period(period), _count(count), _calls(0) {}

		virtual const char* getName() override { return "BtTestStrategy"; }
		virtual const char* getFactName() override { return "BtTestFact"; }

		virtual void on_init(ICtaStraCtx* ctx) override
		{
			WTSKlineSlice* s = ctx->stra_get_bars(_code.c_str(), _period.c_str(), _count, true);
			if (s) s->release();
		}

		virtual void on_schedule(ICtaStraCtx* ctx, uint32_t uDate, uint32_t uTime) override
		{
			WTSKlineSlice* kline = ctx->stra_get_bars(_code.c_str(), _period.c_str(), _count, true);
			if (kline == NULL)
				return;
			if (kline->size() < 20)
			{
				kline->release();
				return;
			}

			//简单的均线交叉：快线在慢线上方做多，下方做空
			double fast = 0, slow = 0;
			for (int32_t i = 0; i < 5; i++)
				fast += kline->at(-1 - i)->close;
			for (int32_t i = 0; i < 20; i++)
				slow += kline->at(-1 - i)->close;
			fast /= 5; slow /= 20;

			double target = (fast > slow) ? 1.0 : -1.0;
			double cur = ctx->stra_get_position(_code.c_str());
			if (!decimal::eq(cur, target))
				ctx->stra_set_position(_code.c_str(), target, "ma_cross");

			_calls++;
			kline->release();
		}

		uint32_t calls() const { return _calls; }

	private:
		std::string	_code;
		std::string	_period;
		uint32_t	_count;
		uint32_t	_calls;
	};

	/*
	 *	CtaMocker 的 _strategy 是 protected 且只有 init_cta_factory 这一条
	 *	通过 dlopen 设置的路径，这里继承一下把测试策略直接注进去
	 */
	class InjectableMocker : public CtaMocker
	{
	public:
		InjectableMocker(HisDataReplayer* r, const char* name)
			: CtaMocker(r, name, 0, true, NULL, false) {}

		void inject(CtaStrategy* s)
		{
			_strategy = s;
			_name = s->id();
		}
	};

	/*
	 *	造 sec5 历史K线
	 *	时间戳用 yyyyMMddHHmmss(timeToSecBar)，和落盘格式一致
	 */
	inline uint32_t write_sec5_bars(const std::string& dir, const char* exchg, const char* code,
		uint32_t startDate, uint32_t days)
	{
		std::string path = fmtutil::format("{}his/sec5/{}/", dir, exchg);
		bfs::create_directories(path);

		std::vector<WTSBarStruct> bars;

		//两小节：9:00-10:15(4500秒) + 10:30-11:30(3600秒)，每5秒一根
		uint32_t date = startDate;
		double px = 1000.0;

		for (uint32_t d = 0; d < days; d++)
		{
			for (uint32_t seg = 0; seg < 2; seg++)
			{
				uint32_t startHms = (seg == 0) ? 90000 : 103000;
				uint32_t secs = (seg == 0) ? 4500 : 3600;

				for (uint32_t s = 5; s <= secs; s += 5)
				{
					uint32_t hms = advance_hms_local(startHms, s);

					uint32_t seed = d * 10000 + seg * 5000 + s;
					double wave = ((seed * 1103515245u + 12345u) % 1000) / 100.0 - 5.0;
					double trend = (d % 4 < 2) ? 0.05 : -0.05;
					px += trend + wave * 0.1;
					if (px < 100.0) px = 100.0;

					WTSBarStruct b;
					b.date = date;
					b.time = TimeUtils::timeToSecBar(date, hms);
					b.open = px - 0.5;
					b.high = px + 1.0;
					b.low = px - 1.0;
					b.close = px;
					b.vol = 10 + (seed % 20);
					b.money = b.close * b.vol * 10;
					b.hold = 10000;
					b.add = 0;
					bars.emplace_back(b);
				}
			}
			date = TimeUtils::getNextDate(date);
		}

		BlockHeader hdr;
		memset(&hdr, 0, sizeof(hdr));
		strcpy(hdr._blk_flag, BLK_FLAG);
		hdr._type = BT_HIS_Sec5;
		hdr._version = BLOCK_VERSION_RAW_V2;

		std::string file = fmtutil::format("{}{}.dsb", path, code);
		BoostFile f;
		f.create_new_file(file.c_str());
		f.write_file(&hdr, sizeof(hdr));
		f.write_file(bars.data(), sizeof(WTSBarStruct) * bars.size());
		f.close_file();

		return (uint32_t)bars.size();
	}

	//把输出目录里的csv读出来，用于逐字节比对
	inline std::string read_output(const std::string& outDir, const char* straName, const char* fname)
	{
		std::string p = fmtutil::format("{}{}/{}", outDir, straName, fname);
		if (!StdFile::exists(p.c_str()))
			return std::string("<missing>");

		std::string content;
		StdFile::read_file_content(p.c_str(), content);
		return content;
	}
}
