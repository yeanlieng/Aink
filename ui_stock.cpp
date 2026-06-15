#include "ui_stock.h"

#include "app_locale.h"
#include "stock_service.h"
#include "ui_fonts.h"

#include <stdio.h>
#include <string.h>

#define STOCK_ARROW_PX     10
#define STOCK_ROW_Y0       6
#define STOCK_ROW_H        32
#define STOCK_ROW_STEP     33
#define STOCK_CONTENT_W    188

typedef struct {
  lv_obj_t *rowPanel;
  lv_obj_t *arrowCanvas;
  lv_obj_t *nameLabel;
  lv_obj_t *priceLabel;
  lv_obj_t *changeBox;
  lv_obj_t *changeLabel;
  lv_obj_t *divider;
  lv_color_t arrowBuf[STOCK_ARROW_PX * STOCK_ARROW_PX];
} StockRowUi;

static lv_obj_t *s_screenStock = nullptr;
static lv_obj_t *s_emptyLabel = nullptr;
static StockRowUi s_rows[STOCK_MAX_ITEMS];

static void style_label(lv_obj_t *label, const lv_font_t *font) {
  lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(label, 0, LV_PART_MAIN);
  if (font != nullptr) {
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  }
}

static void style_row_panel(lv_obj_t *panel, bool inverted) {
  lv_obj_set_style_bg_color(panel, inverted ? lv_color_black() : lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(panel, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(panel, 0, LV_PART_MAIN);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
}

static void style_change_box(lv_obj_t *box, bool filled) {
  lv_obj_set_style_border_color(box, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(box, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(box, 0, LV_PART_MAIN);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  if (filled) {
    lv_obj_set_style_bg_color(box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
  } else {
    lv_obj_set_style_bg_color(box, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
  }
}

static void style_divider(lv_obj_t *divider) {
  lv_obj_set_size(divider, STOCK_CONTENT_W, 1);
  lv_obj_set_style_bg_color(divider, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(divider, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(divider, 0, LV_PART_MAIN);
  lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
}

static void formatChange(int changePctX10, char *out, size_t outLen) {
  if (changePctX10 == 0) {
    snprintf(out, outLen, "0.0%%");
    return;
  }
  const char sign = changePctX10 > 0 ? '+' : '-';
  const int absVal = abs(changePctX10);
  const int whole = absVal / 10;
  const int frac = absVal % 10;
  if (frac == 0) {
    snprintf(out, outLen, "%c%d.0%%", sign, whole);
  } else {
    snprintf(out, outLen, "%c%d.%d%%", sign, whole, frac);
  }
}

static void canvas_draw_arrow(lv_obj_t *canvas, int changePctX10, bool inverted) {
  const int size = STOCK_ARROW_PX;
  const lv_color_t bg = inverted ? lv_color_black() : lv_color_white();
  const lv_color_t fg = inverted ? lv_color_white() : lv_color_black();

  for (int row = 0; row < size; row++) {
    for (int col = 0; col < size; col++) {
      lv_canvas_set_px(canvas, col, row, bg);
    }
  }

  if (changePctX10 > 0) {
    for (int row = 0; row < size; row++) {
      const int half = (row + 1) / 2;
      const int left = (size / 2) - half;
      const int right = (size / 2) + half;
      for (int col = left; col <= right && col < size; col++) {
        if (col >= 0) {
          lv_canvas_set_px(canvas, col, row, fg);
        }
      }
    }
  } else if (changePctX10 < 0) {
    for (int row = 0; row < size; row++) {
      const int invRow = size - 1 - row;
      const int half = (invRow + 1) / 2;
      const int left = (size / 2) - half;
      const int right = (size / 2) + half;
      for (int col = left; col <= right && col < size; col++) {
        if (col >= 0) {
          lv_canvas_set_px(canvas, col, row, fg);
        }
      }
    }
  } else {
    for (int col = 2; col < size - 2; col++) {
      lv_canvas_set_px(canvas, col, size / 2, fg);
      lv_canvas_set_px(canvas, col, size / 2 - 1, fg);
    }
  }
}

static StockRowUi *create_stock_row(lv_obj_t *parent, int index) {
  StockRowUi *row = &s_rows[index];
  const lv_coord_t y = STOCK_ROW_Y0 + index * STOCK_ROW_STEP;

  row->rowPanel = lv_obj_create(parent);
  lv_obj_set_size(row->rowPanel, STOCK_CONTENT_W, STOCK_ROW_H);
  lv_obj_set_pos(row->rowPanel, 6, y);
  style_row_panel(row->rowPanel, false);

  row->arrowCanvas = lv_canvas_create(row->rowPanel);
  lv_canvas_set_buffer(row->arrowCanvas, row->arrowBuf, STOCK_ARROW_PX, STOCK_ARROW_PX,
                       LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_pos(row->arrowCanvas, 0, (STOCK_ROW_H - STOCK_ARROW_PX) / 2);

  row->nameLabel = lv_label_create(row->rowPanel);
  style_label(row->nameLabel, UI_FONT_SM);
  lv_obj_set_width(row->nameLabel, 62);
  lv_label_set_long_mode(row->nameLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(row->nameLabel, 14, 8);

  row->changeBox = lv_obj_create(row->rowPanel);
  lv_obj_set_size(row->changeBox, 42, 18);
  lv_obj_align(row->changeBox, LV_ALIGN_RIGHT_MID, 0, 0);
  style_change_box(row->changeBox, false);

  row->priceLabel = lv_label_create(row->rowPanel);
  style_label(row->priceLabel, UI_FONT_SM);
  lv_obj_set_width(row->priceLabel, 54);
  lv_label_set_long_mode(row->priceLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(row->priceLabel, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  lv_obj_align(row->priceLabel, LV_ALIGN_RIGHT_MID, -50, 0);

  row->changeLabel = lv_label_create(row->changeBox);
  style_label(row->changeLabel, UI_FONT_SM);
  lv_label_set_long_mode(row->changeLabel, LV_LABEL_LONG_CLIP);
  lv_obj_center(row->changeLabel);

  row->divider = lv_obj_create(parent);
  style_divider(row->divider);
  lv_obj_set_pos(row->divider, 6, y + STOCK_ROW_H);

  return row;
}

static void bind_row(StockRowUi *row, const StockQuote *q, bool showData) {
  char label[STOCK_NAME_LEN];
  char price[20];
  const lv_color_t black = lv_color_black();
  const lv_color_t white = lv_color_white();

  if (row == nullptr || q == nullptr) {
    return;
  }

  stock_service_format_display_label(q, label, sizeof(label));

  if (!showData || !q->quoteValid) {
    style_row_panel(row->rowPanel, false);
    canvas_draw_arrow(row->arrowCanvas, 0, false);
    lv_label_set_text(row->nameLabel, label);
    lv_label_set_text(row->priceLabel, "--");
    lv_label_set_text(row->changeLabel, "--");
    style_change_box(row->changeBox, false);
    lv_obj_set_style_text_color(row->nameLabel, black, LV_PART_MAIN);
    lv_obj_set_style_text_color(row->priceLabel, black, LV_PART_MAIN);
    lv_obj_set_style_text_color(row->changeLabel, black, LV_PART_MAIN);
    return;
  }

  char change[12];
  stock_service_format_price(q, price, sizeof(price));
  formatChange(q->changePctX10, change, sizeof(change));

  const bool up = q->changePctX10 > 0;
  const bool down = q->changePctX10 < 0;

  canvas_draw_arrow(row->arrowCanvas, q->changePctX10, down);
  lv_label_set_text(row->nameLabel, label);
  lv_label_set_text(row->priceLabel, price);
  lv_label_set_text(row->changeLabel, change);

  if (down) {
    style_row_panel(row->rowPanel, true);
    lv_obj_set_style_text_color(row->nameLabel, white, LV_PART_MAIN);
    lv_obj_set_style_text_color(row->priceLabel, white, LV_PART_MAIN);
    style_change_box(row->changeBox, false);
    lv_obj_set_style_bg_color(row->changeBox, black, LV_PART_MAIN);
    lv_obj_set_style_border_width(row->changeBox, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(row->changeLabel, white, LV_PART_MAIN);
  } else if (up) {
    style_row_panel(row->rowPanel, false);
    lv_obj_set_style_text_color(row->nameLabel, black, LV_PART_MAIN);
    lv_obj_set_style_text_color(row->priceLabel, black, LV_PART_MAIN);
    style_change_box(row->changeBox, true);
    lv_obj_set_style_text_color(row->changeLabel, white, LV_PART_MAIN);
  } else {
    style_row_panel(row->rowPanel, false);
    lv_obj_set_style_text_color(row->nameLabel, black, LV_PART_MAIN);
    lv_obj_set_style_text_color(row->priceLabel, black, LV_PART_MAIN);
    style_change_box(row->changeBox, false);
    lv_obj_set_style_text_color(row->changeLabel, black, LV_PART_MAIN);
  }
}

static void bind_stock_data(void) {
  StockSnapshot snap = {};
  stock_service_get_snapshot(&snap);

  const bool hasRows = snap.count > 0;
  const bool hasQuotes = snap.valid;

  if (s_emptyLabel != nullptr) {
    if (!hasRows) {
      lv_obj_clear_flag(s_emptyLabel, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_emptyLabel, LV_OBJ_FLAG_HIDDEN);
    }
  }

  for (int i = 0; i < STOCK_MAX_ITEMS; i++) {
    if (i >= snap.count) {
      lv_obj_add_flag(s_rows[i].rowPanel, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_rows[i].divider, LV_OBJ_FLAG_HIDDEN);
      continue;
    }

    lv_obj_clear_flag(s_rows[i].rowPanel, LV_OBJ_FLAG_HIDDEN);
    bind_row(&s_rows[i], &snap.items[i], hasQuotes);

    if (i < snap.count - 1) {
      lv_obj_clear_flag(s_rows[i].divider, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_rows[i].divider, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void ui_stock_init(void) {
  s_screenStock = lv_obj_create(nullptr);
  lv_obj_set_size(s_screenStock, 200, 180);
  lv_obj_set_style_bg_color(s_screenStock, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_screenStock, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_screenStock, LV_OBJ_FLAG_SCROLLABLE);

  for (int i = 0; i < STOCK_MAX_ITEMS; i++) {
    create_stock_row(s_screenStock, i);
    lv_obj_add_flag(s_rows[i].rowPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rows[i].divider, LV_OBJ_FLAG_HIDDEN);
  }

  s_emptyLabel = lv_label_create(s_screenStock);
  style_label(s_emptyLabel, UI_FONT_SM);
  lv_obj_set_width(s_emptyLabel, 160);
  lv_label_set_long_mode(s_emptyLabel, LV_LABEL_LONG_WRAP);
  lv_label_set_text(s_emptyLabel, app_tr(TR_STOCK_NO_DATA));
  lv_obj_set_style_text_align(s_emptyLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(s_emptyLabel, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(s_emptyLabel, LV_OBJ_FLAG_HIDDEN);

  bind_stock_data();
}

void ui_stock_show(void) {
  stock_service_update(true);
  bind_stock_data();
  lv_scr_load(s_screenStock);
  lv_obj_invalidate(s_screenStock);
}

void ui_stock_refresh(void) {
  if (!ui_stock_is_active()) {
    return;
  }
  if (s_emptyLabel != nullptr) {
    lv_label_set_text(s_emptyLabel, app_tr(TR_STOCK_NO_DATA));
  }
  bind_stock_data();
  lv_obj_invalidate(s_screenStock);
}

bool ui_stock_is_active(void) {
  return s_screenStock != nullptr && lv_scr_act() == s_screenStock;
}

lv_obj_t *ui_stock_get_screen(void) {
  return s_screenStock;
}
