/*!
 * \file SecBarBuilder.hpp
 * \project	WonderTrader
 *
 * \brief 5秒线的拼接逻辑
 *
 * By 历史tick转sec5 @ 2026.09.21
 * 实盘落盘(WtDataWriter)和离线转换(WtDtHelper::trans_ticks_to_sec5)共用这一份逻辑，
 * 保证同一批tick两边产出的sec5逐根一致，回测和实盘才能对得上。
 *
 * 只有头文件，因为 WtDataStorage 不链接 WTSTools，而 WtDtHelper 又不能把
 * WtDataWriter 的源码编进来，放在这里两边都能直接include。
 */
#pragma once
#include <string>
#include <algorithm>

#include "../Includes/WTSStruct.h"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/decimal.h"

NS_WTP_BEGIN

namespace sec5
{
	//与 WtDataWriter 的同名配置项一一对应
	struct Options
	{
		bool		_skip_notrade_tick;
		bool		_skip_notrade_bar;
		uint32_t	_min_price_mode;

		Options() : _skip_notrade_tick(false), _skip_notrade_bar(false), _min_price_mode(0) {}
	};

	//tick创新高/新低的标记，与 WTSTickData::isNewHigh/isNewLow 的位定义一致
	const uint32_t LF_NEW_HIGH = 1;
	const uint32_t LF_NEW_LOW = 2;

	enum UpdateResult
	{
		UR_DROP = 0,	//这笔tick没有并入任何bar
		UR_NEW = 1,		//新建了一根bar，内容写在 newBar 里
		UR_UPDATE = 2	//更新了 lastBar
	};

	/*
	 *	pipeToKlines 开头的两道过滤，min1/min5/sec5 共用
	 *	返回 false 表示这笔tick不进任何K线
	 */
	inline bool accept_tick(const WTSTickStruct& tick, WTSSessionInfo* sInfo, bool bSkipNoTradeBar)
	{
		bool tickNoTrade = decimal::eq(tick.turn_over, 0);
		if (bSkipNoTradeBar && tickNoTrade)
			return false;

		uint32_t curTime = tick.action_time / 100000;
		return sInfo->timeToMinutes(curTime, false) != INVALID_UINT32;
	}

	/*
	 *	算这笔tick归属的5秒bar的时间戳(yyyyMMddHHmmss)
	 *	不在交易时段内返回0
	 */
	inline uint64_t bar_time(const WTSTickStruct& tick, WTSSessionInfo* sInfo)
	{
		//tick的秒级时间戳，HHMMSS
		uint32_t curSecTime = tick.action_time / 1000;
		uint32_t seconds = sInfo->timeToSeconds(curSecTime);
		if (seconds == INVALID_UINT32)
			return 0;

		uint32_t barSecs = (seconds / 5) * 5 + 5;
		uint32_t barTime = sInfo->secondsToTime(barSecs);
		uint32_t barDate = tick.action_date;
		if (barTime < curSecTime)
		{
			//bar的收盘时刻小于tick时刻，说明跨日了
			barDate = TimeUtils::getNextDate(barDate);
		}
		return TimeUtils::timeToSecBar(barDate, barTime);
	}

	/*
	 *	把一笔tick并入5秒线
	 *	@limitFlag	LF_NEW_HIGH/LF_NEW_LOW 的组合
	 *	@lastBar	当前最后一根bar，没有则传NULL
	 *	@newBar		返回 UR_NEW 时新bar写在这里。
	 *				只写下面列出的字段，其余字段保持调用方给的值(实盘rt块里是0)
	 */
	inline UpdateResult update(const WTSTickStruct& tick, uint32_t limitFlag, WTSSessionInfo* sInfo,
		const Options& opt, WTSBarStruct* lastBar, WTSBarStruct* newBar)
	{
		uint64_t secBarTime = bar_time(tick, sInfo);
		if (secBarTime == 0)
			return UR_DROP;

		bool isNewHigh = (limitFlag & LF_NEW_HIGH) != 0;
		bool isNewLow = (limitFlag & LF_NEW_LOW) != 0;
		bool tickNoTrade = decimal::eq(tick.turn_over, 0);

		if (lastBar == NULL || secBarTime > lastBar->time)
		{
			newBar->date = tick.trading_date;
			newBar->time = secBarTime;
			newBar->open = tick.price;
			newBar->high = isNewHigh ? tick.high : tick.price;
			newBar->low = isNewLow ? tick.low : tick.price;
			newBar->close = tick.price;

			newBar->vol = tick.volume;
			newBar->money = tick.turn_over;

			if (opt._min_price_mode == 1)
			{
				newBar->bid = tick.bid_prices[0];
				newBar->ask = tick.ask_prices[0];
			}
			else
			{
				newBar->hold = tick.open_interest;
				newBar->add = tick.diff_interest;
			}
			return UR_NEW;
		}

		//只有时间戳完全相同才累积，时间倒序的tick直接丢弃，不能回写已闭合的bar
		if (secBarTime != lastBar->time || (opt._skip_notrade_tick && tickNoTrade))
			return UR_DROP;

		/*
		 *	By Wesley @ 2023.07.05
		 *	某些品种开盘时可能推送price为0的tick，会导致open和low都是0
		 */
		if (decimal::eq(lastBar->open, 0))
			lastBar->open = tick.price;

		if (decimal::eq(lastBar->low, 0))
			lastBar->low = isNewLow ? tick.low : tick.price;
		else
			lastBar->low = isNewLow ? tick.low : std::min(tick.price, lastBar->low);

		lastBar->close = tick.price;
		lastBar->high = isNewHigh ? tick.high : std::max(tick.price, lastBar->high);

		lastBar->vol += tick.volume;
		lastBar->money += tick.turn_over;

		if (opt._min_price_mode == 1)
		{
			lastBar->bid = tick.bid_prices[0];
			lastBar->ask = tick.ask_prices[0];
		}
		else
		{
			lastBar->hold = tick.open_interest;
			lastBar->add += tick.diff_interest;
		}
		return UR_UPDATE;
	}

	/*
	 *	sec5 白名单匹配：合约全代码(SHFE.rb2610)或品种全代码(DCE.jm)命中任一即可
	 *	空白名单表示不限制
	 */
	template<typename SetType>
	inline bool code_match(const SetType& codes, const char* fullCode, const char* fullPid)
	{
		if (codes.empty())
			return true;
		return codes.find(fullCode) != codes.end() || codes.find(fullPid) != codes.end();
	}
}

NS_WTP_END
