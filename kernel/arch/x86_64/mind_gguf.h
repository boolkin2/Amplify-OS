// ============================================================================
// mind_gguf.h — разбор контейнера GGUF (v2/v3) для AMPLIFY MIND (этап 49).
// ----------------------------------------------------------------------------
// GGUF — формат llama.cpp/ggml: заголовок, таблица «ключ -> значение»
// (метаданные: архитектура, гиперпараметры, словарь токенизатора), таблица
// тензоров (имя, размерности, тип, смещение) и выровненная область данных.
// Файл целиком лежит в памяти (mind_load читает его с диска), поэтому парсер
// не копирует ничего крупного: строки и тензоры — указатели в исходный буфер.
//
// Файл приходит от пользователя — каждое чтение проверяется на выход за
// границы, размеры тензоров сверяются с размером файла. Битый файл даёт
// ошибку с текстом, а не #PF.
// ============================================================================
#ifndef AOS_X86_64_MIND_GGUF_H
#define AOS_X86_64_MIND_GGUF_H

#include <stdint.h>
#include <stdbool.h>

// Типы значений метаданных.
enum {
    GGUF_T_U8 = 0, GGUF_T_I8, GGUF_T_U16, GGUF_T_I16, GGUF_T_U32, GGUF_T_I32,
    GGUF_T_F32, GGUF_T_BOOL, GGUF_T_STR, GGUF_T_ARR, GGUF_T_U64, GGUF_T_I64,
    GGUF_T_F64
};

// Типы тензоров ggml, которые понимает MIND.
enum {
    GGML_T_F32 = 0, GGML_T_F16 = 1, GGML_T_Q4_0 = 2, GGML_T_Q8_0 = 8,
    GGML_T_BF16 = 30
};

typedef struct {
    const uint8_t* p;
    uint64_t       len;
} mind_str_t;

typedef struct {
    mind_str_t     key;
    uint32_t       type;
    const uint8_t* val;        // скаляр/строка: начало значения; массив: начало элементов
    uint32_t       arr_type;   // для GGUF_T_ARR
    uint64_t       arr_n;
} gguf_kv_t;

typedef struct {
    mind_str_t     name;
    uint32_t       n_dims;
    uint64_t       ne[4];      // ne[0] — самая «быстрая» размерность (длина строки)
    uint32_t       type;
    uint64_t       offset;     // от начала области данных
    uint64_t       nbytes;
    const uint8_t* data;
} gguf_tensor_t;

typedef struct {
    const uint8_t* base;
    uint64_t       size;
    uint32_t       version;
    uint64_t       n_kv;
    uint64_t       n_tensors;
    gguf_kv_t*     kv;         // kmalloc
    gguf_tensor_t* tensors;    // kmalloc
    uint64_t       alignment;
    uint64_t       data_off;
} gguf_file_t;

// 0 — успех; иначе <0 и текст ошибки в err.
int  gguf_parse(gguf_file_t* g, const uint8_t* buf, uint64_t size, char* err, int errlen);
void gguf_free(gguf_file_t* g);

const gguf_kv_t*     gguf_find(const gguf_file_t* g, const char* key);
const gguf_tensor_t* gguf_tensor(const gguf_file_t* g, const char* name);

// Скаляры любого целого типа приводятся к запрошенному.
bool gguf_get_u32 (const gguf_file_t* g, const char* key, uint32_t* out);
bool gguf_get_f32 (const gguf_file_t* g, const char* key, float* out);
bool gguf_get_bool(const gguf_file_t* g, const char* key, bool* out);
bool gguf_get_str (const gguf_file_t* g, const char* key, mind_str_t* out);

// Размер тензора в байтах по типу (0 — тип не поддерживается).
uint64_t    gguf_type_bytes(uint32_t type, uint64_t n_elems);
const char* gguf_type_name(uint32_t type);

// Итерация по массиву строк: *cursor начинается с kv->val.
bool gguf_arr_next_str(const gguf_file_t* g, const uint8_t** cursor, mind_str_t* out);

#endif
