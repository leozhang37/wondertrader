/*!
 * \file WtDtPorter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "WtDtHelper.h"
#include "../Share/StrUtil.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/BoostFile.hpp"
#include "../Share/Converter.hpp"
#include "../Share/fmtlib.h"
#include "../Share/StdUtils.hpp"

#include "../WtDataStorage/DataDefine.h"
#include "../WTSUtils/WTSCmpHelper.hpp"
#include "../WTSTools/CsvHelper.h"
#include "../WTSTools/WTSDataFactory.h"

#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/decimal.h"
#include "../WTSTools/WTSBaseDataMgr.h"
#include "../WtDataStorage/SecBarBuilder.hpp"

#include <set>
#include <map>
#include <vector>
#include <algorithm>

#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

#include <rapidjson/document.h>
namespace rj = rapidjson;

USING_NS_WTP;

/*
 *	处理块数据
 */
bool proc_block_data(std::string& content, bool isBar, bool bKeepHead /* = true */)
{
	BlockHeader* header = (BlockHeader*)content.data();

	bool bCmped = header->is_compressed();
	bool bOldVer = header->is_old_version();

	//如果既没有压缩，也不是老版本结构体，则直接返回
	if (!bCmped && !bOldVer)
	{
		if (!bKeepHead)
			content.erase(0, BLOCK_HEADER_SIZE);
		return true;
	}

	std::string buffer;
	if (bCmped)
	{
		BlockHeaderV2* blkV2 = (BlockHeaderV2*)content.c_str();

		if (content.size() != (sizeof(BlockHeaderV2) + blkV2->_size))
		{
			return false;
		}

		//将文件头后面的数据进行解压
		buffer = WTSCmpHelper::uncompress_data(content.data() + BLOCK_HEADERV2_SIZE, blkV2->_size);
	}
	else
	{
		if (!bOldVer)
		{
			//如果不是老版本，直接返回
			if (!bKeepHead)
				content.erase(0, BLOCK_HEADER_SIZE);
			return true;
		}
		else
		{
			buffer.append(content.data() + BLOCK_HEADER_SIZE, content.size() - BLOCK_HEADER_SIZE);
		}
	}

	if (bOldVer)
	{
		if (isBar)
		{
			std::string bufV2;
			uint32_t barcnt = buffer.size() / sizeof(WTSBarStructOld);
			bufV2.resize(barcnt * sizeof(WTSBarStruct));
			WTSBarStruct* newBar = (WTSBarStruct*)bufV2.data();
			WTSBarStructOld* oldBar = (WTSBarStructOld*)buffer.data();
			for (uint32_t idx = 0; idx < barcnt; idx++)
			{
				newBar[idx] = oldBar[idx];
			}
			buffer.swap(bufV2);
		}
		else
		{
			uint32_t tick_cnt = buffer.size() / sizeof(WTSTickStructOld);
			std::string bufv2;
			bufv2.resize(sizeof(WTSTickStruct)*tick_cnt);
			WTSTickStruct* newTick = (WTSTickStruct*)bufv2.data();
			WTSTickStructOld* oldTick = (WTSTickStructOld*)buffer.data();
			for (uint32_t i = 0; i < tick_cnt; i++)
			{
				newTick[i] = oldTick[i];
			}
			buffer.swap(bufv2);
		}
	}

	if (bKeepHead)
	{
		content.resize(BLOCK_HEADER_SIZE);
		content.append(buffer);
		header = (BlockHeader*)content.data();
		header->_version = BLOCK_VERSION_RAW_V2;
	}
	else
	{
		content.swap(buffer);
	}

	return true;
}


uint32_t strToTime(const char* strTime, bool bKeepSec = false)
{
	std::string str;
	const char *pos = strTime;
	while (strlen(pos) > 0)
	{
		if (pos[0] != ':')
		{
			str.append(pos, 1);
		}
		pos++;
	}

	uint32_t ret = convert::to_uint32(str.c_str());
	if (ret > 10000 && !bKeepSec)
		ret /= 100;

	return ret;
}

uint32_t strToDate(const char* strDate)
{
	StringVector ay = StrUtil::split(strDate, "/");
	if (ay.size() == 1)
		ay = StrUtil::split(strDate, "-");
	std::stringstream ss;
	if (ay.size() > 1)
	{
		auto pos = ay[2].find(" ");
		if (pos != std::string::npos)
			ay[2] = ay[2].substr(0, pos);
		ss << ay[0] << (ay[1].size() == 1 ? "0" : "") << ay[1] << (ay[2].size() == 1 ? "0" : "") << ay[2];
	}
	else
		ss << ay[0];

	return convert::to_uint32(ss.str().c_str());
}

void dump_bars(WtString binFolder, WtString csvFolder, WtString strFilter /* = "" */, FuncLogCallback cbLogger /* = NULL */)
{
	std::string srcFolder = StrUtil::standardisePath(binFolder);
	if (!StdFile::exists(srcFolder.c_str()))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"目录{}不存在", binFolder));
		return;
	}

	if (!StdFile::exists(csvFolder))
		fs::create_directories(csvFolder);

	fs::path myPath(srcFolder);
	fs::directory_iterator endIter;
	for (fs::directory_iterator iter(myPath); iter != endIter; iter++)
	{
		if (fs::is_directory(iter->path()))
			continue;

		if (iter->path().extension() != ".dsb")
			continue;

		const std::string& path = iter->path().string();

		std::string fileCode = iter->path().stem().string();

		if (cbLogger)
			cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path));

		std::string buffer;
		StdFile::read_file_content(path.c_str(), buffer);
		if (buffer.size() < sizeof(HisKlineBlock))
		{
			if (cbLogger)
				cbLogger(fmtutil::format(u8"文件{}头部校验失败", binFolder));
			continue;
		}

		BlockHeader* bHeader = (BlockHeader*)buffer.data();

		if(bHeader->_type < BT_HIS_Minute1 || bHeader->_type > BT_HIS_Day)
		{
			if (cbLogger)
				cbLogger(fmtutil::format(u8"文件{}不是K线数据，跳过转换", binFolder));
			continue;
		}

		bool isDay = (bHeader->_type == BT_HIS_Day);

		proc_block_data(buffer, true, false);		

		auto kcnt = buffer.size() / sizeof(WTSBarStruct);
		if (kcnt <= 0)
			continue;

		std::string filename = StrUtil::standardisePath(csvFolder);
		filename += fileCode;
		filename += ".csv";

		if (cbLogger)
			cbLogger(fmtutil::format(u8"正在写入{}...", filename));

		WTSBarStruct* bars = (WTSBarStruct*)buffer.data();

		std::stringstream ss;
		ss << "date,time,open,high,low,close,settle,volume,turnover,open_interest,diff_interest" << std::endl;
		ss.setf(std::ios::fixed);

		for (uint32_t i = 0; i < kcnt; i++)
		{
			const WTSBarStruct& curBar = bars[i];
			if(isDay)
			{
				ss << curBar.date << ",0,";
			}
			else
			{
				uint32_t barTime = (uint32_t)(curBar.time % 10000 * 100);
				uint32_t barDate = (uint32_t)(curBar.time / 10000 + 19900000);
				ss << barDate << ","
					<< barTime << ",";
			}
			
			ss << curBar.open << ","
				<< curBar.high << ","
				<< curBar.low << ","
				<< curBar.close << ","
				<< curBar.settle << ","
				<< curBar.vol << ","
				<< curBar.money << ","
				<< curBar.hold << ","
				<< curBar.add << std::endl;
		}

		StdFile::write_file_content(filename.c_str(), ss.str().c_str(), (uint32_t)ss.str().size());

		if (cbLogger)
			cbLogger(fmtutil::format(u8"{}写入完成,共{}条bar", filename, kcnt));
	}

	if (cbLogger)
		cbLogger(fmtutil::format(u8"目录{}全部导出完成...", binFolder));
}

