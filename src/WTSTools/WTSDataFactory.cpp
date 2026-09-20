/*!
 * \file WTSDataFactory.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "WTSDataFactory.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Share/TimeUtils.hpp"

using namespace std;


WTSBarStruct* WTSDataFactory::updateKlineData(WTSKlineData* klineData, WTSTickData* tick, WTSSessionInfo* sInfo, bool bAlignSec/* = false*/)
{
	if(klineData == NULL || tick == NULL)
		return NULL;

	if(strcmp(klineData->code(), tick->code()) != 0)
		return NULL;

	if(sInfo == NULL)
		return NULL;

	if (!sInfo->isInTradingTime(tick->actiontime() / 100000))
		return NULL;

	WTSKlinePeriod period = klineData->period();
	switch( period )
	{
	case KP_Tick:
		return updateSecData(sInfo, klineData, tick);
		break;
	case KP_Minute1:
		return updateMin1Data(sInfo, klineData, tick, bAlignSec);
	case KP_Minute5:
		return updateMin5Data(sInfo, klineData, tick, bAlignSec);
	case KP_DAY:
		return updateDayData(sInfo, klineData, tick);
	case KP_Sec5:
		return updateSec5Data(sInfo, klineData, tick, bAlignSec);
	default:
		return NULL;
	}
}

WTSBarStruct* WTSDataFactory::updateKlineData(WTSKlineData* klineData, WTSBarStruct* newBasicBar, WTSSessionInfo* sInfo, bool bAlignSec/* = false*/)
{
	if (klineData == NULL || newBasicBar == NULL)
		return NULL;

	if (sInfo == NULL)
		return NULL;

	WTSKlinePeriod period = klineData->period();
	switch (period)
	{
	case KP_Minute1:
		return updateMin1Data(sInfo, klineData, newBasicBar, bAlignSec);
	case KP_Minute5:
		return updateMin5Data(sInfo, klineData, newBasicBar, bAlignSec);
	case KP_Hour:
		return updateHourData(sInfo, klineData, newBasicBar);
	case KP_Half:
		return updateHalfData(sInfo, klineData, newBasicBar);
	case KP_Sec5:
		return updateSec5Data(sInfo, klineData, newBasicBar, bAlignSec);
	default:
		return NULL;
	}
}

WTSBarStruct* WTSDataFactory::updateMin1Data(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSBarStruct* newBasicBar, bool bAlignSec/* = false*/)
{
	if (sInfo == NULL)
		return NULL;

	auto secMins = sInfo->getSecMinList();

	if(klineData->times() == 1)
	{
		klineData->appendBar(*newBasicBar);
		klineData->setClosed(true);
		return klineData->at(-1);
	}

	//计算时间步长
	uint32_t steplen = klineData->times();

	const WTSBarStruct& curBar = *newBasicBar;

	uint32_t uTradingDate = curBar.date;
	uint32_t uDate = TimeUtils::minBarToDate(curBar.time);
	if (uDate == 19900000)
		uDate = uTradingDate;
	uint32_t uTime = TimeUtils::minBarToTime(curBar.time);
	uint32_t uMinute = sInfo->timeToMinutes(uTime);
	uint32_t uBarMin = 0;

	/*
	 *	By Wesley @ 2023.05.31
	 *	这里是按小节对齐的核心逻辑
	 *	1、先增加一个基础分钟数，如果不按小节对齐，就固定为0
	 *	2、如果按小节对齐，则判断当前分钟处于哪个小节，然后以上个小节结束的分钟数做基础分钟数
	 *	3、然后根据基础分钟数的差量计算新的对齐分钟数
	 *	4、最终得到bar的时间戳
	 */
	if (bAlignSec)
	{
		auto it = std::lower_bound(secMins.begin(), secMins.end(), uMinute);
		auto secIdx = it - secMins.begin();
		if (secIdx == 0)
		{
			uMinute -= 1;
			uBarMin = (uMinute / steplen)*steplen + steplen;
			if (uBarMin > secMins[secIdx])
				uBarMin = secMins[secIdx];
		}
		else
		{
			uMinute -= secMins[secIdx - 1];
			uBarMin = secMins[secIdx - 1] + (uMinute / steplen)*steplen + steplen;
			if (uBarMin > secMins[secIdx])
				uBarMin = secMins[secIdx];
		}
	}
	else
	{
		uMinute -= 1;
		uBarMin = (uMinute / steplen)*steplen + steplen;
	}

	uint64_t uBarTime = sInfo->minuteToTime(uBarMin);
	if (uBarTime < uTime)
		uDate = TimeUtils::getNextDate(uDate, 1);
	uBarTime = TimeUtils::timeToMinBar(uDate, (uint32_t)uBarTime);

	WTSBarStruct* lastBar = NULL;
	if (klineData->size() > 0)
	{
		lastBar = klineData->at(klineData->size() - 1);
	}

	bool bNewBar = false;
	if (lastBar == NULL || lastBar->date != uDate || lastBar->time != uBarTime)
	{
		//只要日期和时间都不符,则认为已经是一条新的bar了
		lastBar = new WTSBarStruct();
		bNewBar = true;

		memcpy(lastBar, &curBar, sizeof(WTSBarStruct));
		lastBar->date = uDate;
		lastBar->time = uBarTime;
	}
	else
	{
		bNewBar = false;

		lastBar->high = max(lastBar->high, curBar.high);
		lastBar->low = min(lastBar->low, curBar.low);
		lastBar->close = curBar.close;
		lastBar->settle = curBar.settle;

		lastBar->vol += curBar.vol;
		lastBar->money += curBar.money;
		lastBar->add += curBar.add;
		lastBar->hold = curBar.hold;
	}

	if(lastBar->time > curBar.time)
	{
		klineData->setClosed(false);
	}
	else
	{
		klineData->setClosed(true);
	}

	if (bNewBar)
	{
		klineData->appendBar(*lastBar);
		delete lastBar;

		return klineData->at(-1);
	}

	return NULL;
}

