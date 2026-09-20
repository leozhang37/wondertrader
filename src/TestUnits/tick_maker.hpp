/*!
 * \file tick_maker.hpp
 * \project	WonderTrader
 *
 * \brief 测试用tick序列生成器
 *
 * 秒K线测试的公共依赖。除了正常tick，必须能注入下列异常tick——
 * 它们都是min1代码里历史修补过的case（源码中多处 By Wesley @ ... 注释），
 * 秒线会重新遇到同一批边界问题：
 *   1、集合竞价tick（timeToSeconds 会返回0）
 *   2、小节末尾超时tick，如 113000500
 *   3、零成交tick（turn_over == 0）
 *   4、price == 0 的异常tick（pipeToKlines 有 By Wesley @ 2023.07.05 特判）
 *   5、时间倒序tick
 */
#pragma once
#include <vector>
#include <string.h>

#include "../Includes/WTSStruct.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Share/TimeUtils.hpp"

USING_NS_WTP;

namespace tick_maker
{
	/*
	 *	HHMMSS + 秒数偏移 -> HHMMSS
	 *	注意这里是"自然时间"推进，不考虑交易小节，跨小节由调用方自己给时间点
	 */
	inline uint32_t advance_hms(uint32_t hms, uint32_t secs)
	{
		uint32_t h = hms / 10000;
		uint32_t m = hms % 10000 / 100;
		uint32_t s = hms % 100;

		uint32_t total = h * 3600 + m * 60 + s + secs;
		total %= 86400;

		return (total / 3600) * 10000 + (total % 3600 / 60) * 100 + total % 60;
	}

	/*
	 *	造一条tick
	 *	@hms	HHMMSS
	 *	@msec	毫秒部分，用来造 113000500 这类超时tick
	 */
	inline WTSTickStruct make(const char* code, uint32_t tdate, uint32_t adate,
		uint32_t hms, double price, double vol = 1, double turnover = 100, uint32_t msec = 0)
	{
		WTSTickStruct ts;
		memset(&ts, 0, sizeof(WTSTickStruct));

		strcpy(ts.exchg, "TEST");
		strcpy(ts.code, code);

		ts.trading_date = tdate;
		ts.action_date = adate;
		ts.action_time = hms * 1000 + msec;

		ts.price = price;
		ts.open = price;
		ts.high = price;
		ts.low = price;

		ts.volume = vol;
		ts.turn_over = turnover;
		ts.total_volume += vol;
		ts.open_interest = 10000;
		ts.diff_interest = 0;

		return ts;
	}

	/*
	 *	造一串等间隔tick
	 *	@start_hms	起始HHMMSS
	 *	@count		条数
	 *	@step_secs	间隔秒数
	 *	@px_seq		价格序列，按索引取，空则用 base_px
	 */
	inline std::vector<WTSTickStruct> series(const char* code, uint32_t tdate, uint32_t adate,
		uint32_t start_hms, uint32_t count, uint32_t step_secs = 1,
		double base_px = 100.0, const std::vector<double>& px_seq = std::vector<double>())
	{
		std::vector<WTSTickStruct> ret;
		ret.reserve(count);

		for (uint32_t i = 0; i < count; i++)
		{
			double px = px_seq.empty() ? base_px : px_seq[i % px_seq.size()];
			ret.emplace_back(make(code, tdate, adate, advance_hms(start_hms, i * step_secs), px));
		}

		return ret;
	}

	//零成交tick：有价格但没有成交量和成交额
	inline WTSTickStruct no_trade(const char* code, uint32_t tdate, uint32_t adate, uint32_t hms, double price)
	{
		return make(code, tdate, adate, hms, price, 0, 0);
	}

	//price为0的异常tick
	inline WTSTickStruct zero_price(const char* code, uint32_t tdate, uint32_t adate, uint32_t hms)
	{
		return make(code, tdate, adate, hms, 0.0, 1, 0);
	}

	//包装成WTSTickData，调用方负责release
	inline WTSTickData* wrap(WTSTickStruct& ts)
	{
		return WTSTickData::create(ts);
	}
}