void dump_ticks(WtString binFolder, WtString csvFolder, WtString strFilter /* = "" */, FuncLogCallback cbLogger /* = NULL */)
{
	std::string srcFolder = StrUtil::standardisePath(binFolder);
	if (!StdFile::exists(srcFolder.c_str()))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"目录{}不存在", binFolder));
		return;
	}

	if (!StdFile::exists(csvFolder))
		fs::create_directories(csvFolder);

	fs::path myPath(srcFolder);
	fs::directory_iterator endIter;
	for (fs::directory_iterator iter(myPath); iter != endIter; iter++)
	{
		if (fs::is_directory(iter->path()))
			continue;

		if (iter->path().extension() != ".dsb")
			continue;

		const std::string& path = iter->path().string();

		std::string fileCode = iter->path().stem().string();

		if (cbLogger)
			cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path.c_str()));

		std::string buffer;
		StdFile::read_file_content(path.c_str(), buffer);
		if (buffer.size() < sizeof(HisTickBlock))
		{
			if (cbLogger)
				cbLogger(fmtutil::format(u8"文件{}头部校验失败", binFolder));
			continue;
		}

		proc_block_data(buffer, false, false);

		auto tcnt = buffer.size() / sizeof(WTSTickStruct);
		if (tcnt <= 0)
			continue;

		std::string filename = StrUtil::standardisePath(csvFolder);
		filename += fileCode;
		filename += ".csv";

		if (cbLogger)
			cbLogger(fmtutil::format(u8"正在写入{}...", filename.c_str()));

		WTSTickStruct* ticks = (WTSTickStruct*)buffer.data();

		std::stringstream ss;
		ss.setf(std::ios::fixed, std::ios::floatfield);
		ss.precision(6);
		ss << "exchg,code,tradingdate,actiondate,actiontime,price,open,high,low,settle,preclose,"
			<< "presettle,preinterest,total_volume,total_turnover,open_interest,volume,turnover,additional,";
		for (int i = 0; i < 10; i++)
		{
			bool hasTail = (i != 9);
			ss << "bidprice" << i + 1 << "," << "bidqty" << i + 1 << "," << "askprice" << i + 1 << "," << "askqty" << i + 1 << (hasTail ? "," : "");
		}
		ss << std::endl;

		for (uint32_t i = 0; i < tcnt; i++)
		{
			const WTSTickStruct& curTick = ticks[i];
			ss << curTick.exchg << "," << curTick.code << ","
				<< curTick.trading_date << ","
				<< curTick.action_date << ","
				<< curTick.action_time << ","
				<< curTick.price << ","
				<< curTick.open << ","
				<< curTick.high << ","
				<< curTick.low << ","
				<< curTick.settle_price << ","
				<< curTick.pre_close << ","
				<< curTick.pre_settle << ","
				<< curTick.pre_interest << ","
				<< curTick.total_volume << ","
				<< curTick.total_turnover << ","
				<< curTick.open_interest << ","
				<< curTick.volume << ","
				<< curTick.turn_over << ","
				<< curTick.diff_interest << ",";

			for (int j = 0; j < 10; j++)
			{
				bool hasTail = (j != 9);
				ss << curTick.bid_prices[j] << "," << curTick.bid_qty[j] << "," << curTick.ask_prices[j] << "," << curTick.ask_qty[j] << (hasTail ? "," : "");
			}
			ss << std::endl;
		}

		StdFile::write_file_content(filename.c_str(), ss.str().c_str(), (uint32_t)ss.str().size());

		if (cbLogger)
			cbLogger(fmtutil::format(u8"{}写入完成,共{}条tick数据", filename.c_str(), tcnt));
	}

	if (cbLogger)
		cbLogger(fmtutil::format(u8"目录{}全部导出完成...", binFolder));
}

