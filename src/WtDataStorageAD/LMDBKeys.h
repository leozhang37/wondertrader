#pragma once
#include <stdint.h>
#include <string.h>
#include "../Includes/WTSMarcos.h"

static uint16_t reverseEndian(uint16_t src)
{
	uint16_t up = (src & 0x00FF) << 8;
	uint16_t low = (src & 0xFF00) >> 8;
	return up + low;
}

static uint32_t reverseEndian(uint32_t src)
{
	uint32_t x = (src & 0x000000FF) << 24;
	uint32_t y = (src & 0x0000FF00) << 8;
	uint32_t z = (src & 0x00FF0000) >> 8;
	uint32_t w = (src & 0xFF000000) >> 24;
	return x + y + z + w;
}

/*
 *	By 秒K线支持 @ 2026.09.20
 *	秒线的bar时间戳是 yyyyMMddHHmmss(约2e13)，放不进uint32，
 *	所以需要一个uint64的字节序翻转。
 *	翻转的目的和上面两个一样：LMDB的key是按字节比较的，
 *	转成大端之后 memcmp 的顺序才等于数值顺序，范围查询才正确
 */
static uint64_t reverseEndian(uint64_t src)
{
	return ((src & 0x00000000000000FFULL) << 56)
		| ((src & 0x000000000000FF00ULL) << 40)
		| ((src & 0x0000000000FF0000ULL) << 24)
		| ((src & 0x00000000FF000000ULL) << 8)
		| ((src & 0x000000FF00000000ULL) >> 8)
		| ((src & 0x0000FF0000000000ULL) >> 24)
		| ((src & 0x00FF000000000000ULL) >> 40)
		| ((src & 0xFF00000000000000ULL) >> 56);
}

#pragma pack(push, 1)
typedef struct _LMDBHftKey
{
	char		_exchg[MAX_EXCHANGE_LENGTH];
	char		_code[MAX_INSTRUMENT_LENGTH];
	uint32_t	_date;
	uint32_t	_time;

	_LMDBHftKey(const char* exchg, const char* code, uint32_t date, uint32_t time)
	{
		memset(this, 0, sizeof(_LMDBHftKey));
		strcpy(_exchg, exchg);
		strcpy(_code, code);
		_date = reverseEndian(date);
		_time = reverseEndian(time);
	}
} LMDBHftKey;

typedef struct  _LMDBBarKey
{
public:
	char		_exchg[MAX_EXCHANGE_LENGTH];
	char		_code[MAX_INSTRUMENT_LENGTH];
	uint32_t	_bartime;

	_LMDBBarKey(const char* exchg, const char* code, uint32_t bartime)
	{
		memset(this, 0, sizeof(_LMDBBarKey));
		strcpy(_exchg, exchg);
		strcpy(_code, code);
		_bartime = reverseEndian(bartime);
	}
} LMDBBarKey;

/*
 *	秒线的key
 *	By 秒K线支持 @ 2026.09.20
 *
 *	不能复用 LMDBBarKey：它的 _bartime 是 uint32_t，
 *	而分钟线的时间戳((date-19900000)*10000+HHMM，约3.5e8)刚好塞得进去，
 *	秒线的 yyyyMMddHHmmss 约2e13，截断成uint32会把高位丢掉，
 *	不同日期的同一时刻会撞成同一个key。
 *	单独用一个uint64的key结构，和分钟线的库各存各的
 */
typedef struct _LMDBSecBarKey
{
public:
	char		_exchg[MAX_EXCHANGE_LENGTH];
	char		_code[MAX_INSTRUMENT_LENGTH];
	uint64_t	_bartime;

	_LMDBSecBarKey(const char* exchg, const char* code, uint64_t bartime)
	{
		memset(this, 0, sizeof(_LMDBSecBarKey));
		strcpy(_exchg, exchg);
		strcpy(_code, code);
		_bartime = reverseEndian(bartime);
	}
} LMDBSecBarKey;
#pragma pack(pop)