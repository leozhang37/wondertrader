/*!
 * \file mock_datasink.hpp
 * \brief WtDataStorage 单测用的最小mock
 *
 * WtDataWriter/WtDataReader 本身是普通C++类（createDataReader 只是导出工厂），
 * 只要把 IDataWriterSink / IBaseDataMgr / IDataReaderSink 这几个回调接口填上，
 * 就能在gtest里直接实例化，不必起整个QuoteFactory
 */
#pragma once
#include <vector>
#include <string>

#include "../Includes/IDataWriter.h"
#include "../Includes/IDataReader.h"
#include "../Includes/IRdmDtReader.h"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSCollection.hpp"

USING_NS_WTP;

/*
 *	只持有一个品种+一个合约+一个交易时段的基础数据管理器
 */
class MockBaseDataMgr : public IBaseDataMgr
{
public:
	MockBaseDataMgr(const char* exchg, const char* pid, const char* code, WTSSessionInfo* sInfo, uint32_t tdate = 0)
		: _sinfo(sInfo), _tdate(tdate)
	{
		_commodity = WTSCommodityInfo::create(pid, pid, exchg, sInfo->id(), "CHINA");
		_commodity->setSessionInfo(sInfo);
		//和 WTSBaseDataMgr 一样把合约登记到品种上，WtDataWriter::transHisData 靠它枚举收盘作业的合约
		_commodity->addCode(code);

		_contract = WTSContractInfo::create(code, code, exchg, pid);
		_contract->setCommInfo(_commodity);

		_exchg = exchg;
		_pid = pid;
		_code = code;
		_full_pid = _exchg + "." + _pid;
	}

	~MockBaseDataMgr()
	{
		if (_contract) _contract->release();
		if (_commodity) _commodity->release();
	}

	WTSContractInfo* contract() { return _contract; }

public:
	virtual WTSCommodityInfo* getCommodity(const char* exchgpid) override { return _commodity; }
	virtual WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override { return _commodity; }

	/*
	 *	只认自己的合约代码。原先是无条件返回，writer 收盘时处理队列里的 "MARK.<sid>" 标记项
	 *	也会拿到合约，把tick按代码 "<sid>" 落成 his/ticks/.../<sid>.dsb
	 */
	virtual WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t uDate = 0) override
	{
		if (_code != code)
			return NULL;
		if (exchg != NULL && strlen(exchg) > 0 && _exchg != exchg)
			return NULL;
		return _contract;
	}
	virtual WTSArray* getContracts(const char* exchg = "", uint32_t uDate = 0) override { return NULL; }

	virtual WTSSessionInfo* getSession(const char* sid) override { return _sinfo; }
	virtual WTSSessionInfo* getSessionByCode(const char* code, const char* exchg = "") override { return _sinfo; }
	virtual WTSArray* getAllSessions() override { return NULL; }

	virtual bool isHoliday(const char* pid, uint32_t uDate, bool isTpl = false) override { return false; }

	/*
	 *	uDate为0是在问"当前交易日"，必须返回构造时给的_tdate。
	 *	readKlineSlice 靠 calcTradingDate(pid,0,0) 和 calcTradingDate(pid,curDate,curTime)
	 *	是否相等来判断有没有当日实时数据(bHasToday)，
	 *	如果这里对0也原样返回0，bHasToday永远为false，rt块路径就走不到
	 */
	virtual uint32_t calcTradingDate(const char* stdPID, uint32_t uDate, uint32_t uTime, bool isSession = false) override
	{
		return (uDate == 0) ? _tdate : uDate;
	}
	virtual uint64_t getBoundaryTime(const char* stdPID, uint32_t tDate, bool isSession = false, bool isStart = true) override
	{
		return (uint64_t)tDate * 10000 + (isStart ? _sinfo->getOpenTime() : _sinfo->getCloseTime());
	}

	virtual uint32_t getContractSize(const char* exchg = "", uint32_t uDate = 0) override { return 1; }
	virtual uint32_t getGlobalSize() override { return 1; }
	virtual WTSContractInfo* getContractByIndex(uint32_t idx) override { return _contract; }

private:
	WTSSessionInfo*		_sinfo;
	uint32_t			_tdate;		//当前交易日
	WTSCommodityInfo*	_commodity;
	WTSContractInfo*	_contract;
	std::string			_exchg, _pid, _code, _full_pid;
};

/*
 *	落地模块的回调桩
 */
class MockWriterSink : public IDataWriterSink
{
public:
	MockWriterSink(MockBaseDataMgr* bd, uint32_t tdate) : _bd(bd), _tdate(tdate), _verbose(false) {}

	void set_verbose(bool b) { _verbose = b; }