WTSBarStruct* WTSDataFactory::updateMin1Data(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSTickData* tick, bool bAlignSec /* = false */)
{
	//uint32_t curTime = tick->actiontime()/100000;

	uint32_t steplen = klineData->times();

	auto secMins = sInfo->getSecMinList();

	uint32_t uDate = tick->actiondate();
	uint32_t uTime = tick->actiontime() / 100000;
	uint32_t uMinute = sInfo->timeToMinutes(uTime);
	if(uMinute == INVALID_UINT32)
	{
		if(tick->volume() != 0)
		{
			WTSBarStruct *bar = klineData->at(klineData->size()-1);
			bar->close = tick->price();
			bar->high = max(bar->high,tick->price());
			bar->low = min(bar->low,tick->price());
			bar->vol += tick->volume();
			bar->money += tick->turnover();
			bar->hold = tick->openinterest();
			bar->add += tick->additional();
		}

		return NULL;
	}

	if (sInfo->isLastOfSection(uTime))
	{
		uMinute--;
	}

	uint32_t uBarMin = 0;

	/*
	 *	By Wesley @ 2023.05.31
	 *	这里是按小节对齐的核心逻辑
	 *	1、先增加一个基础分钟数，如果不按小节对齐，就固定为0
	 *	2、如果按小节对齐，则判断当前分钟处于哪个小节，然后以上个小节结束的分钟数做基础分钟数
	 *	3、然后根据基础分钟数的差量计算新的对齐分钟数
	 *	4、最终得到bar的时间戳
	 */
	if (bAlignSec)
	{
		auto it = std::lower_bound(secMins.begin(), secMins.end(), uMinute);
		auto secIdx = it - secMins.begin();
		if (secIdx == 0)
		{
			uBarMin = (uMinute / steplen)*steplen + steplen;
			if (uBarMin > secMins[secIdx])
				uBarMin = secMins[secIdx];
		}
		else
		{
			uMinute -= secMins[secIdx - 1];
			uBarMin = secMins[secIdx - 1] + (uMinute / steplen)*steplen + steplen;
			if (uBarMin > secMins[secIdx])
				uBarMin = secMins[secIdx];
		}
	}
	else
	{
		uBarMin = (uMinute / steplen)*steplen + steplen;
	}

	uint32_t uOnlyMin = sInfo->minuteToTime(uBarMin);
	if(uOnlyMin == 0)
	{
		uDate = TimeUtils::getNextDate(uDate);
	}
	uint64_t uBarTime = TimeUtils::timeToMinBar(uDate, uOnlyMin);

	uint64_t lastTime = klineData->time(-1);
	uint32_t lastDate = klineData->date(-1);
	if (lastTime == INVALID_UINT32 || uBarTime > lastTime || tick->tradingdate() > lastDate)
	{
		//如果时间不一致,则新增一条K线
		WTSBarStruct *day = new WTSBarStruct;
		day->date = tick->tradingdate();
		day->time = uBarTime;
		day->open = tick->price();
		day->high = tick->price();
		day->low = tick->price();
		day->close = tick->price();
		day->vol = tick->volume();
		day->money = tick->turnover();
		day->hold = tick->openinterest();
		day->add = tick->additional();

		klineData->appendBar(*day);
		delete day;

		return klineData->at(-1);
	}
	else if (lastTime != INVALID_UINT32 && uBarTime < lastTime)
	{
		//这种情况主要为了防止日期反复出现
		return NULL;
	}
	else
	{
		WTSBarStruct *bar = klineData->at(klineData->size()-1);
		bar->close = tick->price();
		bar->high = max(bar->high,tick->price());
		bar->low = min(bar->low,tick->price());
		bar->vol += tick->volume();
		bar->money += tick->turnover();
		bar->hold = tick->openinterest();
		bar->add += tick->additional();

		return NULL;
	}
}

WTSBarStruct* WTSDataFactory::updateMin5Data(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSBarStruct* newBasicBar, bool bAlignSec/* = false*/)
{
	if (sInfo == NULL)
		return NULL;

	auto secMins = sInfo->getSecMinList();

	if (klineData->times() == 1)
	{
		klineData->appendBar(*newBasicBar);
		return klineData->at(-1);
	}

	//计算时间步长
	uint32_t steplen = 5 * klineData->times();

	const WTSBarStruct& curBar = *newBasicBar;

	uint32_t uTradingDate = curBar.date;
	uint32_t barDate = TimeUtils::minBarToDate(curBar.time);
	if (barDate == 19900000)
		barDate = uTradingDate;
	uint32_t uTime = TimeUtils::minBarToTime(curBar.time);
	uint32_t uMinute = sInfo->timeToMinutes(uTime);
	uint32_t uBarMin = 0;
	/*
	 *	By Wesley @ 2023.05.31
	 *	这里是按小节对齐的核心逻辑
	 *	1、先增加一个基础分钟数，如果不按小节对齐，就固定为0
	 *	2、如果按小节对齐，则判断当前分钟处于哪个小节，然后以上个小节结束的分钟数做基础分钟数
	 *	3、然后根据基础分钟数的差量计算新的对齐分钟数
	 *	4、最终得到bar的时间戳
	 */
	if (bAlignSec)
	{
		auto it = std::lower_bound(secMins.begin(), secMins.end(), uMinute);
		auto secIdx = it - secMins.begin();
		if (secIdx == 0)
		{
			uMinute -= 5;
			uBarMin = (uMinute / steplen)*steplen + steplen;
			if (uBarMin > secMins[secIdx])
				uBarMin = secMins[secIdx];
		}
		else
		{
			uMinute -= secMins[secIdx - 1];
			uBarMin = secMins[secIdx - 1] + (uMinute / steplen)*steplen + steplen;
			if (uBarMin > secMins[secIdx])
				uBarMin = secMins[secIdx];
		}
	}
	else
	{
		uMinute -= 5;
		uBarMin = (uMinute / steplen)*steplen + steplen;
	}

	uint64_t uBarTime = sInfo->minuteToTime(uBarMin);
	if (uBarTime < uTime)
		barDate = TimeUtils::getNextDate(barDate, 1);
	uBarTime = TimeUtils::timeToMinBar(barDate, (uint32_t)uBarTime);

	WTSBarStruct* lastBar = NULL;
	if (klineData->size() > 0)
	{
		lastBar = klineData->at(klineData->size() - 1);
	}

	bool bNewBar = false;
	if (lastBar == NULL || lastBar->time != uBarTime)
	{

		//只要日期和时间都不符,则认为已经是一条新的bar了
		lastBar = new WTSBarStruct();
		bNewBar = true;

		memcpy(lastBar, &curBar, sizeof(WTSBarStruct));
		lastBar->date = uTradingDate;
		lastBar->time = uBarTime;
	}
	else
	{
		bNewBar = false;

		lastBar->high = max(lastBar->high, curBar.high);
		lastBar->low = min(lastBar->low, curBar.low);
		lastBar->close = curBar.close;
		lastBar->settle = curBar.settle;

		lastBar->vol += curBar.vol;
		lastBar->money += curBar.money;
		lastBar->add += curBar.add;
		lastBar->hold = curBar.hold;
	}

	if (lastBar->time > curBar.time)
	{
		klineData->setClosed(false);
	}
	else
	{
		klineData->setClosed(true);
	}

	if (bNewBar)
	{
		klineData->appendBar(*lastBar);
		delete lastBar;

		return klineData->at(-1);
	}

	return NULL;
}

