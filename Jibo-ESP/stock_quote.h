#pragma once
#include <Arduino.h>

// Timeframe ranges for the stock chart.  Each maps to a specific
// Yahoo Finance `range` + `interval` parameter pair.
enum StockRange : uint8_t {
    RANGE_1D  = 0,
    RANGE_1W  = 1,
    RANGE_1M  = 2,
    RANGE_6M  = 3,
    RANGE_YTD = 4,
    RANGE_1Y  = 5,
    RANGE_COUNT
};

const char *stock_range_label(StockRange r);
StockRange  stock_range_from_str(const char *s);

// Max number of points we keep for the line graph.  For longer
// timeframes (YTD, 1Y) at daily resolution, Yahoo returns up to
// ~252 trading-day samples; 260 holds a full year with room to spare.
#define STOCK_GRAPH_POINTS 260

struct StockQuote {
    bool       valid;
    char       symbol[12];
    float      current;
    float      prevClose;
    float      change;
    float      changePercent;
    StockRange range;
    float      points[STOCK_GRAPH_POINTS];
    int        pointsCount;
    char       errorMsg[64];
};

bool stock_start_fetch(const char *symbol, StockRange range = RANGE_1D);
bool stock_is_done();
bool stock_is_busy();
void stock_cancel();
void stock_get_result(StockQuote &out);
