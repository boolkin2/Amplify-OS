// ============================================================================
// mind_chat.h — шаблоны диалога AMPLIFY MIND (этап 49.1).
// ----------------------------------------------------------------------------
// Общий код ядра и хост-сборки (tools/mind_host.c): промпт строится ОДНИМ
// кодом, а тест сверяет его ids с официальным jinja-шаблоном модели
// (tools/mind_ref.py render). Здесь же — описание инструментов в формате
// Qwen3 (<tools>…</tools> с JSON-схемами) и разбор ответа модели
// <tool_call>{"name": …, "arguments": {…}}</tool_call>.
//
// Сборщик промпта: текст между управляющими токенами копится в буфере и
// кодируется ОДНИМ куском (с распознаванием USER_DEFINED-токенов вроде
// <think>) — ровно так HF/llama.cpp токенизируют отрендеренный шаблон, где
// <|im_start|>/<|im_end|> — границы кусков. Управляющие токены добавляются
// только по id (mind_prompt_tok), из текста их не получить.
//
// Стили:
//   MIND_CHAT_CHATML — ChatML без размышлений (SmolLM2, этап 49);
//   MIND_CHAT_QWEN3  — ChatML Qwen3: в начало ответа ассистента кладётся
//                      пустой блок "<think>\n\n</think>\n\n" (как
//                      enable_thinking=False); think=1 — не кладётся, модель
//                      рассуждает сама.
// Без libc и без аллокаций — годится и для ядра, и для хоста.
// ============================================================================
#ifndef AOS_X86_64_MIND_CHAT_H
#define AOS_X86_64_MIND_CHAT_H

#include <stdint.h>
#include <stdbool.h>
#include "mind_tok.h"

enum { MIND_CHAT_NONE = 0, MIND_CHAT_CHATML, MIND_CHAT_QWEN3 };

// Инструменты (биты маски — в порядке описания).
enum { MIND_TOOL_PS = 0, MIND_TOOL_LS, MIND_TOOL_READ, MIND_TOOL_LOGS, MIND_TOOL_EXEC, MIND_TOOL_COUNT };
#define MIND_TOOLS_ALL ((1u << MIND_TOOL_COUNT) - 1)

typedef struct {
    const mind_tok_t* tok;
    int32_t* ids;
    int      n, cap;
    char*    text;           // накопленный текст (UTF-8) до следующего спецтокена
    int      tn, tcap;
    bool     ok;
} mind_prompt_t;

void mind_prompt_begin(mind_prompt_t* p, const mind_tok_t* t, int32_t* ids, int cap,
                       char* textbuf, int tcap);
void mind_prompt_text(mind_prompt_t* p, const char* s);
void mind_prompt_textn(mind_prompt_t* p, const char* s, int n);
void mind_prompt_tok(mind_prompt_t* p, int id);        // сбросить текст, добавить токен
int  mind_prompt_end(mind_prompt_t* p);                // сбросить текст; число ids или -1

// ---- куски шаблона -------------------------------------------------------------
// <|im_start|>system\n{system}[\n\n{блок инструментов}]<|im_end|>\n
void mind_chat_system(mind_prompt_t* p, int style, const char* system, unsigned tool_mask);
// <|im_start|>user\n{text}<|im_end|>\n
void mind_chat_user(mind_prompt_t* p, const char* text);
// <|im_start|>user\n<tool_response>\n{out}\n</tool_response><|im_end|>\n  (Qwen3)
void mind_chat_tool_response(mind_prompt_t* p, const char* out, int n);
// <|im_start|>assistant\n[<think>\n\n</think>\n\n]{prefix}
void mind_chat_assistant(mind_prompt_t* p, int style, bool think, const char* prefix);
// Закрыть прошлую реплику ассистента, которая уже в KV-кэше: pending —
// последний сэмплированный, но не прогнанный токен (или -1). Если это не
// <|im_end|>, дописывается <|im_end|>. Затем "\n".
void mind_chat_close(mind_prompt_t* p, int pending);
// Готовый вызов инструмента от имени ассистента (роутер для Qwen3):
// <tool_call>\n{"name": "ls", "arguments": {"path": "/disk"}}\n</tool_call>
void mind_chat_tool_call_text(mind_prompt_t* p, int tool, const char* arg);

// Блок "# Tools ... </tool_call>" из шаблона Qwen3 (для проверки и отладки).
int  mind_chat_tools_block(char* out, int cap, unsigned tool_mask);
const char* mind_tool_name(int tool);
int  mind_tool_index(const char* name);                // -1 если неизвестно
unsigned mind_tool_mask_parse(const char* list);       // "ps,ls,read" -> маска

// ---- разбор ответа модели ------------------------------------------------------
// Последний <tool_call>{json}</tool_call> в тексте (UTF-8). name — имя
// инструмента, args — значение path/command (или пусто). true — найден.
bool mind_parse_tool_call_json(const char* s, char* name, int ncap, char* args, int acap);
// Старый формат этапа 49: <tool>name args</tool>.
bool mind_parse_tool_call_tag(const char* s, char* name, int ncap, char* args, int acap);

#endif