WTSBarStruct* WTSDataFactory::updateMin5Data(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSTickData* tick, bool bAlignSec /* = false */)
{
	auto secMins = sInfo->getSecMinList();

	uint32_t steplen = 5*klineData->times();

	uint32_t uDate = tick->actiondate();
	uint32_t uTime = tick->actiontime()/100000;
	uint32_t uMinute = sInfo->timeToMinutes(uTime);
	if (sInfo->isLastOfSection(uTime))
	{
		uMinute--;
	}

	uint32_t uBarMin = 0;
	/*
	 *	By Wesley @ 2023.05.31
	 *	这里是按小节对齐的核心逻辑
	 *	1、先增加一个基础分钟数，如果不按小节对齐，就固定为0
	 *	2、如果按小节对齐，则判断当前分钟处于哪个小节，然后以上个小节结束的分钟数做基础分钟数
	 *	3、然后根据基础分钟数的差量计算新的对齐分钟数
	 *	4、最终得到bar的时间戳
	 */
	if (bAlignSec)
	{
		auto it = std::lower_bound(secMins.begin(), secMins.end(), uMinute);
		auto secIdx = it - secMins.begin();
		if (secIdx == 0)
		{
			uBarMin = (uMinute / steplen)*steplen + steplen;
			if (uBarMin > secMins[secIdx])
				uBarMin = secMins[secIdx];
		}
		else
		{
			uMinute -= secMins[secIdx - 1];
			uBarMin = secMins[secIdx - 1] + (uMinute / steplen)*steplen + steplen;
			if (uBarMin > secMins[secIdx])
				uBarMin = secMins[secIdx];
		}
	}
	else
	{
		uBarMin = (uMinute / steplen)*steplen + steplen;
	}

	uint32_t uOnlyMin = sInfo->minuteToTime(uBarMin);
	if (uOnlyMin == 0)
	{
		uDate = TimeUtils::getNextDate(uDate);
	}
	uint64_t uBarTime = TimeUtils::timeToMinBar(uDate, uOnlyMin);

	uint64_t lastTime = klineData->time(klineData->size()-1);
	if(lastTime == INVALID_UINT32 || uBarTime != lastTime)
	{
		//如果时间不一致,则新增一条K线
		WTSBarStruct *day = new WTSBarStruct;
		day->date = tick->tradingdate();
		day->time = uBarTime;
		day->open = tick->price();
		day->high = tick->price();
		day->low = tick->price();
		day->close = tick->price();
		day->vol = tick->volume();
		day->money = tick->turnover();
		day->hold = tick->openinterest();
		day->add = tick->additional();

		klineData->appendBar(*day);
		delete day;

		return klineData->at(-1);
	}
	else
	{
		WTSBarStruct *bar = klineData->at(klineData->size()-1);
		bar->close = tick->price();
		bar->high = max(bar->high,tick->price());
		bar->low = min(bar->low,tick->price());
		bar->vol += tick->volume();
		bar->money += tick->turnover();
		bar->hold = tick->openinterest();
		bar->add = tick->additional();

		return NULL;
	}
}

WTSBarStruct* WTSDataFactory::updateHourData(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSBarStruct* newBasicBar)
{
	if (sInfo == NULL)
		return NULL;

	uint32_t uTradingDate = newBasicBar->date;
	uint32_t uYYYYMMDD = TimeUtils::minBarToDate(newBasicBar->time);
	if (uYYYYMMDD == 19900000)
		uYYYYMMDD = uTradingDate;
	uint32_t uHHMM = TimeUtils::minBarToTime(newBasicBar->time);
	uint32_t uHH = uHHMM / 100;
	uint32_t uMM = uHHMM % 100;
	if (uMM != 0)
	{
		uHH += 1;
		if (uHH == 24)
		{
			uYYYYMMDD = TimeUtils::getNextDate(uYYYYMMDD);
			uHH = 0;
		}
	}

	uHHMM = uHH * 100;
	uint64_t uBarTime = TimeUtils::timeToMinBar(uYYYYMMDD, uHHMM);

	WTSBarStruct* lastBar = NULL;
	if (klineData->size() > 0)
	{
		lastBar = klineData->at(klineData->size() - 1);
	}

	bool bNewBar = false;
	if (lastBar == NULL || lastBar->time != uBarTime)
	{

		//只要日期和时间都不符,则认为已经是一条新的bar了
		lastBar = new WTSBarStruct();
		bNewBar = true;

		memcpy(lastBar, newBasicBar, sizeof(WTSBarStruct));
		lastBar->date = uTradingDate;
		lastBar->time = uBarTime;
	}
	else
	{
		bNewBar = false;

		lastBar->high = max(lastBar->high, newBasicBar->high);
		lastBar->low = min(lastBar->low, newBasicBar->low);
		lastBar->close = newBasicBar->close;
		lastBar->settle = newBasicBar->settle;

		lastBar->vol += newBasicBar->vol;
		lastBar->money += newBasicBar->money;
		lastBar->add += newBasicBar->add;
		lastBar->hold = newBasicBar->hold;
	}

	if (lastBar->time > newBasicBar->time)
	{
		klineData->setClosed(false);
	}
	else
	{
		klineData->setClosed(true);
	}

	if (bNewBar)
	{
		klineData->appendBar(*lastBar);
		delete lastBar;

		return klineData->at(-1);
	}

	return NULL;
}

WTSBarStruct* WTSDataFactory::updateHalfData(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSBarStruct* newBasicBar)
{
	uint32_t uTradingDate = newBasicBar->date;
	uint32_t uYYYYMMDD = TimeUtils::minBarToDate(newBasicBar->time);
	if (uYYYYMMDD == 19900000)
		uYYYYMMDD = uTradingDate;
	uint32_t uHHMM = TimeUtils::minBarToTime(newBasicBar->time);
	uint32_t uHH = uHHMM / 100;
	uint32_t uMM = uHHMM % 100;
	if (0 < uMM && uMM <= 30)
		uMM = 30;
	else
	{
		if (uMM != 0)
		{
			uHH += 1;
			if (uHH == 24)
			{
				uYYYYMMDD = TimeUtils::getNextDate(uYYYYMMDD);
				uHH = 0;
			}
		}

		uMM = 0;
	}

	uHHMM = uHH * 100 + uMM;

	uint64_t uBarTime = TimeUtils::timeToMinBar(uYYYYMMDD, uHHMM);

	WTSBarStruct* lastBar = NULL;
	if (klineData->size() > 0)
	{
		lastBar = klineData->at(klineData->size() - 1);
	}

	bool bNewBar = false;
	if (lastBar == NULL || lastBar->time != uBarTime)
	{
		//只要日期和时间都不符,则认为已经是一条新的bar了
		lastBar = new WTSBarStruct();
		bNewBar = true;

		memcpy(lastBar, newBasicBar, sizeof(WTSBarStruct));
		lastBar->date = uTradingDate;
		lastBar->time = uBarTime;
	}
	else
	{
		bNewBar = false;

		lastBar->high = max(lastBar->high, newBasicBar->high);
		lastBar->low = min(lastBar->low, newBasicBar->low);
		lastBar->close = newBasicBar->close;
		lastBar->settle = newBasicBar->settle;

		lastBar->vol += newBasicBar->vol;
		lastBar->money += newBasicBar->money;
		lastBar->add += newBasicBar->add;
		lastBar->hold = newBasicBar->hold;
	}

	if (lastBar->time > newBasicBar->time)
	{
		klineData->setClosed(false);
	}
	else
	{
		klineData->setClosed(true);
	}

	if (bNewBar)
	{
		klineData->appendBar(*lastBar);
		delete lastBar;

		return klineData->at(-1);
	}

	return NULL;
}

