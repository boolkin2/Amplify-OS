// ============================================================================
// mind_chat.c — шаблоны диалога AMPLIFY MIND (этап 49.1). Контракт — mind_chat.h.
// ============================================================================
#include "mind_chat.h"
#include "mind_util.h"

// ----------------------------------------------------------------------------
// Инструменты. JSON-строки — РОВНО то, что выдаёт json.dumps(ensure_ascii=
// False) для tools/mind_tools.json (так их рендерит jinja-шаблон Qwen3 через
// tojson): ключи в том же порядке, разделители ", " и ": ". Любое отличие
// поймает тест (ids промпта хоста против официального шаблона).
// ----------------------------------------------------------------------------
static const char* const k_tool_names[MIND_TOOL_COUNT] = { "ps", "ls", "read", "logs", "exec" };
static const char* const k_tool_json[MIND_TOOL_COUNT] = {
    "{\"type\": \"function\", \"function\": {\"name\": \"ps\", \"description\": \"Show the state of this "
    "computer: uptime, CPU cores, memory, running tasks and open windows.\", \"parameters\": {\"type\": "
    "\"object\", \"properties\": {}, \"required\": []}}}",
    "{\"type\": \"function\", \"function\": {\"name\": \"ls\", \"description\": \"List files in a directory. "
    "The system disk is /disk.\", \"parameters\": {\"type\": \"object\", \"properties\": {\"path\": {\"type\": "
    "\"string\", \"description\": \"Directory, for example /disk or /disk/vault\"}}, \"required\": [\"path\"]}}}",
    "{\"type\": \"function\", \"function\": {\"name\": \"read\", \"description\": \"Show the beginning of a "
    "text file.\", \"parameters\": {\"type\": \"object\", \"properties\": {\"path\": {\"type\": \"string\", "
    "\"description\": \"File path, for example /disk/Sanctum/system.conf\"}}, \"required\": [\"path\"]}}}",
    "{\"type\": \"function\", \"function\": {\"name\": \"logs\", \"description\": \"Show the last lines of the "
    "kernel log.\", \"parameters\": {\"type\": \"object\", \"properties\": {}, \"required\": []}}}",
    "{\"type\": \"function\", \"function\": {\"name\": \"exec\", \"description\": \"Run a shell command of "
    "AMPLIFY-OS. The user has to confirm it.\", \"parameters\": {\"type\": \"object\", \"properties\": "
    "{\"command\": {\"type\": \"string\", \"description\": \"Shell command, for example clock or disk\"}}, "
    "\"required\": [\"command\"]}}}",
};
// Имя аргумента каждого инструмента (0 — без аргументов).
static const char* const k_tool_arg[MIND_TOOL_COUNT] = { 0, "path", "path", 0, "command" };

static const char k_tools_head[] =
    "# Tools\n\nYou may call one or more functions to assist with the user query.\n\n"
    "You are provided with function signatures within <tools></tools> XML tags:\n<tools>";
static const char k_tools_tail[] =
    "\n</tools>\n\nFor each function call, return a json object with function name and arguments "
    "within <tool_call></tool_call> XML tags:\n<tool_call>\n"
    "{\"name\": <function-name>, \"arguments\": <args-json-object>}\n</tool_call>";

const char* mind_tool_name(int tool) {
    return (tool >= 0 && tool < MIND_TOOL_COUNT) ? k_tool_names[tool] : "";
}

int mind_tool_index(const char* name) {
    for (int i = 0; i < MIND_TOOL_COUNT; i++) if (m_streq(name, k_tool_names[i])) return i;
    return -1;
}

unsigned mind_tool_mask_parse(const char* list) {
    unsigned m = 0;
    char w[16];
    while (*list) {
        int n = 0;
        while (*list == ' ' || *list == ',') list++;
        while (*list && *list != ',' && *list != ' ' && n < 15) w[n++] = *list++;
        w[n] = 0;
        if (n && m_streq(w, "all")) return MIND_TOOLS_ALL;
        int i = n ? mind_tool_index(w) : -1;
        if (i >= 0) m |= 1u << i;
        while (*list && *list != ',' && *list != ' ') list++;
    }
    return m;
}

int mind_chat_tools_block(char* out, int cap, unsigned mask) {
    out[0] = 0;
    m_append(out, (uint64_t)cap, k_tools_head);
    for (int i = 0; i < MIND_TOOL_COUNT; i++) {
        if (!(mask & (1u << i))) continue;
        m_append(out, (uint64_t)cap, "\n");
        m_append(out, (uint64_t)cap, k_tool_json[i]);
    }
    m_append(out, (uint64_t)cap, k_tools_tail);
    return (int)m_strlen(out);
}