void trans_csv_bars(WtString csvFolder, WtString binFolder, WtString period, FuncLogCallback cbLogger /* = NULL */)
{
	if (!StdFile::exists(csvFolder))
		return;

	if (!StdFile::exists(binFolder))
		fs::create_directories(binFolder);

	WTSKlinePeriod kp = KP_DAY;
	if (wt_stricmp(period, "m1") == 0)
		kp = KP_Minute1;
	else if (wt_stricmp(period, "m5") == 0)
		kp = KP_Minute5;
	else
		kp = KP_DAY;

	fs::path myPath(csvFolder);
	fs::directory_iterator endIter;
	for (fs::directory_iterator iter(myPath); iter != endIter; iter++)
	{
		if (fs::is_directory(iter->path()))
			continue;

		if (iter->path().extension() != ".csv")
			continue;

		const std::string& path = iter->path().string();

		if(cbLogger)
			cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path.c_str()));

		CsvReader reader(",");
		if(!reader.load_from_file(path.c_str()))
		{
			if (cbLogger)
				cbLogger(fmtutil::format(u8"读取数据文件{}失败...", path.c_str()));
			continue;
		}

		std::vector<WTSBarStruct> bars;

		while(reader.next_row())
		{
			//逐行读取
			WTSBarStruct bs;
			bs.date = strToDate(reader.get_string("date"));
			if(kp != KP_DAY)
				bs.time = TimeUtils::timeToMinBar(bs.date, strToTime(reader.get_string("time")));
			bs.open = reader.get_double("open");
			bs.high = reader.get_double("high");
			bs.low = reader.get_double("low");
			bs.close = reader.get_double("close");
			bs.vol = reader.get_double("volume");
			bs.money = reader.get_double("turnover");
			bs.hold = reader.get_double("open_interest");
			bs.add = reader.get_double("diff_interest");
			bs.settle = reader.get_double("settle");
			bars.emplace_back(bs);

			if (bars.size() % 1000 == 0)
			{
				if (cbLogger)
					cbLogger(fmtutil::format(u8"已读取数据{}条", bars.size()));
			}
		}
		if (cbLogger)
			cbLogger(fmtutil::format(u8"数据文件{}全部读取完成,共{}条", path.c_str(), bars.size()));

		BlockType btype;
		switch (kp)
		{
		case KP_Minute1: btype = BT_HIS_Minute1; break;
		case KP_Minute5: btype = BT_HIS_Minute5; break;
		default: btype = BT_HIS_Day; break;
		}

		HisKlineBlockV2 kBlock;
		strcpy(kBlock._blk_flag, BLK_FLAG);
		kBlock._type = btype;
		kBlock._version = BLOCK_VERSION_CMP_V2;

		std::string cmprsData = WTSCmpHelper::compress_data(bars.data(), sizeof(WTSBarStruct)*bars.size());
		kBlock._size = cmprsData.size();

		std::string filename = StrUtil::standardisePath(binFolder);
		filename += iter->path().stem().string();
		filename += ".dsb";

		BoostFile bf;
		if (bf.create_new_file(filename.c_str()))
		{
			bf.write_file(&kBlock, sizeof(HisKlineBlockV2));
		}
		bf.write_file(cmprsData);
		bf.close_file();
		if (cbLogger)
			cbLogger(fmtutil::format(u8"数据已转储至{}", filename.c_str()));
	}
}

//bool trans_bars(WtString barFile, FuncGetBarItem getter, int count, WtString period, FuncLogCallback cbLogger /* = NULL */)
//{
//	if (count == 0)
//	{
//		if (cbLogger)
//			cbLogger("K线数据条数为0");
//		return false;
//	}
//
//	BlockType bType = BT_HIS_Day;
//	if (wt_stricmp(period, "m1") == 0)
//		bType = BT_HIS_Minute1;
//	else if (wt_stricmp(period, "m5") == 0)
//		bType = BT_HIS_Minute5;
//	else if(wt_stricmp(period, "d") == 0)
//		bType = BT_HIS_Day;
//	else
//	{
//		if (cbLogger)
//			cbLogger("周期只能为m1、m5或d");
//		return false;
//	}
//
//	std::string buffer;
//	buffer.resize(sizeof(WTSBarStruct)*count);
//	WTSBarStruct* bars = (WTSBarStruct*)buffer.c_str();
//	int realCnt = 0;
//	for(int i = 0; i < count; i++)
//	{
//		bool bSucc = getter(&bars[i], i);
//		if (!bSucc)
//			break;
//
//		realCnt++;
//	}
//
//	if (realCnt != count)
//	{
//		buffer.resize(sizeof(WTSBarStruct)*realCnt);
//	}
//
//	if (cbLogger)
//		cbLogger("K线数据已经读取完成，准备写入文件");
//
//	std::string content;
//	content.resize(sizeof(HisKlineBlockV2));
//	HisKlineBlockV2* block = (HisKlineBlockV2*)content.data();
//	strcpy(block->_blk_flag, BLK_FLAG);
//	block->_version = BLOCK_VERSION_CMP;
//	block->_type = bType;
//	std::string cmp_data = WTSCmpHelper::compress_data(bars, buffer.size());
//	block->_size = cmp_data.size();
//	content.append(cmp_data);
//
//	BoostFile bf;
//	if (bf.create_new_file(barFile))
//	{
//		bf.write_file(content);
//	}
//	bf.close_file();
//
//	if (cbLogger)
//		cbLogger("K线数据写入文件成功");
//	return true;
//}
//
//bool trans_ticks(WtString tickFile, FuncGetTickItem getter, int count, FuncLogCallback cbLogger/* = NULL*/)
//{
//	if (count == 0)
//	{
//		if (cbLogger)
//			cbLogger("Tick数据条数为0");
//		return false;
//	}
//
//	std::string buffer;
//	buffer.resize(sizeof(WTSTickStruct)*count);
//	WTSTickStruct* ticks = (WTSTickStruct*)buffer.c_str();
//	int realCnt = 0;
//	for (int i = 0; i < count; i++)
//	{
//		bool bSucc = getter(&ticks[i], i);
//		if (!bSucc)
//			break;
//
//		realCnt++;
//	}
//
//	if(realCnt != count)
//	{
//		buffer.resize(sizeof(WTSTickStruct)*realCnt);
//	}
//
//	if (cbLogger)
//		cbLogger("Tick数据已经读取完成，准备写入文件");
//
//	std::string content;
//	content.resize(sizeof(HisKlineBlockV2));
//	HisKlineBlockV2* block = (HisKlineBlockV2*)content.data();
//	strcpy(block->_blk_flag, BLK_FLAG);
//	block->_version = BLOCK_VERSION_CMP;
//	block->_type = BT_HIS_Ticks;
//	std::string cmp_data = WTSCmpHelper::compress_data(ticks, buffer.size());
//	block->_size = cmp_data.size();
//	content.append(cmp_data);
//
//	BoostFile bf;
//	if (bf.create_new_file(tickFile))
//	{
//		bf.write_file(content);
//	}
//	bf.close_file();
//
//	if (cbLogger)
//		cbLogger("Tick数据写入文件成功");
//
//	return true;
//}

WtUInt32 read_dsb_ticks(WtString tickFile, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger /* = NULL */)
{
	std::string path = tickFile;

	if (cbLogger)
		cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path.c_str()));

	std::string content;
	StdFile::read_file_content(path.c_str(), content);
	if (content.size() < sizeof(HisTickBlock))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}头部校验失败", tickFile));
		return 0;
	}

	proc_block_data(content, false, false);

	if (content.empty())
	{
		cbCnt(0);
		return 0;
	}

	auto tcnt = content.size() / sizeof(WTSTickStruct);

	cbCnt(tcnt);
	cb((WTSTickStruct*)content.data(), tcnt, true);

	if (cbLogger)
		cbLogger(fmtutil::format(u8"{}读取完成,共{}条tick数据", tickFile, tcnt));

	return (WtUInt32)tcnt;
}