WTSBarStruct* WTSDataFactory::updateDayData(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSTickData* tick)
{
	uint32_t curDate = tick->tradingdate();
	uint32_t lastDate = klineData->date(klineData->size()-1);

	if(lastDate == INVALID_UINT32 || curDate != lastDate)
	{
		//如果时间不一致,则新增一条K线
		WTSBarStruct *day = new WTSBarStruct;
		day->date = curDate;
		day->time = 0;
		day->open = tick->price();
		day->high = tick->price();
		day->low = tick->price();
		day->close = tick->price();
		day->vol = tick->volume();
		day->money = tick->turnover();
		day->hold = tick->openinterest();
		day->add = tick->additional();

		return day;
	}
	else
	{
		WTSBarStruct *bar = klineData->at(klineData->size()-1);
		bar->close = tick->price();
		bar->high = max(bar->high,tick->price());
		bar->low = min(bar->low,tick->price());
		bar->vol += tick->volume();
		bar->money += tick->turnover();
		bar->hold = tick->openinterest();
		bar->add += tick->additional();

		return NULL;
	}
}

/*
 *	按交易秒序号计算bar的对齐秒序号
 *	By 秒K线支持 @ 2026.09.20
 *
 *	sInfo->timeToSeconds 返回的是"从开盘算起的累计交易秒序号"，
 *	并且已经处理了小节结束时刻的归属（seconds == stopSecs 时 offset--），
 *	所以落在小节结束秒上的tick会归到该小节最后一根，与min1的 minutes-- 处理一致
 */
uint32_t WTSDataFactory::alignBarSeconds(WTSSessionInfo* sInfo, uint32_t curSecs, uint32_t seconds, bool bAlignSec)
{
	if (!bAlignSec)
		return (curSecs / seconds)*seconds + seconds;

	/*
	 *	按小节对齐：
	 *	小节边界一定落在整分钟上（addTradingSection的参数是HHMM），
	 *	所以 getSecMinList() 的累计分钟数 *60 就是累计秒边界
	 */
	const std::vector<uint32_t>& secMins = sInfo->getSecMinList();
	uint32_t prevBound = 0;
	uint32_t curBound = 0;
	for (auto it = secMins.begin(); it != secMins.end(); it++)
	{
		curBound = (*it) * 60;
		if (curSecs < curBound)
			break;
		prevBound = curBound;
	}

	uint32_t offset = curSecs - prevBound;
	uint32_t barSecs = prevBound + (offset / seconds)*seconds + seconds;

	//小节结束处强制对齐，不允许跨小节
	if (barSecs > curBound)
		barSecs = curBound;

	return barSecs;
}

/*
 *	秒线更新的公共核心
 *	By 秒K线支持 @ 2026.09.20
 *
 *	这里修复了原 updateSecData 的两个缺陷：
 *	1、新bar只是 new 出来返回，既没有 appendBar 也没有 delete
 *	   —— klineData 永远不增长，导致下一个tick的 time(size-1) 仍返回 INVALID_UINT32，
 *	      每个tick都被判成"新bar"，OHLC累积分支永远走不到，同时每次都泄漏一个 WTSBarStruct
 *	2、barTime 是裸 HHMMSS 不含日期，跨日/跨小节排序会错
 *	   —— 统一改用 TimeUtils::timeToSecBar（yyyyMMddHHmmss）
 *
 *	返回值约定与 updateMin1Data 一致：产生新bar时返回容器内的指针，否则返回NULL
 */
WTSBarStruct* WTSDataFactory::updateSecBar(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSTickData* tick, uint32_t seconds, bool bAlignSec /* = false */)
{
	if (sInfo == NULL || klineData == NULL || tick == NULL || seconds == 0)
		return NULL;

	uint32_t uTime = tick->actiontime() / 1000;	//HHMMSS
	uint32_t curSecs = sInfo->timeToSeconds(uTime);
	if (curSecs == INVALID_UINT32)
	{
		//非交易时间的tick直接丢弃，不能让它污染已有的bar
		return NULL;
	}

	uint32_t barSecs = alignBarSeconds(sInfo, curSecs, seconds, bAlignSec);
	uint32_t barTime = sInfo->secondsToTime(barSecs);
	if (barTime == INVALID_UINT32)
		return NULL;

	uint32_t uDate = tick->actiondate();
	if (barTime < uTime)
	{
		//bar的收盘时刻小于tick时刻，说明跨日了
		uDate = TimeUtils::getNextDate(uDate);
	}

	uint64_t uBarTime = 0;
	if (klineData->isUnixTime())
		uBarTime = (uint64_t)TimeUtils::makeTime(uDate, (long)barTime * 1000) / 1000;
	else
		uBarTime = TimeUtils::timeToSecBar(uDate, barTime);

	WTSBarStruct* lastBar = NULL;
	if (klineData->size() > 0)
		lastBar = klineData->at(-1);

	if (lastBar == NULL || uBarTime > lastBar->time || tick->tradingdate() > lastBar->date)
	{
		WTSBarStruct newBar;
		newBar.date = tick->tradingdate();
		newBar.time = uBarTime;
		newBar.open = tick->price();
		newBar.high = tick->price();
		newBar.low = tick->price();
		newBar.close = tick->price();
		newBar.vol = tick->volume();
		newBar.money = tick->turnover();
		newBar.hold = tick->openinterest();
		newBar.add = tick->additional();

		klineData->appendBar(newBar);
		return klineData->at(-1);
	}
	else if (uBarTime < lastBar->time)
	{
		//时间倒序的tick，不能回写已闭合的bar
		return NULL;
	}
	else
	{
		lastBar->close = tick->price();
		lastBar->high = max(lastBar->high, tick->price());
		lastBar->low = min(lastBar->low, tick->price());
		lastBar->vol += tick->volume();
		lastBar->money += tick->turnover();
		lastBar->hold = tick->openinterest();
		lastBar->add += tick->additional();

		return NULL;
	}
}

/*
 *	KP_Tick 路径：times 直接就是秒数
 *	WtDtServo 的 get_sbars / update_bars 走这里
 */
WTSBarStruct* WTSDataFactory::updateSecData(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSTickData* tick)
{
	if (klineData == NULL)
		return NULL;

	return updateSecBar(sInfo, klineData, tick, klineData->times(), false);
}

/*
 *	KP_Sec5 路径：times 是5秒的倍数
 */
WTSBarStruct* WTSDataFactory::updateSec5Data(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSTickData* tick, bool bAlignSec /* = false */)
{
	if (klineData == NULL)
		return NULL;

	return updateSecBar(sInfo, klineData, tick, klineData->times() * 5, bAlignSec);
}

/*
 *	KP_Sec5 路径：用已闭合的sec5基础线更新更大的秒周期
 */
