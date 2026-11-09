// ============================================================================
// mind_tok.h — токенизаторы AMPLIFY MIND (этапы 49, 49.1).
// ----------------------------------------------------------------------------
// Словарь берётся из метаданных GGUF (tokenizer.ggml.*). Два семейства:
//   * "llama" — SentencePiece: пробел -> U+2581, жадное слияние соседних
//     символов с максимальным score, байтовый fallback <0xXX>;
//   * "gpt2"  — byte-level BPE: претокенизация регуляркой по
//     tokenizer.ggml.pre (gpt2; smollm — цифры по одной; qwen2 — регулярка
//     Qwen2, см. mind_tok.c), байты -> символы bytes_to_unicode, слияния по
//     рангу из tokenizer.ggml.merges.
// Логика повторяет llama.cpp (llama-vocab.cpp, unicode.cpp) и HF tokenizers,
// поэтому ids совпадают с эталоном tools/mind_ref.py (и с tokenizer.json).
//
// Спецтокены в тексте (этап 49.1):
//   * USER_DEFINED (тип 4: <tool_call>, </tool_call>, <think>, </think>,
//     <tool_response>...) распознаются в тексте ВСЕГДА — так делают
//     llama.cpp и HF, и именно так их видела модель при обучении;
//   * CONTROL (тип 3: <|im_start|>, <|im_end|>...) — только с флагом
//     MIND_TOK_PARSE_CONTROL (доверенный текст шаблона). В пользовательском
//     тексте, файлах и журнале — никогда: текст не может «закрыть» реплику и
//     выдать себя за систему.
// ============================================================================
#ifndef AOS_X86_64_MIND_TOK_H
#define AOS_X86_64_MIND_TOK_H

#include <stdint.h>
#include <stdbool.h>
#include "mind_gguf.h"

enum { MIND_TOK_SPM = 1, MIND_TOK_BPE = 2 };
enum { MIND_PRE_GPT2 = 0, MIND_PRE_SMOLLM, MIND_PRE_QWEN2 };

#define MIND_TOK_PARSE_CONTROL 1
#define MIND_TOK_MAX_SPECIAL   64

typedef struct {
    int         kind;
    int         pre;           // MIND_PRE_* (для BPE)
    int         n_vocab;
    mind_str_t* pieces;        // строки токенов (указатели в образ GGUF)
    float*      scores;        // SPM
    int32_t*    types;         // 1 normal, 2 unknown, 3 control, 4 user, 5 unused, 6 byte
    int32_t*    hash;          // открытая адресация: строка -> id
    uint32_t    hash_mask;
    uint64_t*   mkey;          // BPE: (left<<32 | right)
    int32_t*    mrank;
    int32_t*    mres;
    uint32_t    mmask;
    int         byte_tok[256]; // id токена для байта
    int16_t     cp_to_byte[512];
    bool        add_bos, add_space_prefix;
    int         bos, eos, unk;
    int         im_start, im_end, endoftext;
    int         think_open, think_close;             // <think> </think> (-1 если нет)
    int         tool_call_open, tool_call_close;     // <tool_call> </tool_call>
    // Спецтокены для разбиения текста — по убыванию длины строки.
    int32_t     special[MIND_TOK_MAX_SPECIAL];
    int         n_special;
    uint8_t     special_first[32];                   // битовая маска первых байтов
} mind_tok_t;

int  mind_tok_init(mind_tok_t* t, const gguf_file_t* g, char* err, int errlen);
void mind_tok_free(mind_tok_t* t);

// Текст (UTF-8) -> ids. Возвращает число токенов или -1 (не влезло в max).
// USER_DEFINED-спецтокены распознаются, CONTROL — нет.
int  mind_tok_encode(const mind_tok_t* t, const char* text, int len, bool add_bos,
                     int32_t* out, int max);
// То же с флагами (MIND_TOK_PARSE_CONTROL), без BOS.
int  mind_tok_encode_ex(const mind_tok_t* t, const char* text, int len, int flags,
                        int32_t* out, int max);

// Байты токена для вывода (UTF-8). Управляющие и неиспользуемые токены — 0 байт.
int  mind_tok_piece(const mind_tok_t* t, int id, char* out, int cap);

// Точный поиск токена по строке; -1 если нет.
int  mind_tok_find(const mind_tok_t* t, const char* s);

// «Конец реплики»: eos, <|im_end|>, <|endoftext|>.
bool mind_tok_is_eog(const mind_tok_t* t, int id);

#endif
