#ifndef BOOKOFANSWERS_H
#define BOOKOFANSWERS_H

#include <Arduino.h>
#include <esp_random.h>
#include <time.h>

static const char *kBookAnswers[] = {
    "是的", "不是", "再等等", "现在行动", "保持耐心",
    "相信自己", "问问内心", "向前走", "停下来", "换个方向",
    "今日宜", "明日不宜", "机会来了", "静待花开", "放手一搏",
    "谨慎为上", "相信直觉", "听听他人", "大胆去做", "别担心",
    "会实现的", "需要休息", "继续努力", "已经足够", "再试一次",
    "放手过去", "迎接变化", "坚持到底", "接受现实", "调整计划",
    "保持希望", "不必强求", "顺其自然", "该放下了", "重新开始",
    "别太急躁", "时机未到", "马上成功", "前景光明", "谨慎乐观",
    "值得冒险", "避开争执", "专注当下", "积累力量", "等待呼唤",
    "心诚则灵", "一切皆有可能", "命运在你手中", "倾听内心声音", "答案就在眼前",
    "勇敢前行", "三思而行", "跟随内心", "顺势而为", "果断决策",
    "放轻松", "继续前进", "保持平衡", "接纳自己", "放眼未来",
    "小步快跑", "稳中求胜", "以柔克刚", "随机应变", "厚积薄发",
    "量力而行", "尽力而为", "保持低调", "高调做事", "低调做人",
    "不忘初心", "方得始终", "天道酬勤", "勤能补拙", "宁静致远",
    "知足常乐", "随遇而安", "心平气和", "正念当下", "活在当下",
    "行胜于言", "言出必行", "真诚以待", "将心比心", "感恩所有",
    "珍惜眼前", "放下执念", "宽以待人", "严于律己", "修身养性",
    "厚德载物", "上善若水", "大智若愚", "难得糊涂", "知难而进",
    "迎难而上", "百折不挠", "勇往直前", "意气风发", "志存高远",
};

#define BOOK_ANSWERS_COUNT (sizeof(kBookAnswers) / sizeof(kBookAnswers[0]))

static inline const char *bookofanswers_random(void) {
    return kBookAnswers[esp_random() % BOOK_ANSWERS_COUNT];
}

static inline const char *bookofanswers_random_timed(void) {
    uint32_t mix = esp_random();
    mix ^= (uint32_t)time(nullptr);
    mix ^= (uint32_t)millis();
    mix ^= mix << 13;
    mix ^= mix >> 17;
    mix ^= mix << 5;
    return kBookAnswers[mix % BOOK_ANSWERS_COUNT];
}

#endif