WTSBarStruct* WTSDataFactory::updateSec5Data(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSBarStruct* newBasicBar, bool bAlignSec /* = false */)
{
	if (sInfo == NULL || klineData == NULL || newBasicBar == NULL)
		return NULL;

	//一倍周期直接追加
	if (klineData->times() == 1)
	{
		klineData->appendBar(*newBasicBar);
		klineData->setClosed(true);
		return klineData->at(-1);
	}

	uint32_t steplen = klineData->times() * 5;

	const WTSBarStruct& curBar = *newBasicBar;

	uint32_t uDate = TimeUtils::secBarToDate(curBar.time);
	uint32_t uTime = TimeUtils::secBarToTime(curBar.time);
	uint32_t curSecs = sInfo->timeToSeconds(uTime);
	if (curSecs == INVALID_UINT32)
		return NULL;

	/*
	 *	基础线的时间戳是"闭合时刻"，如090005代表090000~090004这5秒，
	 *	所以要先退一秒再对齐，和 extractMin1Data 里的 uMinute -= 1 同理
	 */
	if (curSecs > 0)
		curSecs -= 1;

	uint32_t barSecs = alignBarSeconds(sInfo, curSecs, steplen, bAlignSec);
	uint32_t barTime = sInfo->secondsToTime(barSecs);
	if (barTime == INVALID_UINT32)
		return NULL;

	if (barTime < uTime)
		uDate = TimeUtils::getNextDate(uDate);

	uint64_t uBarTime = TimeUtils::timeToSecBar(uDate, barTime);

	WTSBarStruct* lastBar = NULL;
	if (klineData->size() > 0)
		lastBar = klineData->at(-1);

	if (lastBar == NULL || uBarTime > lastBar->time)
	{
		WTSBarStruct newBar;
		memcpy(&newBar, &curBar, sizeof(WTSBarStruct));
		newBar.time = uBarTime;

		klineData->appendBar(newBar);
		klineData->setClosed(uBarTime == curBar.time);
		return klineData->at(-1);
	}
	else if (uBarTime < lastBar->time)
	{
		return NULL;
	}
	else
	{
		lastBar->high = max(lastBar->high, curBar.high);
		lastBar->low = min(lastBar->low, curBar.low);
		lastBar->close = curBar.close;
		lastBar->settle = curBar.settle;
		lastBar->vol += curBar.vol;
		lastBar->money += curBar.money;
		lastBar->add += curBar.add;
		lastBar->hold = curBar.hold;

		//时间戳一致说明这一根正好闭合
		klineData->setClosed(uBarTime == curBar.time);
		return NULL;
	}
}

uint32_t WTSDataFactory::getPrevMinute(uint32_t curMinute, int period /* = 1 */)
{
	uint32_t h = curMinute/100;
	uint32_t m = curMinute%100;
	if(m == 0)
	{
		m = 60;
		if(h == 0) h = 24;

		return (h-1)*100 + (m-period);
	}
	else
	{
		return h*100 + m - period;
	}
}

WTSKlineData* WTSDataFactory::extractKlineData(WTSKlineSlice* baseKline, WTSKlinePeriod period, uint32_t times, WTSSessionInfo* sInfo, 
		bool bIncludeOpen /* = true */, bool bAlignSec /* = false */)
{
	if(baseKline == NULL || baseKline->size() == 0)
		return NULL;

	//一倍,则不需要转换
	if(times <= 1 || period == KP_Tick)
	{
		return NULL;
	}

	if(period == KP_DAY)
	{
		return extractDayData(baseKline, times, bIncludeOpen);
	}
	else if(period == KP_Sec5)
	{
		return extractSec5Data(baseKline, times, sInfo, bIncludeOpen, bAlignSec);
	}
	else if(period == KP_Minute1)
	{
		return extractMin1Data(baseKline, times, sInfo, bIncludeOpen, bAlignSec);
	}
	else if(period == KP_Minute5)
	{
		if (times == PERIOD_TIMES_HOUR)
			return extractHourData(baseKline, sInfo, bIncludeOpen);
		else if (times == PERIOD_TIMES_HALF)
			return extractHalfData(baseKline, sInfo, bIncludeOpen);
		else
			return extractMin5Data(baseKline, times, sInfo, bIncludeOpen, bAlignSec);
	}
	
	return NULL;
}

/*
 *	从sec5基础线重采样到 5*times 秒线
 *	By 秒K线支持 @ 2026.09.20
 *
 *	结构完全照 extractMin1Data，差别只在：
 *	1、时间编码用 secBarToDate/secBarToTime（yyyyMMddHHmmss）
 *	2、步长单位是秒，且 steplen = 5*times
 *	3、对齐逻辑抽到 alignBarSeconds，与 updateSec5Data 共用同一套规则
 */
WTSKlineData* WTSDataFactory::extractSec5Data(WTSKlineSlice* baseKline, uint32_t times, WTSSessionInfo* sInfo, bool bIncludeOpen /* = true */, bool bAlignSec /* = false */)
{
	if (sInfo == NULL || baseKline == NULL || baseKline->size() == 0)
		return NULL;

	uint32_t steplen = times * 5;

	WTSKlineData* ret = WTSKlineData::create(baseKline->code(), 0);
	ret->setPeriod(KP_Sec5, times);

	for (auto i = 0; i < baseKline->size(); i++)
	{
		const WTSBarStruct& curBar = *baseKline->at(i);

		uint32_t uDate = TimeUtils::secBarToDate(curBar.time);
		uint32_t uTime = TimeUtils::secBarToTime(curBar.time);
		uint32_t curSecs = sInfo->timeToSeconds(uTime);
		if (curSecs == INVALID_UINT32)
			continue;

		/*
		 *	基础线时间戳是闭合时刻（090005 代表 090000~090004），
		 *	先退一秒再对齐，同 extractMin1Data 的 uMinute -= 1
		 */
		if (curSecs > 0)
			curSecs -= 1;

		uint32_t barSecs = alignBarSeconds(sInfo, curSecs, steplen, bAlignSec);
		uint32_t barTime = sInfo->secondsToTime(barSecs);
		if (barTime == INVALID_UINT32)
			continue;

		if (barTime < uTime)
			uDate = TimeUtils::getNextDate(uDate);

		uint64_t uBarTime = TimeUtils::timeToSecBar(uDate, barTime);

		WTSBarStruct* lastBar = NULL;
		if (ret->size() > 0)
			lastBar = ret->at(ret->size() - 1);

		bool bNewBar = false;
		if (lastBar == NULL || lastBar->time != uBarTime)
		{
			lastBar = new WTSBarStruct();
			bNewBar = true;

			memcpy(lastBar, &curBar, sizeof(WTSBarStruct));
			lastBar->time = uBarTime;
		}
		else
		{
			lastBar->high = max(lastBar->high, curBar.high);
			lastBar->low = min(lastBar->low, curBar.low);
			lastBar->close = curBar.close;
			lastBar->settle = curBar.settle;

			lastBar->vol += curBar.vol;
			lastBar->money += curBar.money;
			lastBar->add += curBar.add;
			lastBar->hold = curBar.hold;
		}

		if (bNewBar)
		{
			ret->appendBar(*lastBar);
			delete lastBar;
		}
	}

	if (ret->size() == 0)
		return ret;

	//检查最后一条：如果目标K线的时间戳超过了原始K线最后一条，说明未闭合
	{
		WTSBarStruct* lastRawBar = baseKline->at(-1);
		WTSBarStruct* lastDesBar = ret->at(-1);
		if (lastDesBar->time > lastRawBar->time)
		{
			if (!bIncludeOpen)
				ret->getDataRef().resize(ret->size() - 1);
			else
				ret->setClosed(false);
		}
	}

	return ret;
}

