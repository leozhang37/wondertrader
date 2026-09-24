/*!
 * \file WtExecuter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "WtDiffExecuter.h"
#include "TraderAdapter.h"
#include "WtEngine.h"
#include "WtHelper.h"

#include "../Share/CodeHelper.hpp"
#include "../Includes/IDataManager.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/IHotMgr.h"
#include "../Includes/IBaseDataMgr.h"
#include "../Share/decimal.h"

#include <tuple>

#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSContractInfo.hpp"

#include "../WTSTools/WTSLogger.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
namespace rj = rapidjson;

USING_NS_WTP;


WtDiffExecuter::WtDiffExecuter(WtExecuterFactory* factory, const char* name, IDataManager* dataMgr, IBaseDataMgr* bdMgr)
	: IExecCommand(name)
	, _factory(factory)
	, _data_mgr(dataMgr)
	, _channel_ready(false)
	, _scale(1.0)
	, _trader(NULL)
	, _bd_mgr(bdMgr)
{
}


WtDiffExecuter::~WtDiffExecuter()
{
	if (_pool)
		_pool->wait();
}

void WtDiffExecuter::setTrader(TraderAdapter* adapter)
{
	_trader = adapter;
	//设置的时候读取一下trader的状态
	if(_trader)
		_channel_ready = _trader->isReady();
}

bool WtDiffExecuter::init(WTSVariant* params)
{
	if (params == NULL)
		return false;

	_config = params;
	_config->retain();

	_scale = params->getDouble("scale");

	uint32_t poolsize = params->getUInt32("poolsize");
	if(poolsize > 0)
	{
		_pool.reset(new boost::threadpool::pool(poolsize));
	}

	load_data();

	WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Diff executer inited, scale: {}, thread poolsize: {}", _name, _scale, poolsize);

	return true;
}

void WtDiffExecuter::load_data()
{
	//读取执行器的理论部位，以及待执行的差量
	std::string filename = WtHelper::getExecDataDir();
	filename += _name + ".json";

	if (!StdFile::exists(filename.c_str()))
	{
		return;
	}

	std::string content;
	StdFile::read_file_content(filename.c_str(), content);
	if (content.empty())
		return;

	rj::Document root;
	root.Parse(content.c_str());

	if (root.HasParseError())
		return;

	if(root.HasMember("targets"))
	{
		const rj::Value& jTargets = root["targets"];
		for (const rj::Value& jItem : jTargets.GetArray())
		{
			const char* stdCode = jItem["code"].GetString();
			CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, NULL);
			WTSContractInfo* ct = _bd_mgr->getContract(cInfo._code, cInfo._exchg);
			if (ct == NULL)
			{
				WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Ticker {} is not valid", _name, stdCode);
				continue;
			}

			double pos = jItem["target"].GetDouble();
			_target_pos[stdCode] = pos;
		}
	}

	if (root.HasMember("diffs"))
	{
		const rj::Value& jDiffs = root["diffs"];
		for (const rj::Value& jItem : jDiffs.GetArray())
		{
			const char* stdCode = jItem["code"].GetString();
			CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, NULL);
			WTSContractInfo* ct = _bd_mgr->getContract(cInfo._code, cInfo._exchg);
			if (ct == NULL)
			{
				WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Ticker {} is not valid", _name, stdCode);
				continue;
			}

			double pos = jItem["diff"].GetDouble();
			_diff_pos[stdCode] = pos;
		}
	}

	//By 差量执行器只管自己的仓位 @ 2026.09.24
	//恢复上次会话中自己发出的订单号, 重启后查询到的挂单/成交据此判断是否属于本执行器
	if (root.HasMember("orders"))
	{
		const rj::Value& jOrders = root["orders"];
		for (const rj::Value& jItem : jOrders.GetArray())
			_own_orders[jItem.GetUint()] = 0;
	}
}

void WtDiffExecuter::save_data()
{
	StdLocker<StdRecurMutex> lock(_mtx_pos);

	std::string filename = WtHelper::getExecDataDir();
	filename += _name + ".json";

	rj::Document root(rj::kObjectType);
	rj::Document::AllocatorType &allocator = root.GetAllocator();

	{//目标持仓数据保存
		rj::Value jTarget(rj::kArrayType);

		for (auto& v : _target_pos)
		{
			rj::Value jItem(rj::kObjectType);
			jItem.AddMember("code", rj::Value(v.first.c_str(), allocator), allocator);
			jItem.AddMember("target", v.second, allocator);

			jTarget.PushBack(jItem, allocator);
		}

		root.AddMember("targets", jTarget, allocator);
	}

	{//差量持仓数据保存
		rj::Value jDiff(rj::kArrayType);

		for (auto& v : _diff_pos)
		{
			rj::Value jItem(rj::kObjectType);
			jItem.AddMember("code", rj::Value(v.first.c_str(), allocator), allocator);
			jItem.AddMember("diff", v.second, allocator);

			jDiff.PushBack(jItem, allocator);
		}

		root.AddMember("diffs", jDiff, allocator);
	}

	{//自己发出的订单号保存
	 //已结束超过60秒的订单不再需要(成交回报早已到达), 顺便从内存中清理掉
		const uint64_t KEEP_MS = 60 * 1000;
		uint64_t now = TimeUtils::getLocalTimeNow();
		rj::Value jOrders(rj::kArrayType);

		SpinLock lock(_mtx_own);
		for (auto it = _own_orders.begin(); it != _own_orders.end();)
		{
			if (it->second != 0 && now - it->second > KEEP_MS)
			{
				it = _own_orders.erase(it);
				continue;
			}

			jOrders.PushBack(it->first, allocator);
			it++;
		}

		root.AddMember("orders", jOrders, allocator);
	}

	{
		std::string filename = WtHelper::getExecDataDir();
		filename += _name + ".json";

		BoostFile bf;
		if (bf.create_new_file(filename.c_str()))
		{
			rj::StringBuffer sb;
			rj::PrettyWriter<rj::StringBuffer> writer(sb);
			root.Accept(writer);
			bf.write_file(sb.GetString());
			bf.close_file();
		}
	}
}

ExecuteUnitPtr WtDiffExecuter::getUnit(const char* stdCode, bool bAutoCreate /* = true */)
{
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, NULL);
	std::string commID = codeInfo.stdCommID();

	WTSVariant* policy = _config->get("policy");
	std::string des = commID;
	if (!policy->has(commID.c_str()))
		des = "default";

	ExecuteUnitPtr unit;
	{
		SpinLock lock(_mtx_units);

		auto it = _unit_map.find(stdCode);
		if(it != _unit_map.end())
		{
			return it->second;
		}

		if (!bAutoCreate)
			return ExecuteUnitPtr();

		WTSVariant* cfg = policy->get(des.c_str());
		if (cfg == NULL)
		{
			WTSLogger::error("No execute unit policy for {} configured in executer {}", stdCode, _name);
			return ExecuteUnitPtr();
		}

		const char* name = cfg->getCString("name");
		unit = _factory->createDiffExeUnit(name);
		if (unit == NULL)
		{
			WTSLogger::error("Creating ExecUnit {} failed", name);
			return unit;
		}

		//先初始化再放入, 避免其他线程拿到未初始化的执行单元
		unit->self()->init(this, stdCode, cfg);
		_unit_map[stdCode] = unit;
	}

	//如果通道已经就绪，则直接通知执行单元(不持锁回调)
	if (_channel_ready)
		unit->self()->on_channel_ready();

	return unit;
}