WtUInt32 read_dsb_order_details(WtString dataFile, FuncGetOrdDtlCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger/* = NULL*/)
{
	std::string path = dataFile;

	if (cbLogger)
		cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path.c_str()));

	std::string content;
	StdFile::read_file_content(path.c_str(), content);
	if (content.size() < sizeof(HisOrdDtlBlock))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}头部校验失败", dataFile));
		return 0;
	}

	proc_block_data(content, false, false);

	if (content.empty())
	{
		cbCnt(0);
		return 0;
	}

	auto tcnt = content.size() / sizeof(WTSOrdDtlStruct);

	cbCnt(tcnt);
	cb((WTSOrdDtlStruct*)content.data(), tcnt, true);

	if (cbLogger)
		cbLogger(fmtutil::format(u8"{}读取完成,共{}条order detail数据", dataFile, tcnt));

	return (WtUInt32)tcnt;
}

WtUInt32 read_dsb_order_queues(WtString dataFile, FuncGetOrdQueCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger/* = NULL*/)
{
	std::string path = dataFile;

	if (cbLogger)
		cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path.c_str()));

	std::string content;
	StdFile::read_file_content(path.c_str(), content);
	if (content.size() < sizeof(HisOrdQueBlock))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}头部校验失败", dataFile));
		return 0;
	}

	proc_block_data(content, false, false);

	if (content.empty())
	{
		cbCnt(0);
		return 0;
	}

	auto tcnt = content.size() / sizeof(WTSOrdQueStruct);

	cbCnt(tcnt);
	cb((WTSOrdQueStruct*)content.data(), tcnt, true);

	if (cbLogger)
		cbLogger(fmtutil::format(u8"{}读取完成,共{}条order queue数据", dataFile, tcnt));

	return (WtUInt32)tcnt;
}

WtUInt32 read_dsb_transactions(WtString dataFile, FuncGetTransCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger/* = NULL*/)
{
	std::string path = dataFile;

	if (cbLogger)
		cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path.c_str()));

	std::string content;
	StdFile::read_file_content(path.c_str(), content);
	if (content.size() < sizeof(HisTransBlock))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}头部校验失败", dataFile));
		return 0;
	}

	proc_block_data(content, false, false);

	if (content.empty())
	{
		cbCnt(0);
		return 0;
	}

	auto tcnt = content.size() / sizeof(WTSTransStruct);

	cbCnt(tcnt);
	cb((WTSTransStruct*)content.data(), tcnt, true);

	if (cbLogger)
		cbLogger(fmtutil::format(u8"{}读取完成,共{}条transaction数据", dataFile, tcnt));

	return (WtUInt32)tcnt;
}

WtUInt32 read_dsb_bars(WtString barFile, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger )
{
	std::string path = barFile;
	if (cbLogger)
		cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path.c_str()));

	std::string content;
	StdFile::read_file_content(path.c_str(), content);
	if (content.size() < sizeof(HisKlineBlock))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}头部校验失败", barFile));
		return 0;
	}

	proc_block_data(content, true, false);

	if(content.empty())
	{
		cbCnt(0);
		return 0;
	}


	auto kcnt = content.size() / sizeof(WTSBarStruct);
	cbCnt(kcnt);
	cb((WTSBarStruct*)content.data(), kcnt, true);

	if (cbLogger)
		cbLogger(fmtutil::format(u8"{}读取完成,共{}条bar", barFile, kcnt));

	return (WtUInt32)kcnt;
}

WtUInt32 read_dmb_bars(WtString barFile, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger)
{
	std::string path = barFile;

	std::string buffer;
	StdFile::read_file_content(path.c_str(), buffer);
	if (buffer.size() < sizeof(RTKlineBlock))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}头部校验失败", barFile));
		return 0;
	}

	RTKlineBlock* tBlock = (RTKlineBlock*)buffer.c_str();
	auto kcnt = tBlock->_size;
	if (kcnt <= 0)
	{
		cbCnt(0);
		return 0;
	}

	cbCnt(kcnt);
	cb(tBlock->_bars, kcnt, true);

	if (cbLogger)
		cbLogger(fmtutil::format(u8"{}读取完成,共{}条bar", barFile, kcnt));

	return (WtUInt32)kcnt;
}

WtUInt32 read_dmb_ticks(WtString tickFile, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger /* = NULL */)
{
	std::string path = tickFile;

	if (cbLogger)
		cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path.c_str()));

	std::string buffer;
	StdFile::read_file_content(path.c_str(), buffer);
	if (buffer.size() < sizeof(RTTickBlock))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}头部校验失败", tickFile));
		return 0;
	}

	RTTickBlock* tBlock = (RTTickBlock*)buffer.c_str();
	auto tcnt = tBlock->_size;
	if (tcnt <= 0)
	{
		cbCnt(0);
		return 0;
	}

	cbCnt(tcnt);
	cb(tBlock->_ticks, tcnt, true);

	if (cbLogger)
		cbLogger(fmtutil::format(u8"{}读取完成,共{}条tick数据", tickFile, tcnt));

	return (WtUInt32)tcnt;
}