WTSKlineData* WTSDataFactory::extractMin1Data(WTSKlineSlice* baseKline, uint32_t times, WTSSessionInfo* sInfo, bool bIncludeOpen /* = true */, bool bAlignSec /* = false */)
{
	//根据合约代码获取市场信息
	if(sInfo == NULL)
		return NULL;

	//计算时间步长
	uint32_t steplen = times;

	/*
	 *	By Wesley @ 2023.05.31
	 *	要增加一个按照小节对齐的重采样方式
	 *	一般逻辑就是每个小节开始重新计算条数，然后在小节结束时，强制对齐
	 */
	auto secMins = sInfo->getSecMinList();

	WTSKlineData* ret = WTSKlineData::create(baseKline->code(), 0);
	ret->setPeriod(KP_Minute1, times);

	for (auto i = 0; i < baseKline->size(); i++)
	{
		const WTSBarStruct& curBar = *baseKline->at(i);

		uint32_t uTradingDate = curBar.date;
		uint32_t uDate = TimeUtils::minBarToDate(curBar.time);
		if(uDate == 19900000)
			uDate = uTradingDate;
		uint32_t uTime = TimeUtils::minBarToTime(curBar.time);
		uint32_t uMinute = sInfo->timeToMinutes(uTime);
		uint32_t uBarMin = 0;

		/*
		 *	By Wesley @ 2023.05.31
		 *	这里是按小节对齐的核心逻辑
		 *	1、先增加一个基础分钟数，如果不按小节对齐，就固定为0
		 *	2、如果按小节对齐，则判断当前分钟处于哪个小节，然后以上个小节结束的分钟数做基础分钟数
		 *	3、然后根据基础分钟数的差量计算新的对齐分钟数
		 *	4、最终得到bar的时间戳
		 */
		if(bAlignSec)
		{
			auto it = std::lower_bound(secMins.begin(), secMins.end(), uMinute);
			auto secIdx = it - secMins.begin();
			if(secIdx == 0)
			{
				uMinute -= 1;
				uBarMin = (uMinute / steplen)*steplen + steplen;
				if (uBarMin > secMins[secIdx])
					uBarMin = secMins[secIdx];
			}
			else
			{
				uMinute -= secMins[secIdx - 1];
				uBarMin = secMins[secIdx - 1] + (uMinute / steplen)*steplen + steplen;
				if (uBarMin > secMins[secIdx])
					uBarMin = secMins[secIdx];
			}
		}
		else
		{
			uMinute -= 1;
			uBarMin = (uMinute / steplen)*steplen + steplen;
		}

		uint64_t uBarTime = sInfo->minuteToTime(uBarMin);
		if (uBarTime < uTime)
			uDate = TimeUtils::getNextDate(uDate, 1);
		uBarTime = TimeUtils::timeToMinBar(uDate, (uint32_t)uBarTime);

		WTSBarStruct* lastBar = NULL;
		if(ret->size() > 0)
		{
			lastBar = ret->at(ret->size()-1);
		}

		bool bNewBar = false;
		if(lastBar == NULL || lastBar->time != uBarTime)
		{
			//只要日期和时间都不符,则认为已经是一条新的bar了
			lastBar = new WTSBarStruct();
			bNewBar = true;

			memcpy(lastBar, &curBar, sizeof(WTSBarStruct));
			lastBar->date = uDate;
			lastBar->time = uBarTime;
		}
		else
		{
			bNewBar = false;

			lastBar->high = max(lastBar->high, curBar.high);
			lastBar->low = min(lastBar->low, curBar.low);
			lastBar->close = curBar.close;
			lastBar->settle = curBar.settle;

			lastBar->vol += curBar.vol;
			lastBar->money += curBar.money;
			lastBar->add += curBar.add;
			lastBar->hold = curBar.hold;
		}

		if(bNewBar)
		{
			ret->appendBar(*lastBar);
			delete lastBar;
		}
	}

	//检查最后一条数据
	{
		WTSBarStruct* lastRawBar = baseKline->at(-1);
		WTSBarStruct* lastDesBar = ret->at(-1);
		//如果目标K线的最后一条数据的日期或者时间大于原始K线最后一条的日期或时间
		if ( lastDesBar->date > lastRawBar->date || lastDesBar->time > lastRawBar->time)
		{
			if (!bIncludeOpen)
				ret->getDataRef().resize(ret->size() - 1);
			else
				ret->setClosed(false);
		}
	}
	

	return ret;
}

WTSKlineData* WTSDataFactory::extractMin5Data(WTSKlineSlice* baseKline, uint32_t times, WTSSessionInfo* sInfo, bool bIncludeOpen /* = true */, bool bAlignSec /* = false */)
{
	if(sInfo == NULL)
		return NULL;

	//计算时间步长
	uint32_t steplen = 5*times;
	/*
	 *	By Wesley @ 2023.05.31
	 *	要增加一个按照小节对齐的重采样方式
	 *	一般逻辑就是每个小节开始重新计算条数，然后在小节结束时，强制对齐
	 */
	auto secMins = sInfo->getSecMinList();

	WTSKlineData* ret = WTSKlineData::create(baseKline->code(), 0);
	ret->setPeriod(KP_Minute5, times);

	for (auto i = 0; i < baseKline->size(); i++)
	{
		const WTSBarStruct& curBar = *baseKline->at(i);

		uint32_t uTradingDate = curBar.date;
		uint32_t uDate = TimeUtils::minBarToDate(curBar.time);
		if(uDate == 19900000)
			uDate = uTradingDate;
		uint32_t uTime = TimeUtils::minBarToTime(curBar.time);
		uint32_t uMinute = sInfo->timeToMinutes(uTime);
		uint32_t uBarMin = 0;
		/*
		 *	By Wesley @ 2023.05.31
		 *	这里是按小节对齐的核心逻辑
		 *	1、先增加一个基础分钟数，如果不按小节对齐，就固定为0
		 *	2、如果按小节对齐，则判断当前分钟处于哪个小节，然后以上个小节结束的分钟数做基础分钟数
		 *	3、然后根据基础分钟数的差量计算新的对齐分钟数
		 *	4、最终得到bar的时间戳
		 */
		if (bAlignSec)
		{
			auto it = std::lower_bound(secMins.begin(), secMins.end(), uMinute);
			auto secIdx = it - secMins.begin();
			if (secIdx == 0)
			{
				uMinute -= 5;
				uBarMin = (uMinute / steplen)*steplen + steplen;
				if (uBarMin > secMins[secIdx])
					uBarMin = secMins[secIdx];
			}
			else
			{
				uMinute -= secMins[secIdx - 1];
				uBarMin = secMins[secIdx - 1] + (uMinute / steplen)*steplen + steplen;
				if (uBarMin > secMins[secIdx])
					uBarMin = secMins[secIdx];
			}
		}
		else
		{
			uMinute -= 5;
			uBarMin = (uMinute / steplen)*steplen + steplen;
		}

		uint64_t uBarTime = sInfo->minuteToTime(uBarMin);
		if (uBarTime < uTime)
			uDate = TimeUtils::getNextDate(uDate, 1);
		uBarTime = TimeUtils::timeToMinBar(uDate, (uint32_t)uBarTime);

		WTSBarStruct* lastBar = NULL;
		if(ret->size() > 0)
		{
			lastBar = ret->at(ret->size()-1);
		}

		bool bNewBar = false;
		if(lastBar == NULL || lastBar->time != uBarTime)
		{
			//只要日期和时间都不符,则认为已经是一条新的bar了
			lastBar = new WTSBarStruct();
			bNewBar = true;

			memcpy(lastBar, &curBar, sizeof(WTSBarStruct));
			lastBar->date = uTradingDate;
			lastBar->time = uBarTime;
		}
		else
		{
			bNewBar = false;

			lastBar->high = max(lastBar->high, curBar.high);
			lastBar->low = min(lastBar->low, curBar.low);
			lastBar->close = curBar.close;
			lastBar->settle = curBar.settle;

			lastBar->vol += curBar.vol;
			lastBar->money += curBar.money;
			lastBar->add += curBar.add;
			lastBar->hold = curBar.hold;
		}

		if(bNewBar)
		{
			ret->appendBar(*lastBar);
			delete lastBar;
		}
	}

	//检查最后一条数据
	{
		WTSBarStruct* lastRawBar = baseKline->at(-1);
		WTSBarStruct* lastDesBar = ret->at(-1);
		//如果目标K线的最后一条数据的日期或者时间大于原始K线最后一条的日期或时间
		if (lastDesBar->date > lastRawBar->date || lastDesBar->time > lastRawBar->time)
		{
			if (!bIncludeOpen)
				ret->getDataRef().resize(ret->size() - 1);
			else
				ret->setClosed(false);
		}
	}

	return ret;
}