std::vector<ExecuteUnitPtr> WtDiffExecuter::snapshot_units()
{
	std::vector<ExecuteUnitPtr> ret;
	SpinLock lock(_mtx_units);
	ret.reserve(_unit_map.size());
	for (auto it = _unit_map.begin(); it != _unit_map.end(); it++)
	{
		if (it->second)
			ret.emplace_back(it->second);
	}
	return ret;
}

void WtDiffExecuter::dispatch_diff(ExecuteUnitPtr unit, const std::string& stdCode, double diff)
{
	if (_pool)
	{
		_pool->schedule([unit, stdCode, diff]() {
			unit->self()->set_position(stdCode.c_str(), diff);
		});
	}
	else
	{
		unit->self()->set_position(stdCode.c_str(), diff);
	}
}


//////////////////////////////////////////////////////////////////////////
//ExecuteContext
#pragma region Context回调接口
WTSTickSlice* WtDiffExecuter::getTicks(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	if (_data_mgr == NULL)
		return NULL;

	return _data_mgr->get_tick_slice(stdCode, count);
}

WTSTickData* WtDiffExecuter::grabLastTick(const char* stdCode)
{
	if (_data_mgr == NULL)
		return NULL;

	return _data_mgr->grab_last_tick(stdCode);
}

double WtDiffExecuter::getPosition(const char* stdCode, bool validOnly /* = true */, int32_t flag /* = 3 */)
{
	if (NULL == _trader)
		return 0.0;

	return _trader->getPosition(stdCode, validOnly, flag);
}

