// ============================================================================
// phantom.h — верхний слой гипервизора phantom: модель устройств гостя.
// ----------------------------------------------------------------------------
// Подэтап 50.1: настоящих устройств ещё нет. Здесь «стенд» — несколько портов
// отладки и встроенный проверочный гость (phantom_guest.asm), которыми
// доказывается работа нижних слоёв (vmx.c, hv.c): вход и выход, режимы
// процессора, изоляция памяти, вытеснение, делёж ядра с пулом.
// Все функции — для BSP.
// ============================================================================
#ifndef AOS_X86_64_PHANTOM_H
#define AOS_X86_64_PHANTOM_H

#include <stdint.h>
#include <stdbool.h>

typedef void (*phantom_sink_t)(const char* s);

// smoke 50.1: VMX на всех ядрах + один гость «реальный -> защищённый ->
// длинный режим». Печатает строки `[smoke 50.1] …` в serial. true и там, где
// VT-x нет (это не ошибка) — тогда строка говорит «skipped».
bool phantom_boot_selftest(void);

// `phantom selftest`: все сценарии. Строки `[phantom] test …` идут в serial,
// короткие — в out (окно терминала). Возвращает число провалов.
int  phantom_selftest(phantom_sink_t out);

// `phantom bench`: цена одного выхода из гостя (команда OUT туда и обратно).
void phantom_bench(phantom_sink_t out);

// Фоновый гость, который считает в бесконечном цикле: `phantom spin`,
// `phantom status`, `phantom stop`. Нужен, чтобы увидеть систему при
// работающем госте (кадры, пул, MIND).
bool phantom_spin_start(phantom_sink_t out);
void phantom_spin_status(phantom_sink_t out);
void phantom_spin_stop(phantom_sink_t out);
bool phantom_spin_active(void);

// ---- [50.2] настоящий гость: ядро Linux на устройствах phantom_pc ------------
// Ядро и initrd берутся из /disk/vault/phantom (vmlinuz64, corepure64.gz) и
// грузятся напрямую — это стенд устройств, пользователю он не показывается
// (в справке его нет): штатный путь гостя — через BIOS, подэтап 50.3.
// full_init=false — вместо init запускается /bin/sh прямо на консоли.
bool phantom_linux_start(phantom_sink_t out, bool full_init);
// Набрать строку на консоли гостя (в конце — Enter); пустая — один Enter.
void phantom_type(phantom_sink_t out, const char* text);

// ---- [50.3] гость с ISO через BIOS: `phantom run <путь>` -----------------------
// Штатный путь: файл ISO с тома FAT читается в память целиком, наш BIOS
// находит на нём загрузочный образ (El Torito) и запускает его в реальном
// режиме — дальше работает загрузчик с самого диска. Текстовый экран BIOS
// дублируется в serial (`[guest] …`), строку на консоль подаёт `phantom
// type`; экран и клавиатура гостя — окно Phantom (app_phantom.c, 50.5).
// false — запуск не начат (причина — в out и в serial).
// [50.5] Путь вида /cdrom/guests/x.iso — файл установочного носителя. Функция
// возвращается сразу: образ читается порциями из phantom_tick, гость
// стартует сам (строка `[phantom] bios boot: …`), отказ после чтения —
// `[phantom] run refused (…)` и phantom_info().error.
// [50.4] Тот же ISO отдаётся гостю как диск (virtio-blk, только чтение): после
// загрузки ядро гостя находит на нём свой носитель. pretype — строка, которая
// «уже набрана» к моменту приглашения загрузчика (0 — ничего): у загрузчика
// Alpine на ввод одна секунда, вручную не успеть.
bool phantom_run(phantom_sink_t out, const char* iso_path, const char* pretype);
// Текстовый экран гостя (память B8000) — в serial строками `[screen] NN|…`.
void phantom_screen(phantom_sink_t out);
// BSP, каждый проход цикла WM: выносит вывод консоли гостя в serial строками
// `[guest] …` и один раз сообщает, чем гость кончил.
// [50.5] Он же читает образ ISO в память по частям: phantom_run теперь
// возвращается сразу, а гость стартует, когда образ прочитан.
void phantom_tick(void);
// Идёт чтение образа: цикл WM в это время не засыпает между проходами.
bool phantom_loading(void);

// ---- [50.5] для окна гостя (app_phantom.c) и команд ---------------------------
#define PHANTOM_DISK_DIR    "/disk/vault/phantom"   // образы на системном томе
#define PHANTOM_MEDIUM_DIR  "/guests"               // … и на установочном носителе

typedef enum {
    PHANTOM_IDLE = 0,           // гостя нет
    PHANTOM_LOADING,            // образ читается в память
    PHANTOM_RUNNING,            // гость работает (или спит по HLT)
    PHANTOM_ENDED,              // гость кончил сам; экран и счётчики ещё есть
} phantom_state_t;

typedef struct {
    phantom_state_t state;
    char     image[64];         // имя файла образа
    uint64_t image_bytes, loaded_bytes;
    uint64_t ram_mb;
    int      core;              // на каком ядре живёт гость
    bool     sleeping;          // гость спит (HLT с разрешёнными прерываниями)
    uint32_t cpu_pct;           // доля ядра за последнюю секунду
    uint64_t uptime_ms;
    uint64_t exits, disk_mb, keys;
    char     end_text[120];     // чем гость кончил (PHANTOM_ENDED)
    char     error[120];        // почему не состоялся последний запуск ("" — всё хорошо)
    char     unavailable[120];  // почему гостей запускать нельзя вовсе ("" — можно)
    bool     test_guest;        // ядро занято проверочным гостем (`phantom spin`)
} phantom_info_t;

void phantom_info(phantom_info_t* out);
void phantom_clear_error(void);
// Снять гостя (или прервать чтение образа). Память гостя и образа освобождается.
void phantom_stop_guest(void);

// Текстовый экран гостя: 80x25 ячеек «символ | атрибут << 8», уже с учётом
// того, где гость держит начало экрана; курсор (-1 — спрятан). false — гостя нет.
bool phantom_screen_cells(uint16_t* cells, int* cur_col, int* cur_row);

// Клавиша гостю: скан-код набора 1 (бит 7 — отпускание), extended — с
// префиксом E0. Можно звать из обработчика прерывания клавиатуры; из главного
// потока — только с запрещёнными прерываниями.
void phantom_key(uint8_t sc, bool extended);

typedef struct {
    char     name[64];
    char     path[160];         // то, что принимает phantom_run
    uint64_t bytes;
    bool     on_medium;         // лежит на установочном носителе, а не на диске
} phantom_image_t;
// Образы гостей (*.iso) на диске и на носителе. Возвращает их число (<= max).
int phantom_images(phantom_image_t* out, int max);

#endif