WTSKlineData* WTSDataFactory::extractHourData(WTSKlineSlice* baseKline, WTSSessionInfo* sInfo, bool bIncludeOpen /* = true */)
{
	if (sInfo == NULL)
		return NULL;

	auto secMins = sInfo->getSecMinList();

	WTSKlineData* ret = WTSKlineData::create(baseKline->code(), 0);
	ret->setPeriod(KP_Hour, 1);

	for (auto i = 0; i < baseKline->size(); i++)
	{
		const WTSBarStruct& curBar = *baseKline->at(i);

		uint32_t uTradingDate = curBar.date;
		uint32_t uYYYYMMDD = TimeUtils::minBarToDate(curBar.time);
		if (uYYYYMMDD == 19900000)
			uYYYYMMDD = uTradingDate;
		uint32_t uHHMM = TimeUtils::minBarToTime(curBar.time);
		uint32_t uHH = uHHMM / 100;
		uint32_t uMM = uHHMM % 100;
		if (uMM != 0)
		{
			uHH += 1;
			if (uHH == 24)
			{
				uYYYYMMDD = TimeUtils::getNextDate(uYYYYMMDD);
				uHH = 0;
			}
		}

		uint64_t uBarTime = TimeUtils::timeToMinBar(uYYYYMMDD, uHH*100);

		WTSBarStruct* lastBar = NULL;
		if (ret->size() > 0)
		{
			lastBar = ret->at(ret->size() - 1);
		}

		bool bNewBar = false;
		if (lastBar == NULL || lastBar->time != uBarTime)
		{
			//只要日期和时间都不符,则认为已经是一条新的bar了
			lastBar = new WTSBarStruct();
			bNewBar = true;

			memcpy(lastBar, &curBar, sizeof(WTSBarStruct));
			lastBar->date = uTradingDate;
			lastBar->time = uBarTime;
		}
		else
		{
			bNewBar = false;

			lastBar->high = max(lastBar->high, curBar.high);
			lastBar->low = min(lastBar->low, curBar.low);
			lastBar->close = curBar.close;
			lastBar->settle = curBar.settle;

			lastBar->vol += curBar.vol;
			lastBar->money += curBar.money;
			lastBar->add += curBar.add;
			lastBar->hold = curBar.hold;
		}

		if (bNewBar)
		{
			ret->appendBar(*lastBar);
			delete lastBar;
		}
	}

	//检查最后一条数据
	{
		WTSBarStruct* lastRawBar = baseKline->at(-1);
		WTSBarStruct* lastDesBar = ret->at(-1);
		//如果目标K线的最后一条数据的日期或者时间大于原始K线最后一条的日期或时间
		if (lastDesBar->date > lastRawBar->date || lastDesBar->time > lastRawBar->time)
		{
			if (!bIncludeOpen)
				ret->getDataRef().resize(ret->size() - 1);
			else
				ret->setClosed(false);
		}
	}

	return ret;
}

WTSKlineData* WTSDataFactory::extractHalfData(WTSKlineSlice* baseKline, WTSSessionInfo* sInfo, bool bIncludeOpen /* = true */)
{
	if (sInfo == NULL)
		return NULL;

	/*
	 *	By Wesley @ 2023.05.31
	 *	要增加一个按照小节对齐的重采样方式
	 *	一般逻辑就是每个小节开始重新计算条数，然后在小节结束时，强制对齐
	 */
	auto secMins = sInfo->getSecMinList();

	WTSKlineData* ret = WTSKlineData::create(baseKline->code(), 0);
	ret->setPeriod(KP_Half, 1);

	for (auto i = 0; i < baseKline->size(); i++)
	{
		const WTSBarStruct& curBar = *baseKline->at(i);

		uint32_t uTradingDate = curBar.date;
		uint32_t uYYYYMMDD = TimeUtils::minBarToDate(curBar.time);
		if (uYYYYMMDD == 19900000)
			uYYYYMMDD = uTradingDate;
		uint32_t uHHMM = TimeUtils::minBarToTime(curBar.time);
		uint32_t uHH = uHHMM / 100;
		uint32_t uMM = uHHMM % 100;
		if (0 < uMM && uMM <= 30)
			uMM = 30;
		else
		{
			if(uMM != 0)
			{
				uHH += 1;
				if (uHH == 24)
				{
					uYYYYMMDD = TimeUtils::getNextDate(uYYYYMMDD);
					uHH = 0;
				}
			}

			uMM = 0;
		}

		uHHMM = uHH * 100 + uMM;

		uint64_t uBarTime = TimeUtils::timeToMinBar(uYYYYMMDD, uHHMM);

		WTSBarStruct* lastBar = NULL;
		if (ret->size() > 0)
		{
			lastBar = ret->at(ret->size() - 1);
		}

		bool bNewBar = false;
		if (lastBar == NULL || lastBar->time != uBarTime)
		{
			//只要日期和时间都不符,则认为已经是一条新的bar了
			lastBar = new WTSBarStruct();
			bNewBar = true;

			memcpy(lastBar, &curBar, sizeof(WTSBarStruct));
			lastBar->date = uTradingDate;
			lastBar->time = uBarTime;
		}
		else
		{
			bNewBar = false;

			lastBar->high = max(lastBar->high, curBar.high);
			lastBar->low = min(lastBar->low, curBar.low);
			lastBar->close = curBar.close;
			lastBar->settle = curBar.settle;

			lastBar->vol += curBar.vol;
			lastBar->money += curBar.money;
			lastBar->add += curBar.add;
			lastBar->hold = curBar.hold;
		}

		if (bNewBar)
		{
			ret->appendBar(*lastBar);
			delete lastBar;
		}
	}

	//检查最后一条数据
	{
		WTSBarStruct* lastRawBar = baseKline->at(-1);
		WTSBarStruct* lastDesBar = ret->at(-1);
		//如果目标K线的最后一条数据的日期或者时间大于原始K线最后一条的日期或时间
		if (lastDesBar->date > lastRawBar->date || lastDesBar->time > lastRawBar->time)
		{
			if (!bIncludeOpen)
				ret->getDataRef().resize(ret->size() - 1);
			else
				ret->setClosed(false);
		}
	}

	return ret;
}