double WtDiffExecuter::getUndoneQty(const char* stdCode)
{
	if (NULL == _trader)
		return 0.0;

	return _trader->getUndoneQty(stdCode);
}

OrderMap* WtDiffExecuter::getOrders(const char* stdCode)
{
	if (NULL == _trader)
		return NULL;

	//By 差量执行器只管自己的仓位 @ 2026.09.24
	//只返回本执行器自己的存活委托
	OrderMap* ret = OrderMap::create();
	enum_own_alive_orders(stdCode, [ret](uint32_t localid, WTSOrderInfo* ordInfo, bool) {
		ret->add(localid, ordInfo);
	});
	return ret;
}

OrderIDs WtDiffExecuter::buy(const char* stdCode, double price, double qty, bool bForceClose/* = false*/)
{
	if (!_channel_ready)
		return OrderIDs();

	OrderIDs ids = _trader->buy(stdCode, price, qty, 0, bForceClose);
	add_own_orders(ids);
	return ids;
}

OrderIDs WtDiffExecuter::sell(const char* stdCode, double price, double qty, bool bForceClose/* = false*/)
{
	if (!_channel_ready)
		return OrderIDs();

	OrderIDs ids = _trader->sell(stdCode, price, qty, 0, bForceClose);
	add_own_orders(ids);
	return ids;
}

bool WtDiffExecuter::cancel(uint32_t localid)
{
	if (!_channel_ready)
		return false;

	//By 差量执行器只管自己的仓位 @ 2026.09.24
	if (!is_own_order(localid))
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_WARN, "[{}] Order {} is not placed by this executer, cancel ignored", _name, localid);
		return false;
	}

	return _trader->cancel(localid);
}

OrderIDs WtDiffExecuter::cancel(const char* stdCode, bool isBuy, double qty)
{
	if (!_channel_ready)
		return OrderIDs();

	/*
	 *	By 差量执行器只管自己的仓位 @ 2026.09.24
	 *	原来直接调用 _trader->cancel(stdCode, isBuy, qty), 会把合约上所有同方向的挂单(包括手工单、其他执行器的单)都撤掉
	 *	这里只撤本执行器自己的挂单
	 */
	std::vector<uint32_t> toCancel;
	double actQty = 0;
	enum_own_alive_orders(stdCode, [&](uint32_t localid, WTSOrderInfo* ordInfo, bool bBuy) {
		if (bBuy != isBuy)
			return;

		if (qty > 0 && decimal::ge(actQty, qty))
			return;

		toCancel.emplace_back(localid);
		actQty += ordInfo->getVolLeft();
	});

	OrderIDs ret;
	for (uint32_t localid : toCancel)
	{
		if (_trader->cancel(localid))
			ret.emplace_back(localid);
	}
	return ret;
}

