/*!
 * \file WtCtaEngine.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#pragma once
#include "../Includes/ICtaStraCtx.h"
#include "../Share/threadpool.hpp"
#include "WtExecMgr.h"
#include "WtEngine.h"

NS_WTP_BEGIN
class WTSVariant;
typedef std::shared_ptr<ICtaStraCtx> CtaContextPtr;

class WtCtaRtTicker;

class WtCtaEngine : public WtEngine, public IExecuterStub
{
public:
	WtCtaEngine();
	virtual ~WtCtaEngine();

public:
	//////////////////////////////////////////////////////////////////////////
	//WtEngine接口
	virtual void handle_push_quote(WTSTickData* newTick) override;

	virtual void on_tick(const char* stdCode, WTSTickData* curTick) override;

	virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;

	virtual void on_init() override;
	virtual void on_session_begin() override;
	virtual void on_session_end() override;

	virtual void run() override;

	virtual void init(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtDtMgr* dataMgr, IHotMgr* hotMgr, EventNotifier* notifier) override;

	virtual bool isInTrading() override;
	virtual uint32_t transTimeToMin(uint32_t uTime) override;

	///////////////////////////////////////////////////////////////////////////
	//IExecuterStub 接口
	virtual uint64_t get_real_time() override;
	virtual WTSCommodityInfo* get_comm_info(const char* stdCode) override;
	virtual WTSSessionInfo* get_sess_info(const char* stdCode) override;
	virtual IHotMgr* get_hot_mon() { return _hot_mgr; }
	virtual uint32_t get_trading_day() { return _cur_tdate; }


public:
	void on_schedule(uint32_t curDate, uint32_t curTime);	

	void handle_pos_change(const char* straName, const char* stdCode, double diffPos);

	void addContext(CtaContextPtr ctx);
	
	CtaContextPtr	getContext(uint32_t id);

	inline void addExecuter(ExecCmdPtr executer)
	{
		_exec_mgr.add_executer(executer);
		executer->setStub(this);
	}

	inline bool loadRouterRules(WTSVariant* cfg)
	{
		return _exec_mgr.load_router_rules(cfg);
	}

public:
	void notify_chart_marker(uint64_t time, const char* straId, double price, const char* icon, const char* tag);
	void notify_chart_index(uint64_t time, const char* straId, const char* idxName, const char* lineName, double val);
	void notify_trade(const char* straId, const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, const char* userTag);

private:
	typedef wt_hashmap<uint32_t, CtaContextPtr> ContextMap;
	ContextMap		_ctx_map;

	WtCtaRtTicker*	_tm_ticker;

	/*
	 *	上次做"定期刷新"的本地时间(毫秒)
	 *	By 秒K线支持 @ 2026.09.20
	 *	on_schedule 末尾的 save_datas 和 refresh_funds 都是定期性质的，
	 *	秒线作主周期后调度频率涨12倍，这两件事都要节流：
	 *	- save_datas 是组合持仓/资金的全量JSON落盘，成交时另有调用点会存
	 *	- refresh_funds 会向每个交易通道发查询资金请求，而 queryFund
	 *	  直接调 _trader_api->queryAccount() 没有任何流控保护，
	 *	  每5秒一次纯属浪费交易通道带宽，多账户时还有触发流控的风险
	 */
	uint64_t		_last_periodic_time;

	WtExecuterMgr	_exec_mgr;

	WTSVariant*		_cfg;

	typedef std::shared_ptr<boost::threadpool::pool> ThreadPoolPtr;
	ThreadPoolPtr		_pool;
};

NS_WTP_END

