// ============================================================================
// mind.h — AMPLIFY MIND: локальный ИИ-ассистент ОС (этапы 49, 49.1).
// ----------------------------------------------------------------------------
// Слои (снизу вверх):
//   mind_gguf   — разбор GGUF-файла модели;
//   mind_math   — матричные ядра (Q8_0/Q4_0/F16/F32, SSSE3), пул AP;
//   mind_model  — трансформер llama / qwen3, KV-кэш, пакетный prefill;
//   mind_tok    — токенизаторы SPM / GPT2-BPE (gpt2, smollm, qwen2);
//   mind_chat   — шаблоны диалога (ChatML, Qwen3), <tool_call>;
//   mind        — ЭТОТ модуль: загрузка модели, диалог (ChatML), генерация
//                 на ядре-дирижёре, сэмплер, журнал реплик, роутер
//                 инструментов;
//   mind_tools  — доступ к API ОС: процессы, VFS, журнал ядра, exec;
//   mind_widget — оверлей Pearl по Ctrl+Space; shell-команда `muse`.
//
// Потоки: всё API ниже вызывается ТОЛЬКО на BSP (из главного цикла WM или
// shell). Сама генерация идёт на AP (smp_pool_spawn); токены приходят через
// SPSC-кольцо, mind_poll() каждый кадр переносит их в журнал реплик.
// ============================================================================
#ifndef AOS_X86_64_MIND_H
#define AOS_X86_64_MIND_H

#include <stdint.h>
#include <stdbool.h>

#define MIND_MODEL_DIR      "/disk/vault/mind"
#define MIND_CONF_PATH      "/disk/Sanctum/mind.conf"
#define MIND_BUILTIN_MODEL  "stories260K.gguf"      // в initrd — запасная модель
#define MIND_CD_PREFIX      "/cdrom/"                // файлы установочного носителя (ISO9660)

typedef enum {
    MIND_ST_EMPTY = 0,    // модель не загружена
    MIND_ST_LOADING,      // идёт чтение весов с диска
    MIND_ST_READY,        // ждёт вопроса
    MIND_ST_BUSY,         // prefill / генерация
    MIND_ST_CONFIRM,      // модель просит exec — ждём решения пользователя
    MIND_ST_ERROR
} mind_state_t;

typedef enum {
    MIND_ROLE_USER = 0,
    MIND_ROLE_MIND,
    MIND_ROLE_TOOL,
    MIND_ROLE_INFO
} mind_role_t;

// ---- жизненный цикл ------------------------------------------------------------
void         mind_init(void);                 // на загрузке: CPU-фичи, без модели
int          mind_load_begin(const char* path); // path=0 — из mind.conf / автопоиск
int          mind_load_sync(const char* path);  // для shell и smoke: ждать до конца
void         mind_unload(void);
mind_state_t mind_state(void);
const char*  mind_error(void);
int          mind_load_progress(void);        // 0..100 во время LOADING
bool         mind_is_chat(void);              // есть ChatML-разметка
bool         mind_warming(void);              // Qwen3: считается KV системного промпта
bool         mind_think_mode(void);           // Qwen3: think=1 в mind.conf

// «SmolLM2-135M-Instruct-Q8_0 · 135M · Q8_0 · ctx 120/1024 · 43 tok/s»
void         mind_status(char* out, int cap);
const char*  mind_model_label(void);

// ---- диалог ---------------------------------------------------------------------
// Текст пользователя: вопрос или команда (/ps /ls /read /logs /exec /new /help).
int          mind_submit(const char* text);
void         mind_poll(void);                 // каждый кадр (BSP)
void         mind_set_ui_busy(bool busy);     // [48.2] идёт анимация: чтение модели подождёт
void         mind_cancel(void);
void         mind_confirm(bool allow);
const char*  mind_pending_exec(void);         // команда, ждущая подтверждения
void         mind_new_chat(void);

int          mind_msg_count(void);
mind_role_t  mind_msg_role(int i);
const char*  mind_msg_text(int i);            // UTF-8
uint32_t     mind_version(void);              // растёт при любом изменении журнала
// Всего реплик создано (монотонно; журнал хранит последние 40). Индекс
// реплики с номером s: s - (mind_msg_total() - mind_msg_count()).
uint32_t     mind_msg_total(void);

// ---- параметры генерации ----------------------------------------------------------
void         mind_set_greedy(bool greedy);    // для тестов: детерминированный вывод
int          mind_last_gen_tokens(void);
int          mind_last_tok_per_s10(void);     // tok/s × 10 последней генерации
int          mind_last_prefill_per_s10(void);

// ---- самопроверка (smoke 49) и замер ----------------------------------------------------
bool         mind_self_test(void);
// Greedy-генерация n токенов из «Once upon a time»; tok/s ×10 генерации и prefill.
int          mind_bench(int n, int* gen_tps10, int* prefill_tps10);
// mind.conf bench_on_boot=N: загрузить модель, замерить, выгрузить (serial).
void         mind_boot_bench(void);

// ---- инструменты (mind_tools.c) -----------------------------------------------------
// Результат — текст UTF-8 в out (NUL-терминирован), возврат — длина или -1.
int          mind_tool_run(const char* name, const char* args, char* out, int cap);
bool         mind_tool_known(const char* name);
bool         mind_tool_needs_confirm(const char* name);

// Перекодировки: родная кодировка строк ядра — CP1251, модели — UTF-8.
int          mind_to_utf8(const char* in, int n, char* out, int cap);
int          mind_to_cp1251(const char* in, int n, char* out, int cap);

// [49.1] Точный короткий ответ по данным ОС (шаблон, без модели) на вопрос q
// (UTF-8) про процессы/память/ядра/окна/время работы: markdown-список
// в out (UTF-8, на языке вопроса). 0 — вопрос не про это.
int          mind_facts(const char* q, char* out, int cap);
// CP1251: к нижнему регистру (латиница и кириллица) на месте.
void         mind_lower_cp1251(char* s, int n);
// Есть ли подстрока stem (CP1251, нижний регистр) в s — для русских основ
// слов в роутере («памят» найдёт «памяти», «память», «памятью»).
bool         mind_has_stem(const char* s, const char* stem);

#endif