void WtDiffExecuter::add_own_orders(const OrderIDs& ids)
{
	if (ids.empty())
		return;

	{
		SpinLock lock(_mtx_own);
		for (uint32_t localid : ids)
			_own_orders[localid] = 0;
	}

	//订单号要立即落地, 否则发单后异常退出, 重启后就识别不出这些挂单
	save_data();
}

bool WtDiffExecuter::is_valid_contract(const char* stdCode)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, NULL);
	return _bd_mgr->getContract(cInfo._code, cInfo._exchg) != NULL;
}

bool WtDiffExecuter::is_own_order(uint32_t localid)
{
	if (localid == 0)
		return false;

	SpinLock lock(_mtx_own);
	return _own_orders.find(localid) != _own_orders.end();
}

void WtDiffExecuter::finish_own_order(uint32_t localid)
{
	SpinLock lock(_mtx_own);
	auto it = _own_orders.find(localid);
	if (it != _own_orders.end() && it->second == 0)
		it->second = TimeUtils::getLocalTimeNow();
}

void WtDiffExecuter::enum_own_alive_orders(const char* stdCode, std::function<void(uint32_t, WTSOrderInfo*, bool)> cb)
{
	if (NULL == _trader)
		return;

	//TraderAdapter::getOrders 按原始合约代码比较, 传标准代码匹配不上, 所以取全部订单后自己按标准代码过滤
	OrderMap* orders = _trader->getOrders("");
	if (orders == NULL)
		return;

	for (auto it = orders->begin(); it != orders->end(); it++)
	{
		uint32_t localid = it->first;
		WTSOrderInfo* ordInfo = (WTSOrderInfo*)it->second;
		if (ordInfo == NULL || !ordInfo->isAlive() || !is_own_order(localid))
			continue;

		std::string code;
		WTSContractInfo* cInfo = ordInfo->getContractInfo();
		if (cInfo != NULL)
		{
			WTSCommodityInfo* commInfo = cInfo->getCommInfo();
			if (commInfo->getCategoty() == CC_FutOption || commInfo->getCategoty() == CC_SpotOption)
				code = CodeHelper::rawFutOptCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
			else if (CodeHelper::isMonthlyCode(cInfo->getCode()))
				code = CodeHelper::rawMonthCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
			else
				code = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), cInfo->getProduct());
		}
		else
		{
			code = CodeHelper::rawMonthCodeToStdCode(ordInfo->getCode(), ordInfo->getExchg());
		}

		if (strlen(stdCode) != 0 && code.compare(stdCode) != 0)
			continue;

		bool isBuy = (ordInfo->getDirection() == WDT_LONG && ordInfo->getOffsetType() == WOT_OPEN) || (ordInfo->getDirection() == WDT_SHORT && ordInfo->getOffsetType() != WOT_OPEN);
		cb(localid, ordInfo, isBuy);
	}

	orders->release();
}

void WtDiffExecuter::writeLog(const char* message)
{
	static thread_local char szBuf[2048] = { 0 };
	fmtutil::format_to(szBuf, "[{}] {}", _name.c_str(), message);
	WTSLogger::log_dyn_raw("executer", _name.c_str(), LL_INFO, szBuf);
}

WTSCommodityInfo* WtDiffExecuter::getCommodityInfo(const char* stdCode)
{
	return _stub->get_comm_info(stdCode);
}

WTSSessionInfo* WtDiffExecuter::getSessionInfo(const char* stdCode)
{
	return _stub->get_sess_info(stdCode);
}