	virtual IBaseDataMgr* getBDMgr() override { return _bd; }
	virtual bool canSessionReceive(const char* sid) override { return true; }

	virtual void broadcastTick(WTSTickData* curTick) override {}
	virtual void broadcastOrdQue(WTSOrdQueData* curOrdQue) override {}
	virtual void broadcastOrdDtl(WTSOrdDtlData* curOrdDtl) override {}
	virtual void broadcastTrans(WTSTransData* curTrans) override {}

	virtual CodeSet* getSessionComms(const char* sid) override
	{
		_comms.clear();
		_comms.insert(_bd->contract()->getFullPid());
		return &_comms;
	}

	virtual uint32_t getTradingDate(const char* pid) override { return _tdate; }

	virtual void outputLog(WTSLogLevel ll, const char* message) override
	{
		_logs.emplace_back(message);
		if (_verbose)
			printf("[writer] %s\n", message);
	}

	const std::vector<std::string>& logs() const { return _logs; }

private:
	MockBaseDataMgr*	_bd;
	uint32_t			_tdate;
	CodeSet				_comms;
	std::vector<std::string> _logs;
	bool				_verbose;
};

/*
 *	读取模块的回调桩
 *	on_bar 的调用序列会被记录下来，用于验证闭合事件的次数与顺序
 */
class MockReaderSink : public IDataReaderSink
{
public:
	MockReaderSink(MockBaseDataMgr* bd)
		: _bd(bd), _date(0), _min_time(0), _secs(0), _verbose(false) {}

	//设置"当前时刻"，min_time为HHMM，secs为毫秒数(如 3000 表示第3秒)
	void set_time(uint32_t date, uint32_t min_time, uint32_t secs = 0)
	{
		_date = date; _min_time = min_time; _secs = secs;
	}
	void set_verbose(bool b) { _verbose = b; }

	struct BarEvent
	{
		std::string		_code;
		WTSKlinePeriod	_period;
		uint32_t		_date;
		uint64_t		_time;
		double			_close;
	};

	const std::vector<BarEvent>& bars() const { return _bars; }
	const std::vector<uint32_t>& updates() const { return _updates; }
	void clear() { _bars.clear(); _updates.clear(); }

public:
	virtual void on_bar(const char* stdCode, WTSKlinePeriod period, WTSBarStruct* newBar) override
	{
		BarEvent e;
		e._code = stdCode;
		e._period = period;
		e._date = newBar->date;
		e._time = newBar->time;
		e._close = newBar->close;
		_bars.emplace_back(e);

		if (_verbose)
			printf("[reader] on_bar %s period=%u time=%llu close=%.2f\n",
				stdCode, (uint32_t)period, (unsigned long long)newBar->time, newBar->close);
	}

	virtual void on_all_bar_updated(uint32_t updateTime) override
	{
		_updates.emplace_back(updateTime);
	}

	virtual IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	virtual IHotMgr* get_hot_mgr() override { return NULL; }

	virtual uint32_t get_date() override { return _date; }
	virtual uint32_t get_min_time() override { return _min_time; }
	virtual uint32_t get_secs() override { return _secs; }

	virtual void reader_log(WTSLogLevel ll, const char* message) override
	{
		_logs.emplace_back(message);
		if (_verbose)
			printf("[reader] %s\n", message);
	}

	const std::vector<std::string>& logs() const { return _logs; }

	//日志里是否出现过某个片段
	bool log_contains(const char* frag) const
	{
		for (const std::string& l : _logs)
		{
			if (l.find(frag) != std::string::npos)
				return true;
		}
		return false;
	}

private:
	MockBaseDataMgr*		_bd;
	uint32_t				_date;
	uint32_t				_min_time;
	uint32_t				_secs;
	bool					_verbose;
	std::vector<BarEvent>	_bars;
	std::vector<uint32_t>	_updates;
	std::vector<std::string> _logs;
};

/*
 *	随机读取模块(DtServo查询用)的回调桩
 *	接口只有三个方法，比 IDataReaderSink 更简单：没有时间来源，
 *	因为查询接口的时间范围是调用方直接给的
 */
class MockRdmSink : public IRdmDtReaderSink
{
public:
	MockRdmSink(MockBaseDataMgr* bd) : _bd(bd), _verbose(false) {}

	void set_verbose(bool b) { _verbose = b; }

	virtual IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	virtual IHotMgr* get_hot_mgr() override { return NULL; }

	virtual void reader_log(WTSLogLevel ll, const char* message) override
	{
		_logs.emplace_back(message);
		if (_verbose)
			printf("[rdm] %s\n", message);
	}

	const std::vector<std::string>& logs() const { return _logs; }

private:
	MockBaseDataMgr*	_bd;
	bool				_verbose;
	std::vector<std::string> _logs;
};
