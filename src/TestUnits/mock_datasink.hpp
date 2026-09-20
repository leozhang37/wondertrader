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
	MockBaseDataMgr(const char* exchg, const char* pid, const char* code, WTSSessionInfo* sInfo)
		: _sinfo(sInfo)
	{
		_commodity = WTSCommodityInfo::create(pid, pid, exchg, sInfo->id(), "CHINA");
		_commodity->setSessionInfo(sInfo);

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

	virtual WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t uDate = 0) override { return _contract; }
	virtual WTSArray* getContracts(const char* exchg = "", uint32_t uDate = 0) override { return NULL; }

	virtual WTSSessionInfo* getSession(const char* sid) override { return _sinfo; }
	virtual WTSSessionInfo* getSessionByCode(const char* code, const char* exchg = "") override { return _sinfo; }
	virtual WTSArray* getAllSessions() override { return NULL; }

	virtual bool isHoliday(const char* pid, uint32_t uDate, bool isTpl = false) override { return false; }

	virtual uint32_t calcTradingDate(const char* stdPID, uint32_t uDate, uint32_t uTime, bool isSession = false) override
	{
		return uDate;
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