uint64_t WtDiffExecuter::getCurTime()
{
	return _stub->get_real_time();
	//return TimeUtils::makeTime(_stub->get_date(), _stub->get_raw_time() * 100000 + _stub->get_secs());
}

#pragma endregion Context回调接口
//ExecuteContext
//////////////////////////////////////////////////////////////////////////


#pragma region 外部接口
void WtDiffExecuter::on_position_changed(const char* stdCode, double diffPos)
{
	ExecuteUnitPtr unit = getUnit(stdCode, true);
	if (unit == NULL)
		return;

	//如果差量为0，则直接返回
	if (decimal::eq(diffPos, 0))
		return;

	diffPos = round(diffPos*_scale);

	double newDiff = 0;
	{
		StdLocker<StdRecurMutex> lock(_mtx_pos);
		double oldVol = _target_pos[stdCode];
		double& targetPos = _target_pos[stdCode];
		targetPos += diffPos;

		/*
		 *	By Sunseeeeeker @ 2023.01.10
		 *	更新差量
		*/
		double& thisDiff = _diff_pos[stdCode];
		double prevDiff = thisDiff;
		thisDiff += diffPos;
		newDiff = thisDiff;

		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Target position of {} changed additonally: {} -> {}, diff postion changed: {} -> {}", _name, stdCode, oldVol, targetPos, prevDiff, thisDiff);
	}

	if (_trader && !_trader->checkOrderLimits(stdCode))
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] {} is disabled", _name, stdCode);
		return;
	}

	//TODO 差量执行还要再看一下
	dispatch_diff(unit, stdCode, newDiff);
}