WTSKlineData* WTSDataFactory::extractDayData(WTSKlineSlice* baseKline, uint32_t times, bool bIncludeOpen /* = true */)
{
	//计算时间步长
	uint32_t steplen = times;

	WTSKlineData* ret = WTSKlineData::create(baseKline->code(), 0);
	ret->setPeriod(KP_DAY, times);

	uint32_t count = 0;
	//WTSKlineData::WTSBarList& bars = baseKline->getDataRef();
	//WTSKlineData::WTSBarList::const_iterator it = bars.begin();
	//for(; it != bars.end(); it++,count++)
	for (auto i = 0; i < baseKline->size(); i++, count++)
	{
		const WTSBarStruct& curBar = *baseKline->at(i);

		uint32_t uDate = curBar.date;

		WTSBarStruct* lastBar = NULL;
		if(ret->size() > 0)
		{
			lastBar = ret->at(ret->size()-1);
		}

		bool bNewBar = false;
		if(lastBar == NULL || count == steplen)
		{
			//只要日期和时间都不符,则认为已经是一条新的bar了
			lastBar = new WTSBarStruct();
			bNewBar = true;

			memcpy(lastBar, &curBar, sizeof(WTSBarStruct));
			lastBar->date = uDate;
			lastBar->time = 0;
			count = 0;
		}
		else
		{
			bNewBar = false;

			lastBar->high = max(lastBar->high, curBar.high);
			lastBar->low = min(lastBar->low, curBar.low);
			lastBar->close = curBar.close;
			lastBar->settle = curBar.settle;

			lastBar->vol += curBar.vol;
			lastBar->money += curBar.money;
			lastBar->add = curBar.add;
			lastBar->hold = curBar.hold;
		}

		if(bNewBar)
		{
			ret->appendBar(*lastBar);
			delete lastBar;
		}
	}

	return ret;
}

WTSKlineData* WTSDataFactory::extractKlineData(WTSTickSlice* ayTicks, uint32_t seconds, 
	WTSSessionInfo* sInfo, bool bUnixTime /* = false */, bool bAlignSec /* = false */)
{
	if(ayTicks == NULL || ayTicks->size() == 0)
		return NULL;
	
	const WTSTickStruct& firstTick = *(ayTicks->at(0));

	if(sInfo == NULL)
		return NULL;

	WTSKlineData* ret = WTSKlineData::create(firstTick.code,0);
	ret->setPeriod(KP_Tick, seconds);
	ret->setUnixTime(bUnixTime);

	for (uint32_t i = 0; i < ayTicks->size(); i++)
	{
		WTSBarStruct* lastBar = NULL;
		if(ret->size() > 0)
		{
			lastBar = ret->at(ret->size()-1);
		}

		const WTSTickStruct* curTick = ayTicks->at(i);
		uint32_t uDate = curTick->trading_date;
		uint32_t curSeconds = sInfo->timeToSeconds(curTick->action_time/1000);
		uint32_t barSeconds = (curSeconds/seconds)*seconds + seconds;
		uint64_t barTime = sInfo->secondsToTime(barSeconds);

		//如果计算出来的K线时间戳小于tick数据的时间戳
		uint32_t actDt = curTick->action_date;
		if (barTime < curTick->action_time / 1000)
		{
			actDt = TimeUtils::getNextDate(actDt);
		}

		if(bUnixTime)
		{
			barTime = (uint64_t)TimeUtils::makeTime(actDt, (long)(barTime * 1000)) / 1000;
		}
		else
		{
			//等价于原来的 actDt*1000000+barTime，提取成命名函数便于全局统一
			barTime = TimeUtils::timeToSecBar(actDt, (uint32_t)barTime);
		}

		bool bNewBar = false;
		if (lastBar == NULL || uDate != lastBar->date || barTime != lastBar->time)
		{
			lastBar = new WTSBarStruct();
			bNewBar = true;

			lastBar->date = uDate;
			lastBar->time = barTime;

			lastBar->open = curTick->price;
			lastBar->high = curTick->price;
			lastBar->low = curTick->price;
			lastBar->close = curTick->price;
			lastBar->vol = curTick->volume;
			lastBar->money = curTick->turn_over;
			lastBar->hold = curTick->open_interest;
			lastBar->add = curTick->diff_interest;
		}
		else
		{
			lastBar->close = curTick->price;
			lastBar->high = max(lastBar->high,curTick->price);
			lastBar->low = min(lastBar->low,curTick->price);
			lastBar->vol += curTick->volume;
			lastBar->money += curTick->turn_over;
			lastBar->hold = curTick->open_interest;
			lastBar->add += curTick->diff_interest;
		}

		if(bNewBar)
		{
			ret->appendBar(*lastBar);
			delete lastBar;
		}
	}

	return ret;
}

bool WTSDataFactory::mergeKlineData(WTSKlineData* klineData, WTSKlineData* newKline)
{
	if (klineData == NULL || newKline == NULL)
		return false;

	if (strcmp(klineData->code(), newKline->code()) != 0)
		return false;

	if (!(klineData->period() == newKline->period() && klineData->times() == newKline->times()))
		return false;

	WTSKlineData::WTSBarList& bars = klineData->getDataRef();
	WTSKlineData::WTSBarList& newBars = newKline->getDataRef();
	if(bars.empty())
	{
		bars.swap(newBars);
		newBars.clear();
		return true;
	}
	else
	{
		uint64_t sTime,eTime;
		if(klineData->period() == KP_DAY)
		{
			sTime = bars[0].date;
			eTime = bars[bars.size() - 1].date;
		}
		else
		{
			sTime = bars[0].time;
			eTime = bars[bars.size() - 1].time;
		}

		WTSKlineData::WTSBarList tempHead, tempTail;
		uint32_t count = newKline->size();
		for (uint32_t i = 0; i < count; i++)
		{
			WTSBarStruct& curBar = newBars[i];

			uint64_t curTime;
			if (klineData->period() == KP_DAY)
				curTime = curBar.date;
			else
				curTime = curBar.time;

			if(curTime < sTime)
			{
				tempHead.emplace_back(curBar);
			}
			else if(curTime > eTime)
			{
				tempTail.emplace_back(curBar);
			}
		}

		bars.insert(bars.begin(), tempHead.begin(), tempHead.end());
		bars.insert(bars.end(), tempTail.begin(), tempTail.end());
	}
	
	return true;
}