// ----------------------------------------------------------------------------
// Сборщик промпта.
// ----------------------------------------------------------------------------
void mind_prompt_begin(mind_prompt_t* p, const mind_tok_t* t, int32_t* ids, int cap,
                       char* textbuf, int tcap) {
    p->tok = t;
    p->ids = ids;
    p->n = 0;
    p->cap = cap;
    p->text = textbuf;
    p->tn = 0;
    p->tcap = tcap;
    p->ok = true;
}

void mind_prompt_textn(mind_prompt_t* p, const char* s, int n) {
    if (!p->ok || n <= 0) return;
    if (p->tn + n > p->tcap) { p->ok = false; return; }
    m_memcpy(p->text + p->tn, s, (uint64_t)n);
    p->tn += n;
}

void mind_prompt_text(mind_prompt_t* p, const char* s) {
    mind_prompt_textn(p, s, (int)m_strlen(s));
}

static void flush_text(mind_prompt_t* p) {
    if (!p->ok || p->tn == 0) return;
    int r = mind_tok_encode_ex(p->tok, p->text, p->tn, 0, p->ids + p->n, p->cap - p->n);
    if (r < 0) { p->ok = false; return; }
    p->n += r;
    p->tn = 0;
}

void mind_prompt_tok(mind_prompt_t* p, int id) {
    flush_text(p);
    if (!p->ok) return;
    if (id < 0 || p->n >= p->cap) { p->ok = false; return; }
    p->ids[p->n++] = id;
}

int mind_prompt_end(mind_prompt_t* p) {
    flush_text(p);
    return p->ok ? p->n : -1;
}

// ----------------------------------------------------------------------------
// Куски шаблона.
// ----------------------------------------------------------------------------
void mind_chat_system(mind_prompt_t* p, int style, const char* system, unsigned tool_mask) {
    mind_prompt_tok(p, p->tok->im_start);
    mind_prompt_text(p, "system\n");
    if (system && *system) mind_prompt_text(p, system);
    if (style == MIND_CHAT_QWEN3 && tool_mask) {
        // Как в шаблоне Qwen3: system + "\n\n" + блок инструментов.
        if (system && *system) mind_prompt_text(p, "\n\n");
        mind_prompt_text(p, k_tools_head);
        for (int i = 0; i < MIND_TOOL_COUNT; i++) {
            if (!(tool_mask & (1u << i))) continue;
            mind_prompt_text(p, "\n");
            mind_prompt_text(p, k_tool_json[i]);
        }
        mind_prompt_text(p, k_tools_tail);
    }
    mind_prompt_tok(p, p->tok->im_end);
    mind_prompt_text(p, "\n");
}

void mind_chat_user(mind_prompt_t* p, const char* text) {
    mind_prompt_tok(p, p->tok->im_start);
    mind_prompt_text(p, "user\n");
    mind_prompt_text(p, text);
    mind_prompt_tok(p, p->tok->im_end);
    mind_prompt_text(p, "\n");
}

void mind_chat_tool_response(mind_prompt_t* p, const char* out, int n) {
    // Хвостовые переводы строк — лишние: шаблон сам ставит "\n" перед
    // </tool_response> (иначе ids разошлись бы с эталоном).
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) n--;
    mind_prompt_tok(p, p->tok->im_start);
    mind_prompt_text(p, "user\n<tool_response>\n");
    mind_prompt_textn(p, out, n);
    mind_prompt_text(p, "\n</tool_response>");
    mind_prompt_tok(p, p->tok->im_end);
    mind_prompt_text(p, "\n");
}

void mind_chat_assistant(mind_prompt_t* p, int style, bool think, const char* prefix) {
    mind_prompt_tok(p, p->tok->im_start);
    mind_prompt_text(p, "assistant\n");
    if (style == MIND_CHAT_QWEN3 && !think) mind_prompt_text(p, "<think>\n\n</think>\n\n");
    if (prefix && *prefix) mind_prompt_text(p, prefix);
}

void mind_chat_close(mind_prompt_t* p, int pending) {
    if (pending >= 0) mind_prompt_tok(p, pending);
    if (pending != p->tok->im_end) mind_prompt_tok(p, p->tok->im_end);
    mind_prompt_text(p, "\n");
}

// Строка JSON в кавычках (экранируются " и \, управляющие — \n/\t/\u00XX).
static void json_str(mind_prompt_t* p, const char* s) {
    static const char hx[] = "0123456789abcdef";
    mind_prompt_text(p, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') mind_prompt_text(p, "\\\"");
        else if (c == '\\') mind_prompt_text(p, "\\\\");
        else if (c == '\n') mind_prompt_text(p, "\\n");
        else if (c == '\t') mind_prompt_text(p, "\\t");
        else if (c < 0x20) {
            char u[7] = { '\\', 'u', '0', '0', hx[c >> 4], hx[c & 15], 0 };
            mind_prompt_text(p, u);
        } else mind_prompt_textn(p, (const char*)&c, 1);
    }
    mind_prompt_text(p, "\"");
}