void WtDiffExecuter::set_position(const wt_hashmap<std::string, double>& targets)
{
	/*
	 *	By 差量执行器线程安全 @ 2026.09.24
	 *	getUnit 创建执行单元时会回调执行单元(on_channel_ready -> do_calc, 持有执行单元的计算锁),
	 *	而执行单元下单后会回到执行器落地订单号(需要 _mtx_pos), 所以不能在持有 _mtx_pos 时调用 getUnit,
	 *	否则两个线程分别按 _mtx_pos->计算锁 和 计算锁->_mtx_pos 的顺序加锁会死锁
	 *	先在锁外准备好所有需要的执行单元, 再在锁内更新目标仓位和差量, 最后在锁外推送差量
	 */
	wt_hashmap<std::string, ExecuteUnitPtr> units;
	for (auto it = targets.begin(); it != targets.end(); it++)
	{
		ExecuteUnitPtr unit = getUnit(it->first.c_str());
		if (unit != NULL)
			units[it->first] = unit;
	}

	std::vector<std::string> dropped;
	{
		StdLocker<StdRecurMutex> lock(_mtx_pos);
		for (auto it = _target_pos.begin(); it != _target_pos.end(); it++)
		{
			if (targets.find(it->first) == targets.end() && it->second != 0)
				dropped.emplace_back(it->first);
		}
	}

	for (const std::string& stdCode : dropped)
	{
		if (!is_valid_contract(stdCode.c_str()))
			continue;

		ExecuteUnitPtr unit = getUnit(stdCode.c_str());
		if (unit != NULL)
			units[stdCode] = unit;
	}

	std::vector<std::tuple<ExecuteUnitPtr, std::string, double>> dispatches;
	{
		StdLocker<StdRecurMutex> lock(_mtx_pos);

		for (auto it = targets.begin(); it != targets.end(); it++)
		{
			const char* stdCode = it->first.c_str();
			double newVol = it->second;
			auto uit = units.find(it->first);
			if (uit == units.end())
				continue;

			ExecuteUnitPtr unit = uit->second;

			newVol = round(newVol*_scale);
			double oldVol = _target_pos[stdCode];
			_target_pos[stdCode] = newVol;
			if (decimal::eq(oldVol, newVol))
				continue;

			//差量更新
			double& thisDiff = _diff_pos[stdCode];
			double prevDiff = thisDiff;
			thisDiff += (newVol - oldVol);

			WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Target position of {} changed: {} -> {}, diff postion changed: {} -> {}", _name, stdCode, oldVol, newVol, prevDiff, thisDiff);

			if (_trader && !_trader->checkOrderLimits(stdCode))
			{
				WTSLogger::log_dyn("executer", _name.c_str(), LL_WARN, "[{}] {} is disabled due to entrust limit control ", _name, stdCode);
				continue;
			}

			//TODO 差量执行还要再看一下
			dispatches.emplace_back(unit, it->first, thisDiff);
		}

		//在原来的目标头寸中，但是不在新的目标头寸中，则需要自动设置为0
		for (auto it = _target_pos.begin(); it != _target_pos.end(); it++)
		{
			const char* stdCode = it->first.c_str();
			double& pos = (double&)it->second;
			auto tit = targets.find(stdCode);
			if(tit != targets.end())
				continue;

			/*
			 *	By 差量执行器只管自己的仓位 @ 2026.09.24
			 *	原来是 _bd_mgr->getContract(stdCode), 基础数据按原始代码(如rb2610)索引, 传标准代码(如SHFE.rb.2610)永远查不到,
			 *	导致这个分支从未执行: 不在目标中的合约, 自己的仓位一直不会被平掉
			 */
			if (!is_valid_contract(stdCode))
				continue;

			if(pos != 0)
			{
				WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] {} is not in target, set to 0 automatically", _name, stdCode);

				auto uit = units.find(it->first);
				if (uit == units.end())
					continue;

				//更新差量
				double& thisDiff = _diff_pos[stdCode];
				double prevDiff = thisDiff;

				/*
				 *	By 差量执行器线程安全 @ 2026.09.24
				 *	目标仓位从 pos 变为 0, 差量应变化 0 - pos, 与上面目标仓位显式设为 0 时的 thisDiff += (newVol - oldVol) 一致
				 *	原来写成 thisDiff -= -pos, 实际是加上 pos, 方向相反, 会在自己原有仓位的基础上再开同方向同数量的仓
				 */
				thisDiff -= pos;
				pos = 0;

				WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Diff of {} changed: {} -> {} as target reset to 0", _name, stdCode, prevDiff, thisDiff);

				dispatches.emplace_back(uit->second, it->first, thisDiff);
			}
		}

		save_data();
	}

	for (auto& item : dispatches)
		dispatch_diff(std::get<0>(item), std::get<1>(item), std::get<2>(item));
}

void WtDiffExecuter::on_tick(const char* stdCode, WTSTickData* newTick)
{
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (unit == NULL)
		return;

	//unit->self()->on_tick(newTick);
	if (_pool)
	{
		newTick->retain();
		_pool->schedule([unit, newTick](){
			unit->self()->on_tick(newTick);
			newTick->release();
		});
	}
	else
	{
		unit->self()->on_tick(newTick);
	}
}

void WtDiffExecuter::on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price)
{
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (unit == NULL)
		return;

	/*
	 *	By 差量执行器只管自己的仓位 @ 2026.09.24
	 *	原来只判断 localid 不为 0, 同一交易通道上其他执行器的成交也会被扣减到本执行器的差量上
	 *	只有本执行器自己发出的订单的成交才更新差量
	 */
	if (!is_own_order(localid))
		return;

	//自己订单的成交, 更新差量
	{
		StdLocker<StdRecurMutex> lock(_mtx_pos);
		double& curDiff = _diff_pos[stdCode];
		double prevDiff = curDiff;
		curDiff -= vol * (isBuy ? 1 : -1);

		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Diff of {} updated by trade: {} -> {}", _name, stdCode, prevDiff, curDiff);
		save_data();
	}

	if (_pool)
	{
		std::string code = stdCode;
		_pool->schedule([localid, unit, code, isBuy, vol, price]() {
			unit->self()->on_trade(localid, code.c_str(), isBuy, vol, price);
		});
	}
	else
	{
		unit->self()->on_trade(localid, stdCode, isBuy, vol, price);
		}
}