WtUInt32 resample_bars(WtString barFile, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt, WtUInt64 fromTime, WtUInt64 endTime,
	WtString period, WtUInt32 times, WtString sessInfo, FuncLogCallback cbLogger /* = NULL */, bool bAlignSec/* = false*/)
{
	WTSKlinePeriod kp;
	if(wt_stricmp(period, "m1") == 0)
	{
		kp = KP_Minute1;
	}
	else if (wt_stricmp(period, "m5") == 0)
	{
		kp = KP_Minute5;
	}
	else if (wt_stricmp(period, "d") == 0)
	{
		kp = KP_DAY;
	}
	else if (period[0] == 'h')
	{
		kp = KP_Minute5;
		times = PERIOD_TIMES_HOUR;
	}
	else if (period[0] == 'f')
	{
		kp = KP_Minute5;
		times = PERIOD_TIMES_HALF;
	}
	else
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"周期{}不是基础周期...", period));
		return 0;
	}

	bool isDay = (kp == KP_DAY);

	if(isDay)
	{
		if(fromTime >= 100000000 || endTime > 100000000)
		{
			if (cbLogger)
				cbLogger(u8"日线基础数据的开始时间结束时间应为日期，格式如yyyymmdd");
			return 0;
		}
	}
	else
	{
		if (fromTime < 100000000 || endTime < 100000000)
		{
			if (cbLogger)
				cbLogger(u8"分钟线基础数据的开始时间结束时间应为时间，格式如yyyymmddHHMM");
			return 0;
		}
	}

	if(fromTime > endTime)
	{
		std::swap(fromTime, endTime);
	}

	WTSSessionInfo* sInfo = NULL;
	{
		rj::Document root;
		if (root.Parse(sessInfo).HasParseError())
		{
			if (cbLogger)
				cbLogger(u8"交易时间模板解析失败");
			return 0;
		}

		int32_t offset = root["offset"].GetInt();

		sInfo = WTSSessionInfo::create("tmp", "tmp", offset);

		if (!root["auction"].IsNull())
		{
			const rj::Value& jAuc = root["auction"];
			sInfo->setAuctionTime(jAuc["from"].GetUint(), jAuc["to"].GetUint());
		}

		const rj::Value& jSecs = root["sections"];
		if (jSecs.IsNull() || !jSecs.IsArray())
		{
			if (cbLogger)
				cbLogger(u8"交易时间模板格式错误");
			return 0;
		}

		for (const rj::Value& jSec : jSecs.GetArray())
		{
			sInfo->addTradingSection(jSec["from"].GetUint(), jSec["to"].GetUint());
		}
	}

	std::string path = barFile;
	if (cbLogger)
		cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path.c_str()));

	std::string buffer;
	StdFile::read_file_content(path.c_str(), buffer);
	if (buffer.size() < sizeof(HisKlineBlock))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}头部校验失败", barFile));
		return 0;
	}

	proc_block_data(buffer, true, false);

	auto kcnt = buffer.size() / sizeof(WTSBarStruct);
	if (kcnt <= 0)
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"{}数据为空", barFile));
		return 0;
	}

	WTSBarStruct* bars = (WTSBarStruct*)buffer.c_str();

	//确定第一条K线的位置
	WTSBarStruct bar;
	if (isDay)
		bar.date = (uint32_t)fromTime;
	else
	{

		bar.time = fromTime % 100000000 + ((fromTime / 100000000) - 1990) * 100000000;
	}

	WTSBarStruct* pBar = std::lower_bound(bars, bars + (kcnt - 1), bar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
		if (isDay)
			return a.date < b.date;
		else
			return a.time < b.time;
	});


	uint32_t sIdx = (uint32_t)(pBar - bars);
	if((isDay && pBar->date < bar.date) || (!isDay && pBar->time < bar.time))
	{
		//如果返回的K线的时间小于要查找的时间，说明没有符合条件的数据
		if (cbLogger)
			cbLogger(u8"没有找到指定时间范围的K线");
		return 0;
	}
	else if (sIdx != 0 && ((isDay && pBar->date > bar.date) || (!isDay && pBar->time > bar.time)))
	{
		pBar--;
		sIdx--;
	}

	//确定最后一条K线的位置
	if (isDay)
		bar.date = (uint32_t)endTime;
	else
	{

		bar.time = endTime % 100000000 + ((endTime / 100000000) - 1990) * 100000000;
	}
	pBar = std::lower_bound(bars, bars + (kcnt - 1), bar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
		if (isDay)
			return a.date < b.date;
		else
			return a.time < b.time;
	});

	uint32_t eIdx = 0;
	if (pBar == NULL)
		eIdx = kcnt - 1;
	else
		eIdx = (uint32_t)(pBar - bars);

	if (eIdx != 0 && ((isDay && pBar->date > bar.date) || (!isDay && pBar->time > bar.time)))
	{
		pBar--;
		eIdx--;
	}

	uint32_t hitCnt = eIdx - sIdx + 1;
	WTSKlineSlice* slice = WTSKlineSlice::create("", kp, 1, &bars[sIdx], hitCnt);
	WTSDataFactory fact;
	WTSKlineData* kline = fact.extractKlineData(slice, kp, times, sInfo, true, bAlignSec);
	if(kline == NULL)
	{
		if (cbLogger)
			cbLogger(u8"K线重采样失败");
		return 0;
	}

	uint32_t newCnt = kline->size();
	cbCnt(newCnt);
	cb(&kline->getDataRef().at(0),newCnt, true);

	if (cbLogger)
		cbLogger(fmtutil::format(u8"{}重采样完成,共将{}条bar重采样为{}条新bar", barFile, hitCnt, newCnt));

	
	kline->release();
	sInfo->release();
	slice->release();

	return (WtUInt32)newCnt;
}

bool store_bars(WtString barFile, WTSBarStruct* firstBar, int count, WtString period, FuncLogCallback cbLogger /* = NULL */)
{
	if (count == 0)
	{
		if (cbLogger)
			cbLogger(u8"K线数据条数为0");
		return false;
	}

	BlockType bType = BT_HIS_Day;
	if (wt_stricmp(period, "m1") == 0)
		bType = BT_HIS_Minute1;
	else if (wt_stricmp(period, "m5") == 0)
		bType = BT_HIS_Minute5;
	else if (wt_stricmp(period, "d") == 0)
		bType = BT_HIS_Day;
	else
	{
		if (cbLogger)
			cbLogger(u8"周期只能为m1、m5或d");
		return false;
	}

	std::string buffer;
	buffer.resize(sizeof(WTSBarStruct)*count);
	WTSBarStruct* bars = (WTSBarStruct*)buffer.c_str();
	memcpy(bars, firstBar, sizeof(WTSBarStruct)*count);

	if (cbLogger)
		cbLogger(u8"K线数据已经读取完成，准备写入文件");

	std::string content;
	content.resize(sizeof(HisKlineBlockV2));
	HisKlineBlockV2* block = (HisKlineBlockV2*)content.data();
	strcpy(block->_blk_flag, BLK_FLAG);
	block->_version = BLOCK_VERSION_CMP_V2;
	block->_type = bType;
	std::string cmp_data = WTSCmpHelper::compress_data(bars, buffer.size());
	block->_size = cmp_data.size();
	content.append(cmp_data);

	BoostFile bf;
	if (bf.create_new_file(barFile))
	{
		bf.write_file(content);
	}
	bf.close_file();

	if (cbLogger)
		cbLogger(u8"K线数据写入文件成功");
	return true;
}