void mind_chat_tool_call_text(mind_prompt_t* p, int tool, const char* arg) {
    mind_prompt_text(p, "<tool_call>\n{\"name\": \"");
    mind_prompt_text(p, mind_tool_name(tool));
    mind_prompt_text(p, "\", \"arguments\": {");
    const char* an = (tool >= 0 && tool < MIND_TOOL_COUNT) ? k_tool_arg[tool] : 0;
    if (an) {
        mind_prompt_text(p, "\"");
        mind_prompt_text(p, an);
        mind_prompt_text(p, "\": ");
        json_str(p, arg ? arg : "");
    }
    mind_prompt_text(p, "}}\n</tool_call>");
}

// ----------------------------------------------------------------------------
// Разбор вызова инструмента.
// ----------------------------------------------------------------------------
static const char* skip_ws(const char* s, const char* e) {
    while (s < e && (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t')) s++;
    return s;
}

// Строка JSON с s (указывает на '"'); результат в out (UTF-8), возврат —
// позиция после закрывающей кавычки или 0.
static const char* json_read_str(const char* s, const char* e, char* out, int cap) {
    if (s >= e || *s != '"') return 0;
    s++;
    int n = 0;
    while (s < e && *s != '"') {
        char c = *s++;
        if (c == '\\' && s < e) {
            char q = *s++;
            if (q == 'n') c = '\n';
            else if (q == 't') c = '\t';
            else if (q == 'r') c = '\r';
            else if (q == 'u' && e - s >= 4) {
                uint32_t v = 0;
                for (int k = 0; k < 4; k++) {
                    char h = s[k];
                    v = v * 16 + (uint32_t)(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                }
                s += 4;
                // UTF-8 (без суррогатных пар: в путях и командах их не бывает)
                if (v < 0x80) c = (char)v;
                else {
                    char u[3];
                    int un = 0;
                    if (v < 0x800) { u[un++] = (char)(0xC0 | (v >> 6)); u[un++] = (char)(0x80 | (v & 0x3F)); }
                    else {
                        u[un++] = (char)(0xE0 | (v >> 12)); u[un++] = (char)(0x80 | ((v >> 6) & 0x3F));
                        u[un++] = (char)(0x80 | (v & 0x3F));
                    }
                    for (int k = 0; k < un && n + 1 < cap; k++) out[n++] = u[k];
                    continue;
                }
            } else c = q;                       // \" \\ \/ и прочее — как есть
        }
        if (n + 1 < cap) out[n++] = c;
    }
    out[n] = 0;
    return (s < e) ? s + 1 : 0;
}

bool mind_parse_tool_call_json(const char* s, char* name, int ncap, char* args, int acap) {
    name[0] = 0;
    args[0] = 0;
    const char* open = 0;
    for (const char* q = s; *q; q++) if (m_starts(q, "<tool_call>")) open = q;
    if (!open) return false;
    const char* b = open + 11;
    const char* e = b;
    while (*e && !m_starts(e, "</tool_call>")) e++;
    if (!*e) return false;
    // Ищем ключи "name" и "arguments" на любом уровне первого объекта.
    for (const char* q = b; q < e; q++) {
        if (*q != '"') continue;
        char key[24];
        const char* after = json_read_str(q, e, key, sizeof(key));
        if (!after) break;
        const char* v = skip_ws(after, e);
        if (v >= e || *v != ':') { q = after - 1; continue; }
        v = skip_ws(v + 1, e);
        if (m_streq(key, "name") && v < e && *v == '"') {
            const char* nx = json_read_str(v, e, name, ncap);
            if (!nx) break;
            q = nx - 1;
        } else if ((m_streq(key, "path") || m_streq(key, "command")) && v < e && *v == '"') {
            const char* nx = json_read_str(v, e, args, acap);
            if (!nx) break;
            q = nx - 1;
        } else {
            q = v - 1;
        }
    }
    return name[0] != 0;
}

bool mind_parse_tool_call_tag(const char* s, char* name, int ncap, char* args, int acap) {
    const char* open = 0;
    for (const char* p = s; *p; p++) if (m_starts(p, "<tool>")) open = p;
    if (!open) return false;
    const char* b = open + 6;
    const char* e = b;
    while (*e && !m_starts(e, "</tool>")) e++;
    if (!*e) return false;
    while (b < e && *b == ' ') b++;
    int n = 0;
    while (b < e && *b != ' ' && n + 1 < ncap) name[n++] = *b++;
    name[n] = 0;
    while (b < e && *b == ' ') b++;
    n = 0;
    while (b < e && n + 1 < acap) args[n++] = *b++;
    while (n > 0 && args[n - 1] == ' ') n--;
    args[n] = 0;
    return name[0] != 0;
}
