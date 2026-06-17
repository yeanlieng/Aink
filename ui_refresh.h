#ifndef UI_REFRESH_H
#define UI_REFRESH_H

enum UiRefreshMode {
  UI_REFRESH_NONE = 0,
  /** 焦点移动：只重绘变化格子 + DisplayPart（最快） */
  UI_REFRESH_FAST,
  /** 相机预览：按 FAST 刷新，但排队时只保留最新帧，不升级为 NAV */
  UI_REFRESH_PREVIEW,
  /** 换页（进/退详情）：整帧 LVGL + DisplayPart（较快） */
  UI_REFRESH_NAV,
  /** 分钟 / 天气变化：整帧 LVGL + 同步基准帧的 DisplayPart */
  UI_REFRESH_QUALITY,
  /** 启动：Init + BaseImage + Init_Partial */
  UI_REFRESH_FULL,
};

#endif
