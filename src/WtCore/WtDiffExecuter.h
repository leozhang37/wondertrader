/*!
 * \file WtExecuter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#pragma once

#include "ITrdNotifySink.h"
#include "IExecCommand.h"
#include "WtExecuterFactory.h"
#include "../Includes/ExecuteDefs.h"
#include "../Share/threadpool.hpp"
#include "../Share/SpinMutex.hpp"
#include "../Share/StdUtils.hpp"

#include <atomic>
#include <functional>
#include <vector>

NS_WTP_BEGIN
class WTSVariant;
class IDataManager;
class IBaseDataMgr;
class TraderAdapter;
class IHotMgr;
class WTSOrderInfo;

//本地执行器
class WtDiffExecuter : public ExecuteContext,
		public ITrdNotifySink, public IExecCommand
{
public:
	WtDiffExecuter(WtExecuterFactory* factory, const char* name, IDataManager* dataMgr, IBaseDataMgr* bdMgr);
	virtual ~WtDiffExecuter();

public:
	/*
	 *	初始化执行器
	 *	传入初始化参数
	 */
	bool init(WTSVariant* params);

	void setTrader(TraderAdapter* adapter);

private:
	ExecuteUnitPtr	getUnit(const char* code, bool bAutoCreate = true);

	void	save_data();
	void	load_data();

public:
	//////////////////////////////////////////////////////////////////////////
	//ExecuteContext
	virtual WTSTickSlice* getTicks(const char* code, uint32_t count, uint64_t etime = 0) override;

	virtual WTSTickData*	grabLastTick(const char* code) override;

	virtual double		getPosition(const char* stdCode, bool validOnly = true, int32_t flag = 3) override;
	virtual OrderMap*	getOrders(const char* code) override;
	virtual double		getUndoneQty(const char* code) override;

	virtual OrderIDs	buy(const char* code, double price, double qty, bool bForceClose = false) override;
	virtual OrderIDs	sell(const char* code, double price, double qty, bool bForceClose = false) override;
	virtual bool		cancel(uint32_t localid) override;
	virtual OrderIDs	cancel(const char* code, bool isBuy, double qty) override;
	virtual void		writeLog(const char* message) override;

	virtual WTSCommodityInfo*	getCommodityInfo(const char* stdCode) override;
	virtual WTSSessionInfo*		getSessionInfo(const char* stdCode) override;

	virtual uint64_t	getCurTime() override;

public:
	/*
	 *	设置目标仓位
	 */
	virtual void set_position(const wt_hashmap<std::string, double>& targets) override;


	/*
	 *	合约仓位变动
	 */
	virtual void on_position_changed(const char* stdCode, double diffPos) override;

	/*
	 *	实时行情回调
	 */
	virtual void on_tick(const char* stdCode, WTSTickData* newTick) override;

	/*
	 *	成交回报
	 */
	virtual void on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override;

	/*
	 *	订单回报
	 */
	virtual void on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled = false) override;

	/*
	 *	
	 */
	virtual void on_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday) override;

	/*
	 *	
	 */
	virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override;

	/*
	 *	交易通道就绪
	 */
	virtual void on_channel_ready() override;

	/*
	 *	交易通道丢失
	 */
	virtual void on_channel_lost() override;

	/*
	 *	资金回报
	 */
	virtual void on_account(const char* currency, double prebalance, double balance, double dynbalance,
		double avaliable, double closeprofit, double dynprofit, double margin, double fee, double deposit, double withdraw) override;


private:
	ExecuteUnitMap		_unit_map;
	TraderAdapter*		_trader;
	WtExecuterFactory*	_factory;
	IDataManager*		_data_mgr;
	IBaseDataMgr*		_bd_mgr;
	WTSVariant*			_config;

	double				_scale;				//放大倍数
	std::atomic<bool>	_channel_ready;

	/*
	 *	By 差量执行器线程安全 @ 2026.09.24
	 *	_unit_map/_target_pos/_diff_pos 会被引擎线程(set_position)、交易通道线程(on_channel_ready/on_trade)、
	 *	行情线程(on_tick)同时访问, 原来的锁被注释掉了, 启动时引擎推送初始目标仓位与通道就绪同时发生会导致哈希表并发读写而崩溃
	 *	_mtx_units 只保护 _unit_map 的查找和创建, 持有期间不回调执行单元
	 *	_mtx_pos 保护 _target_pos/_diff_pos 及其落地, 加锁顺序固定为 _mtx_pos -> _mtx_units
	 */
	SpinMutex			_mtx_units;
	StdRecurMutex		_mtx_pos;

	wt_hashmap<std::string, double> _target_pos;
	wt_hashmap<std::string, double> _diff_pos;

	//获取当前所有执行单元的快照, 用于在不持锁的情况下逐个回调
	std::vector<ExecuteUnitPtr> snapshot_units();

	//把差量推送给执行单元(按是否有线程池分派)
	void dispatch_diff(ExecuteUnitPtr unit, const std::string& stdCode, double diff);

	/*
	 *	By 差量执行器只管自己的仓位 @ 2026.09.24
	 *	同一个交易通道上的所有执行器都会收到全部 WT 订单的回报(localid 不为 0),
	 *	差量执行器只能根据自己发出的订单的成交更新差量, 撤单也只能撤自己的订单
	 *	_own_orders: 本执行器经 buy/sell 发出的订单号 -> 订单结束时间(毫秒, 0 表示尚未结束)
	 *	订单号随差量一起落地, 重启后仍能识别上次会话中自己的挂单(localid 按年内秒数播种, 跨重启不重复)
	 */
	wt_hashmap<uint32_t, uint64_t>	_own_orders;
	SpinMutex			_mtx_own;

	void	add_own_orders(const OrderIDs& ids);
	bool	is_own_order(uint32_t localid);

	//标准代码(如SHFE.rb.2610)对应的合约是否存在
	bool	is_valid_contract(const char* stdCode);
	void	finish_own_order(uint32_t localid);

	//遍历本执行器在指定合约上仍然存活的委托, 回调参数为 localid, 订单, 是否买入方向
	void	enum_own_alive_orders(const char* stdCode, std::function<void(uint32_t, WTSOrderInfo*, bool)> cb);

	typedef std::shared_ptr<boost::threadpool::pool> ThreadPoolPtr;
	ThreadPoolPtr		_pool;
};
NS_WTP_END