bool store_ticks(WtString tickFile, WTSTickStruct* firstTick, int count, FuncLogCallback cbLogger/* = NULL*/)
{
	if (count == 0)
	{
		if (cbLogger)
			cbLogger(u8"Tick数据条数为0");
		return false;
	}

	std::string buffer;
	buffer.resize(sizeof(WTSTickStruct)*count);
	WTSTickStruct* ticks = (WTSTickStruct*)buffer.c_str();
	memcpy(ticks, firstTick, sizeof(WTSTickStruct)*count);

	if (cbLogger)
		cbLogger(u8"Tick数据已经读取完成，准备写入文件");

	std::string content;
	content.resize(sizeof(HisTickBlockV2));
	HisTickBlockV2* block = (HisTickBlockV2*)content.data();
	strcpy(block->_blk_flag, BLK_FLAG);
	block->_version = BLOCK_VERSION_CMP_V2;
	block->_type = BT_HIS_Ticks;
	std::string cmp_data = WTSCmpHelper::compress_data(ticks, buffer.size());
	block->_size = cmp_data.size();
	content.append(cmp_data);

	BoostFile bf;
	if (bf.create_new_file(tickFile))
	{
		bf.write_file(content);
	}
	bf.close_file();

	if (cbLogger)
		cbLogger(u8"Tick数据写入文件成功");

	return true;
}

bool store_order_details(WtString tickFile, WTSOrdDtlStruct* firstItem, int count, FuncLogCallback cbLogger/* = NULL*/)
{
	if (count == 0)
	{
		if (cbLogger)
			cbLogger("Size of OrderDetail is 0");
		return false;
	}

	std::string buffer;
	buffer.resize(sizeof(WTSOrdDtlStruct)*count);
	WTSOrdDtlStruct* items = (WTSOrdDtlStruct*)buffer.c_str();
	memcpy(items, firstItem, sizeof(WTSOrdDtlStruct)*count);

	if (cbLogger)
		cbLogger("Reading order details done, prepare to write...");

	std::string content;
	content.resize(sizeof(HisOrdDtlBlockV2));
	HisOrdDtlBlockV2* block = (HisOrdDtlBlockV2*)content.data();
	strcpy(block->_blk_flag, BLK_FLAG);
	block->_version = BLOCK_VERSION_CMP_V2;
	block->_type = BT_HIS_OrdDetail;
	std::string cmp_data = WTSCmpHelper::compress_data(items, buffer.size());
	block->_size = cmp_data.size();
	content.append(cmp_data);

	BoostFile bf;
	if (bf.create_new_file(tickFile))
	{
		bf.write_file(content);
	}
	bf.close_file();

	if (cbLogger)
		cbLogger("Writing order details succeed");

	return true;
}

bool store_order_queues(WtString tickFile, WTSOrdQueStruct* firstItem, int count, FuncLogCallback cbLogger/* = NULL*/)
{
	if (count == 0)
	{
		if (cbLogger)
			cbLogger("Size of order queues is 0");
		return false;
	}

	std::string buffer;
	buffer.resize(sizeof(WTSOrdQueStruct)*count);
	WTSOrdQueStruct* items = (WTSOrdQueStruct*)buffer.c_str();
	memcpy(items, firstItem, sizeof(WTSOrdQueStruct)*count);

	if (cbLogger)
		cbLogger("Reading order queues done, prepare to write...");

	std::string content;
	content.resize(sizeof(HisOrdQueBlockV2));
	HisOrdQueBlockV2* block = (HisOrdQueBlockV2*)content.data();
	strcpy(block->_blk_flag, BLK_FLAG);
	block->_version = BLOCK_VERSION_CMP_V2;
	block->_type = BT_HIS_OrdQueue;
	std::string cmp_data = WTSCmpHelper::compress_data(items, buffer.size());
	block->_size = cmp_data.size();
	content.append(cmp_data);

	BoostFile bf;
	if (bf.create_new_file(tickFile))
	{
		bf.write_file(content);
	}
	bf.close_file();

	if (cbLogger)
		cbLogger("Writing order queues to file succeedd");

	return true;
}

bool store_transactions(WtString tickFile, WTSTransStruct* firstItem, int count, FuncLogCallback cbLogger/* = NULL*/)
{
	if (count == 0)
	{
		if (cbLogger)
			cbLogger("Size of transations is 0");
		return false;
	}

	std::string buffer;
	buffer.resize(sizeof(WTSTransStruct)*count);
	WTSTransStruct* items = (WTSTransStruct*)buffer.c_str();
	memcpy(items, firstItem, sizeof(WTSTransStruct)*count);

	if (cbLogger)
		cbLogger("Reading transactions done, prepare to write...");

	std::string content;
	content.resize(sizeof(HisTransBlockV2));
	HisTransBlockV2* block = (HisTransBlockV2*)content.data();
	strcpy(block->_blk_flag, BLK_FLAG);
	block->_version = BLOCK_VERSION_CMP_V2;
	block->_type = BT_HIS_Trnsctn;
	std::string cmp_data = WTSCmpHelper::compress_data(items, buffer.size());
	block->_size = cmp_data.size();
	content.append(cmp_data);

	BoostFile bf;
	if (bf.create_new_file(tickFile))
	{
		bf.write_file(content);
	}
	bf.close_file();

	if (cbLogger)
		cbLogger("Write transactions to file succeedd");

	return true;
}

/*
 *	历史tick转5秒线
 *	By 历史tick转sec5 @ 2026.09.21
 *
 *	拼接逻辑和实盘落盘共用 SecBarBuilder.hpp，这里只负责：
 *	枚举tick文件、还原落盘时丢掉的新高/新低标记、按交易日合并、写出文件
 */
namespace
{
	inline void sec5_log(FuncLogCallback cbLogger, const std::string& msg)
	{
		if (cbLogger)
			cbLogger(msg.c_str());
	}

	/*
	 *	郑商所3位月份代码转4位，如 AP601 -> AP2601
	 *	十位数按tick所在交易日推断，规则与 CodeHelper::rawMonthCodeToStdCode / CTPLoader 一致：
	 *	合约年份个位 >= 当前年份个位 -> 当前十年，否则 -> 下一个十年
	 */
	std::string czce_full_code(const std::string& code, uint32_t tdate)
	{
		std::string pid = CodeHelper::rawMonthCodeToRawCommID(code.c_str());
		std::string month = code.substr(pid.size());
		if (month.size() != 3 || !std::all_of(month.begin(), month.end(), ::isdigit))
			return code;

		uint32_t curYear = tdate / 10000;
		uint32_t curDecade = (curYear % 100) / 10;
		uint32_t curUnit = curYear % 10;
		uint32_t cUnit = month[0] - '0';
		uint32_t decade = (cUnit >= curUnit) ? curDecade : (curDecade + 1) % 10;

		return pid + (char)('0' + decade) + month;
	}

	bool load_sec5_file(const std::string& filename, std::vector<WTSBarStruct>& bars)
	{
		if (!StdFile::exists(filename.c_str()))
			return false;

		std::string content;
		StdFile::read_file_content(filename.c_str(), content);
		if (content.size() < BLOCK_HEADER_SIZE)
			return false;

		if (!proc_block_data(content, true, false))
			return false;

		const WTSBarStruct* first = (const WTSBarStruct*)content.data();
		bars.assign(first, first + content.size() / sizeof(WTSBarStruct));
		return true;
	}

