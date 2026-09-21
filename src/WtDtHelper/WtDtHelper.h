/*!
 * \file WtDtPorter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#pragma once

#include "../Includes/WTSTypes.h"

NS_WTP_BEGIN
struct WTSBarStruct;
struct WTSTickStruct;
struct WTSOrdDtlStruct;
struct WTSOrdQueStruct;
struct WTSTransStruct;
NS_WTP_END

USING_NS_WTP;

typedef void(PORTER_FLAG *FuncLogCallback)(WtString message);
typedef void(PORTER_FLAG *FuncGetBarsCallback)(WTSBarStruct* bar, WtUInt32 count, bool isLast);
typedef void(PORTER_FLAG *FuncGetTicksCallback)(WTSTickStruct* tick, WtUInt32 count, bool isLast);
typedef void(PORTER_FLAG *FuncGetOrdDtlCallback)(WTSOrdDtlStruct* item, WtUInt32 count, bool isLast);
typedef void(PORTER_FLAG *FuncGetOrdQueCallback)(WTSOrdQueStruct* item, WtUInt32 count, bool isLast);
typedef void(PORTER_FLAG *FuncGetTransCallback)(WTSTransStruct* item, WtUInt32 count, bool isLast);
typedef void(PORTER_FLAG *FuncCountDataCallback)(WtUInt32 dataCnt);

//改成直接从python传内存块的方式
//typedef bool(PORTER_FLAG *FuncGetBarItem)(WTSBarStruct* curBar,int idx);
//typedef bool(PORTER_FLAG *FuncGetTickItem)(WTSTickStruct* curTick, int idx);

#ifdef __cplusplus
extern "C"
{
#endif
	EXPORT_FLAG	void		dump_bars(WtString binFolder, WtString csvFolder, WtString strFilter = "", FuncLogCallback cbLogger = NULL);
	EXPORT_FLAG	void		dump_ticks(WtString binFolder, WtString csvFolder, WtString strFilter = "", FuncLogCallback cbLogger = NULL);
	EXPORT_FLAG	void		trans_csv_bars(WtString csvFolder, WtString binFolder, WtString period, FuncLogCallback cbLogger = NULL);

	EXPORT_FLAG	WtUInt32	read_dsb_ticks(WtString tickFile, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL);
	EXPORT_FLAG	WtUInt32	read_dsb_order_details(WtString dataFile, FuncGetOrdDtlCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL);
	EXPORT_FLAG	WtUInt32	read_dsb_order_queues(WtString dataFile, FuncGetOrdQueCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL);
	EXPORT_FLAG	WtUInt32	read_dsb_transactions(WtString dataFile, FuncGetTransCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL);

	EXPORT_FLAG	WtUInt32	read_dsb_bars(WtString barFile, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL);

	EXPORT_FLAG	WtUInt32	read_dmb_ticks(WtString tickFile, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL);
	EXPORT_FLAG	WtUInt32	read_dmb_bars(WtString barFile, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL);

	//EXPORT_FLAG bool		trans_bars(WtString barFile, FuncGetBarItem getter, int count, WtString period, FuncLogCallback cbLogger = NULL);
	//EXPORT_FLAG bool		trans_ticks(WtString tickFile, FuncGetTickItem getter, int count, FuncLogCallback cbLogger = NULL);

	EXPORT_FLAG bool		store_bars(WtString barFile, WTSBarStruct* firstBar, int count, WtString period, FuncLogCallback cbLogger = NULL);
	EXPORT_FLAG bool		store_ticks(WtString tickFile, WTSTickStruct* firstTick, int count, FuncLogCallback cbLogger = NULL);

	//股票level2数据存储
	EXPORT_FLAG bool		store_order_details(WtString tickFile, WTSOrdDtlStruct* firstItem, int count, FuncLogCallback cbLogger = NULL);
	EXPORT_FLAG bool		store_order_queues(WtString tickFile, WTSOrdQueStruct* firstItem, int count, FuncLogCallback cbLogger = NULL);
	EXPORT_FLAG bool		store_transactions(WtString tickFile, WTSTransStruct* firstItem, int count, FuncLogCallback cbLogger = NULL);

	EXPORT_FLAG WtUInt32	resample_bars(WtString barFile, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt, 
		WtUInt64 fromTime, WtUInt64 endTime, WtString period, WtUInt32 times, WtString sessInfo, FuncLogCallback cbLogger = NULL, bool bAlignSec = false);

	/*
	 *	历史tick转5秒线
	 *	By 历史tick转sec5 @ 2026.09.21
	 *
	 *	@tickFolder	his/ticks 根目录，目录结构为 <exchg>/<date>/<code>.dsb
	 *	@outFolder	his/sec5 根目录，输出 <exchg>/<code>.dsb
	 *	@commFile	品种配置文件(commodities.json)
	 *	@sessFile	交易时间配置文件(sessions.json)
	 *	@filter		合约或品种白名单，写法同 sec5_codes，如 DCE.jm,SHFE.rb2610，空表示全部
	 *	@sDate		起始交易日(含)，0表示不限
	 *	@eDate		截止交易日(含)，0表示不限
	 *	@options	JSON，如 {"skip_notrade_tick":false,"skip_notrade_bar":false,"minbar_price_mode":0,"overwrite":false}
	 *				前三项必须和datakit的配置一致，否则产出与实盘落盘的对不上；
	 *				overwrite为false时，输出文件里已有的交易日保留原样，只补缺失的交易日
	 *
	 *	返回新写入的bar条数
	 */
	EXPORT_FLAG WtUInt32	trans_ticks_to_sec5(WtString tickFolder, WtString outFolder, WtString commFile, WtString sessFile,
		WtString filter, WtUInt32 sDate, WtUInt32 eDate, WtString options, FuncLogCallback cbLogger = NULL);
#ifdef __cplusplus
}
#endif