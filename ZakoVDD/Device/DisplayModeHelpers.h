#pragma once

#include "..\Driver.h"

constexpr DISPLAYCONFIG_VIDEO_SIGNAL_INFO dispinfo(UINT32 h, UINT32 v, UINT32 rn, UINT32 rd)
{
	const UINT32 safe_rd = (rd > 0) ? rd : 1;
	const UINT32 h_total = h + 4;
	const UINT32 v_total = v + 4;
	const UINT32 clock_rate = static_cast<UINT32>(static_cast<UINT64>(rn) * h_total * v_total / safe_rd);
	return {
		clock_rate, // pixel clock rate [Hz]
		{clock_rate, h_total}, // fractional horizontal refresh rate [Hz]
		{clock_rate, static_cast<UINT32>(static_cast<UINT64>(h_total) * v_total)}, // fractional vertical refresh rate [Hz]
		{h, v}, // (horizontal, vertical) active pixel resolution
		{h_total, v_total}, // (horizontal, vertical) total pixel resolution
		{{255, 0}}, // video standard and vsync divider
		DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE};
}