	/*
	 *	写sec5历史文件，格式与 WtDataWriter 收盘转存的一致：
	 *	12字节的 BlockHeader + BT_HIS_Sec5 + 未压缩(读取侧要mmap和尾部定位)
	 *	先写临时文件再rename，避免中途失败把已有的历史文件弄坏
	 */
	bool save_sec5_file(const std::string& filename, const std::vector<WTSBarStruct>& bars)
	{
		std::string tmpfile = filename + ".tmp";
		{
			BoostFile f;
			if (!f.create_new_file(tmpfile.c_str()))
				return false;

			BlockHeader header;
			memset(&header, 0, sizeof(header));
			strcpy(header._blk_flag, BLK_FLAG);
			header._type = BT_HIS_Sec5;
			header._version = BLOCK_VERSION_RAW_V2;
			f.write_file(&header, sizeof(header));
			if (!bars.empty())
				f.write_file(bars.data(), sizeof(WTSBarStruct)*bars.size());
			f.close_file();
		}

		boost::system::error_code ec;
		fs::rename(tmpfile, filename, ec);
		if (ec)
		{
			fs::remove(tmpfile, ec);
			return false;
		}
		return true;
	}

	/*
	 *	把一个交易日的tick转成5秒线
	 *	新高/新低标记不在落盘的tick里(它是 WTSTickData 的成员)，按 WtDataWriter::updateCache 的规则还原：
	 *	交易日第一笔同时算新高和新低，之后和上一笔的当日累计高低价比较
	 */
	void ticks_to_sec5(const WTSTickStruct* ticks, std::size_t count, WTSSessionInfo* sInfo,
		const sec5::Options& opt, std::vector<WTSBarStruct>& bars)
	{
		const WTSTickStruct* prevTick = NULL;
		for (std::size_t i = 0; i < count; i++)
		{
			const WTSTickStruct& curTick = ticks[i];

			uint32_t limitFlag = 0;
			if (prevTick == NULL)
			{
				limitFlag = sec5::LF_NEW_HIGH | sec5::LF_NEW_LOW;
			}
			else
			{
				if (decimal::gt(curTick.high, prevTick->high))
					limitFlag |= sec5::LF_NEW_HIGH;
				if (decimal::lt(curTick.low, prevTick->low))
					limitFlag |= sec5::LF_NEW_LOW;
			}
			//不进K线的tick在实盘里也更新了缓存，所以这里要先更新 prevTick 再过滤
			prevTick = &curTick;

			if (!sec5::accept_tick(curTick, sInfo, opt._skip_notrade_bar))
				continue;

			WTSBarStruct newBar;
			WTSBarStruct* lastBar = bars.empty() ? NULL : &bars.back();
			if (sec5::update(curTick, limitFlag, sInfo, opt, lastBar, &newBar) == sec5::UR_NEW)
				bars.emplace_back(newBar);
		}
	}

	struct Sec5DayFile
	{
		std::string	_path;
		bool		_is_alt;	//是否是郑商所3位代码的文件
	};

	struct Sec5Task
	{
		std::string	_exchg;
		std::string	_code;		//输出用的代码，郑商所统一为4位
		std::string	_pid;
		std::set<std::string>	_alt_codes;	//出现过的郑商所3位代码
		std::map<uint32_t, Sec5DayFile>	_days;
	};
}

