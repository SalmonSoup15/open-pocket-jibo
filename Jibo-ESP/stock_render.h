#pragma once
#include <lvgl.h>
#include "stock_quote.h"

typedef void (*stock_range_cb_t)(StockRange newRange);
void stock_set_range_callback(stock_range_cb_t cb);

lv_obj_t *stock_render(lv_obj_t *parent, const StockQuote &q,
                       StockRange activeRange);

lv_obj_t *stock_render_loading(lv_obj_t *parent, const char *symbol);

lv_obj_t *stock_render_error(lv_obj_t *parent, const char *symbol,
                             const char *msg);