void WtDiffExecuter::on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled /* = false */)
{
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (unit == NULL)
		return;

	//By 差量执行器只管自己的仓位 @ 2026.09.24
	if (!is_own_order(localid))
		return;

	if (isCanceled || decimal::eq(leftQty, 0))
		finish_own_order(localid);

	if (_pool)
	{
		std::string code = stdCode;
		_pool->schedule([localid, unit, code, isBuy, leftQty, price, isCanceled](){
			unit->self()->on_order(localid, code.c_str(), isBuy, leftQty, price, isCanceled);
		});
	}
	else
	{
		unit->self()->on_order(localid, stdCode, isBuy, leftQty, price, isCanceled);
	}
}

void WtDiffExecuter::on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message)
{
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (unit == NULL)
		return;

	//By 差量执行器只管自己的仓位 @ 2026.09.24
	if (!is_own_order(localid))
		return;

	if (!bSuccess)
		finish_own_order(localid);

	if (_pool)
	{
		std::string code = stdCode;
		std::string msg = message;
		_pool->schedule([unit, localid, code, bSuccess, msg](){
			unit->self()->on_entrust(localid, code.c_str(), bSuccess, msg.c_str());
		});
	}
	else
	{
		unit->self()->on_entrust(localid, stdCode, bSuccess, message);
	}
}

void WtDiffExecuter::on_channel_ready()
{
	_channel_ready = true;
	for (ExecuteUnitPtr& unitPtr : snapshot_units())
	{
		if (_pool)
		{
			_pool->schedule([unitPtr](){
				unitPtr->self()->on_channel_ready();
			});
		}
		else
		{
			unitPtr->self()->on_channel_ready();
		}
	}

	//先复制一份差量再推送, 避免遍历 _diff_pos 时被其他线程修改
	std::vector<std::pair<std::string, double>> diffs;
	{
		StdLocker<StdRecurMutex> lock(_mtx_pos);
		diffs.assign(_diff_pos.begin(), _diff_pos.end());
	}

	for(auto& v : diffs)
	{
		const std::string& stdCode = v.first;
		ExecuteUnitPtr unit = getUnit(stdCode.c_str());
		if (unit == NULL)
			continue;

		dispatch_diff(unit, stdCode, v.second);

		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Diff of {} recovered to {}", _name, stdCode, v.second);
	}
}

void WtDiffExecuter::on_channel_lost()
{
	_channel_ready = false;
	for (ExecuteUnitPtr& unitPtr : snapshot_units())
	{
		if (_pool)
		{
			_pool->schedule([unitPtr](){
				unitPtr->self()->on_channel_lost();
			});
		}
		else
		{
			unitPtr->self()->on_channel_lost();
		}
	}
}

void WtDiffExecuter::on_account(const char* currency, double prebalance, double balance, double dynbalance,
	double avaliable, double closeprofit, double dynprofit, double margin, double fee, double deposit, double withdraw)
{
	for (ExecuteUnitPtr& unitPtr : snapshot_units())
	{
		if (_pool)
		{
			std::string strCur = currency;
			_pool->schedule([unitPtr, strCur, prebalance, balance, dynbalance, avaliable, closeprofit, dynprofit, margin, fee, deposit, withdraw]() {
				unitPtr->self()->on_account(strCur.c_str(), prebalance, balance, dynbalance, avaliable, closeprofit, dynprofit, margin, fee, deposit, withdraw);
			});
		}
		else
		{
			unitPtr->self()->on_account(currency, prebalance, balance, dynbalance, avaliable, closeprofit, dynprofit, margin, fee, deposit, withdraw);
		}
	}
}

void WtDiffExecuter::on_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday)
{

}

#pragma endregion 外部接口