WtUInt32 trans_ticks_to_sec5(WtString tickFolder, WtString outFolder, WtString commFile, WtString sessFile,
	WtString filter, WtUInt32 sDate, WtUInt32 eDate, WtString options, FuncLogCallback cbLogger /* = NULL */)
{
	//1、解析选项
	sec5::Options opt;
	bool bOverwrite = false;
	if (options != NULL && strlen(options) > 0)
	{
		rj::Document root;
		if (root.Parse(options).HasParseError() || !root.IsObject())
		{
			sec5_log(cbLogger, fmtutil::format(u8"选项解析失败: {}", options));
			return 0;
		}

		if (root.HasMember("skip_notrade_tick"))
			opt._skip_notrade_tick = root["skip_notrade_tick"].GetBool();
		if (root.HasMember("skip_notrade_bar"))
			opt._skip_notrade_bar = root["skip_notrade_bar"].GetBool();
		if (root.HasMember("minbar_price_mode"))
			opt._min_price_mode = root["minbar_price_mode"].GetUint();
		if (root.HasMember("overwrite"))
			bOverwrite = root["overwrite"].GetBool();
	}

	//2、加载交易时间和品种，不依赖 contracts.json：已到期的历史合约一般不在当前的合约表里
	if (!StdFile::exists(sessFile) || !StdFile::exists(commFile))
	{
		sec5_log(cbLogger, fmtutil::format(u8"配置文件不存在: {} / {}", sessFile, commFile));
		return 0;
	}

	WTSBaseDataMgr bdMgr;
	if (!bdMgr.loadSessions(sessFile) || !bdMgr.loadCommodities(commFile))
	{
		sec5_log(cbLogger, u8"交易时间或品种配置加载失败");
		return 0;
	}

	wt_hashset<std::string> codeFilter;
	if (filter != NULL && strlen(filter) > 0)
	{
		for (const std::string& item : StrUtil::split(filter, ","))
		{
			std::string c = StrUtil::trim(item.c_str());
			if (!c.empty())
				codeFilter.insert(c);
		}
	}

	std::string tickRoot = StrUtil::standardisePath(tickFolder);
	std::string outRoot = StrUtil::standardisePath(outFolder);
	if (!fs::exists(tickRoot))
	{
		sec5_log(cbLogger, fmtutil::format(u8"tick目录不存在: {}", tickRoot));
		return 0;
	}

	//3、枚举 <exchg>/<date>/<code>.dsb，按合约归组
	std::map<std::string, Sec5Task> tasks;
	for (fs::directory_iterator eit(tickRoot); eit != fs::directory_iterator(); eit++)
	{
		if (!fs::is_directory(eit->path()))
			continue;

		std::string exchg = eit->path().filename().string();
		for (fs::directory_iterator dit(eit->path()); dit != fs::directory_iterator(); dit++)
		{
			std::string dname = dit->path().filename().string();
			if (!fs::is_directory(dit->path()) || dname.size() != 8 || !std::all_of(dname.begin(), dname.end(), ::isdigit))
				continue;

			uint32_t uDate = strtoul(dname.c_str(), NULL, 10);
			if ((sDate != 0 && uDate < sDate) || (eDate != 0 && uDate > eDate))
				continue;

			for (fs::directory_iterator fit(dit->path()); fit != fs::directory_iterator(); fit++)
			{
				if (!fs::is_regular_file(fit->path()) || fit->path().extension().string() != ".dsb")
					continue;

				std::string rawCode = fit->path().stem().string();
				std::string pid = CodeHelper::rawMonthCodeToRawCommID(rawCode.c_str());
				//只处理分月合约，品种代码后面必须跟着月份
				if (pid.empty() || pid.size() == rawCode.size())
					continue;

				std::string code = (exchg == "CZCE") ? czce_full_code(rawCode, uDate) : rawCode;
				bool isAlt = (code != rawCode);

				std::string fullCode = exchg + "." + code;
				std::string fullPid = exchg + "." + pid;
				bool bMatch = sec5::code_match(codeFilter, fullCode.c_str(), fullPid.c_str());
				if (!bMatch && isAlt)
					bMatch = codeFilter.find(exchg + "." + rawCode) != codeFilter.end();
				if (!bMatch)
					continue;

				Sec5Task& task = tasks[fullCode];
				task._exchg = exchg;
				task._code = code;
				task._pid = pid;
				if (isAlt)
					task._alt_codes.insert(rawCode);

				auto it = task._days.find(uDate);
				if (it != task._days.end())
				{
					//同一天新老代码的文件都有，以新代码的为准
					if (isAlt)
						continue;
					sec5_log(cbLogger, fmtutil::format(u8"{} 在 {} 同时存在新老代码的tick文件，使用 {}", fullCode, uDate, fit->path().string()));
				}
				task._days[uDate] = { fit->path().string(), isAlt };
			}
		}
	}

	if (tasks.empty())
	{
		sec5_log(cbLogger, u8"没有找到符合条件的tick文件");
		return 0;
	}

	//4、逐合约转换
	WtUInt32 total = 0;
	for (auto& item : tasks)
	{
		const std::string& fullCode = item.first;
		Sec5Task& task = item.second;

		WTSCommodityInfo* commInfo = bdMgr.getCommodity(task._exchg.c_str(), task._pid.c_str());
		if (commInfo == NULL || commInfo->getSessionInfo() == NULL)
		{
			sec5_log(cbLogger, fmtutil::format(u8"品种 {}.{} 没有配置或没有交易时间，跳过 {}", task._exchg, task._pid, fullCode));
			continue;
		}
		WTSSessionInfo* sInfo = commInfo->getSessionInfo();

		std::string outDir = outRoot + task._exchg + "/";
		fs::create_directories(outDir);
		std::string filename = outDir + task._code + ".dsb";

		//已有的数据：新代码文件优先，郑商所老代码文件里有而新文件里没有的交易日也并进来
		std::vector<WTSBarStruct> oldBars;
		load_sec5_file(filename, oldBars);

		std::vector<std::string> altFiles;
		for (const std::string& alt : task._alt_codes)
		{
			std::string altfile = outDir + alt + ".dsb";
			std::vector<WTSBarStruct> altBars;
			if (!load_sec5_file(altfile, altBars))
				continue;

			altFiles.emplace_back(altfile);
			std::set<uint32_t> dates;
			for (const WTSBarStruct& bar : oldBars)
				dates.insert(bar.date);
			for (const WTSBarStruct& bar : altBars)
			{
				if (dates.find(bar.date) == dates.end())
					oldBars.emplace_back(bar);
			}
		}

		std::set<uint32_t> oldDates;
		for (const WTSBarStruct& bar : oldBars)
			oldDates.insert(bar.date);

		std::vector<WTSBarStruct> newBars;
		std::set<uint32_t> doneDates;
		uint32_t skipped = 0;
		for (auto& day : task._days)
		{
			uint32_t uDate = day.first;
			if (!bOverwrite && oldDates.find(uDate) != oldDates.end())
			{
				skipped++;
				continue;
			}

			std::string content;
			StdFile::read_file_content(day.second._path.c_str(), content);
			if (content.size() < sizeof(HisTickBlock) || !proc_block_data(content, false, false))
			{
				sec5_log(cbLogger, fmtutil::format(u8"tick文件 {} 校验失败，跳过", day.second._path));
				continue;
			}

			std::vector<WTSBarStruct> dayBars;
			ticks_to_sec5((const WTSTickStruct*)content.data(), content.size() / sizeof(WTSTickStruct), sInfo, opt, dayBars);
			newBars.insert(newBars.end(), dayBars.begin(), dayBars.end());
			doneDates.insert(uDate);
		}

		if (doneDates.empty() && altFiles.empty())
		{
			sec5_log(cbLogger, fmtutil::format(u8"{} 没有需要转换的交易日(跳过已有的 {} 天)", fullCode, skipped));
			continue;
		}

		//5、合并：去掉被覆盖的交易日，按时间排序，同一时间戳以新算的为准
		std::vector<WTSBarStruct> allBars;
		allBars.reserve(oldBars.size() + newBars.size());
		for (const WTSBarStruct& bar : oldBars)
		{
			if (doneDates.find(bar.date) == doneDates.end())
				allBars.emplace_back(bar);
		}
		allBars.insert(allBars.end(), newBars.begin(), newBars.end());
		std::stable_sort(allBars.begin(), allBars.end(), [](const WTSBarStruct& a, const WTSBarStruct& b) {
			return a.time < b.time;
		});

		std::vector<WTSBarStruct> finalBars;
		finalBars.reserve(allBars.size());
		for (const WTSBarStruct& bar : allBars)
		{
			if (!finalBars.empty() && finalBars.back().time == bar.time)
				finalBars.back() = bar;
			else
				finalBars.emplace_back(bar);
		}

		if (!save_sec5_file(filename, finalBars))
		{
			sec5_log(cbLogger, fmtutil::format(u8"写入 {} 失败", filename));
			continue;
		}

		//和 WtDataWriter 一样，郑商所老代码的文件并入新代码文件后就不再保留
		for (const std::string& altfile : altFiles)
		{
			boost::system::error_code ec;
			fs::remove(altfile, ec);
		}

		total += (WtUInt32)newBars.size();
		sec5_log(cbLogger, fmtutil::format(u8"{} 转换完成：{} 个交易日，新增 {} 条5秒线，跳过已有的 {} 天，文件共 {} 条",
			fullCode, doneDates.size(), newBars.size(), skipped, finalBars.size()));
	}

	return total;
}
