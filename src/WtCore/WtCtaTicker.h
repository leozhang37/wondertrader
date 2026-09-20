/*!
 * \file WtCtaTicker.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#pragma once
#include <stdint.h>
#include <atomic>

#include "../Includes/WTSMarcos.h"
#include "../Share/StdUtils.hpp"

NS_WTP_BEGIN
class WTSSessionInfo;
class IDataReader;
class WTSTickData;

class WtCtaEngine;
//////////////////////////////////////////////////////////////////////////
//生产时间步进器
class WtCtaRtTicker
{
public:
	WtCtaRtTicker(WtCtaEngine* engine) 
		: _engine(engine)
		, _stopped(false)
		, _date(0)
		, _time(UINT_MAX)
		, _next_check_time(0)
		, _last_emit_pos(0)
		, _cur_pos(0)
		, _cur_sec_pos(0)
		, _last_emit_sec_pos(0)
		, _next_sec_check(0)
		, _sec_enabled(false){}
	~WtCtaRtTicker(){}

public:
	void	init(IDataReader* store, const char* sessionID);

	/*
	 *	打开秒级推进
	 *	By 秒K线支持 @ 2026.09.20
	 *	由引擎在发现有策略订阅了秒线时调用
	 */
	void	enable_seconds(bool bEnabled = true) { _sec_enabled = bEnabled; }
	bool	is_seconds_enabled() const { return _sec_enabled; }
	//void	set_time(uint32_t uDate, uint32_t uTime);
	void	on_tick(WTSTickData* curTick);

	void	run();
	void	stop();

	bool		is_in_trading() const;
	uint32_t	time_to_mins(uint32_t uTime) const;

private:
	void	trigger_price(WTSTickData* curTick);

	/*
	 *	检查并闭合秒线
	 *	@uDate/@uTime	闭合时刻，uTime为HHMMSS
	 *	@bFromTick		是否由tick驱动(否则是本地时钟兜底)
	 */
	void	check_sec_end(uint32_t uDate, uint32_t secPos, bool bEndingTDate);

private:
	WTSSessionInfo*	_s_info;
	WtCtaEngine*	_engine;
	IDataReader*	_store;

	uint32_t	_date;
	uint32_t	_time;

	uint32_t	_cur_pos;

	/*
	 *	秒线游标
	 *	By 秒K线支持 @ 2026.09.20
	 *
	 *	和分钟游标是两套独立的推进轴：
	 *	_cur_sec_pos 是当前tick所处的5秒bar编号(从开盘起算)，
	 *	_last_emit_sec_pos 是已经闭合过的编号。
	 *	闭合判定必须由时间驱动而不是tick驱动，否则5秒内没有tick的合约
	 *	会一直不闭合，on_bar和on_schedule都不触发
	 */
	uint32_t	_cur_sec_pos;
	std::atomic<uint32_t>	_last_emit_sec_pos;
	//本地时钟兜底：下一次该检查秒线闭合的时间点
	std::atomic<uint64_t>	_next_sec_check;
	//是否有策略订阅了秒线，没有就不做秒级推进，避免无谓开销
	bool		_sec_enabled;

	StdUniqueMutex	_mtx;
	std::atomic<uint64_t>	_next_check_time;
	std::atomic<uint32_t>	_last_emit_pos;

	bool			_stopped;
	StdThreadPtr	_thrd;

};
NS_WTP_END