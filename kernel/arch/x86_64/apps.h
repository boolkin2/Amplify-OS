// ============================================================================
// apps.h — GUI-приложения для AMPLIFY-OS x86_64 (подэтап 37.6c-3).
// ----------------------------------------------------------------------------
// Каждое app() создаёт окно через window_create + window_t.on_draw/click/key,
// добавляет его в WM через wm_add_window и возвращает указатель (или NULL).
// Внутреннее состояние app живёт в win->user_data (выделяется в *_open).
// ============================================================================
#ifndef AOS_X86_64_APPS_H
#define AOS_X86_64_APPS_H

#include "window.h"

window_t* app_about_open  (void);
window_t* app_info_open   (void);
window_t* app_clock_open  (void);
window_t* app_matrix_open (void);

window_t* app_calc_open   (void);
window_t* app_notes_open  (void);
window_t* app_term_open   (void);
// [51.3] Для программы, запущенной командой терминала: приёмник её вывода (в
// терминал и в журнал — и после возврата команды) и сам терминал-владелец
// (0 — команда исполняется не из окна терминала).
void (*app_term_job_sink(void))(const char*);
void* app_term_job_owner(void);
window_t* app_files_open  (void);
window_t* app_paint_open  (void);
window_t* app_embers_open (void);
window_t* app_browser_open(void);
window_t* app_taskmgr_open(void);
window_t* app_strata_open(void);
window_t* app_thread_open(void);
window_t* app_amplifier_open(void);
// Pearl Stage E workspaces.
window_t* app_signal_open(void);
window_t* app_arc_open(void);
window_t* app_resonance_open(void);
window_t* app_core_open(void);
window_t* app_facet_open(void);
window_t* app_mesh_open(void);
// [50.5] Phantom — окно гипервизора: библиотека образов и консоль гостя.
// Окно одно; повторный вызов показывает уже открытое. tick — каждый проход
// цикла WM (экран гостя, захват клавиатуры); без окна — одна проверка.
window_t* app_phantom_open(void);
void      app_phantom_tick(void);
// [51.4] Rune — окно программ WebAssembly: библиотека и консоль программы.
// Окно одно; его закрытие программу не трогает. tick — каждый проход цикла WM.
// dump — `rune screen` (что видно в консоли) и `rune log` (весь журнал) в serial.
window_t* app_rune_open(void);
void      app_rune_tick(void);
void      app_rune_dump(bool whole);
window_t* app_capacity_open(void);

// [этап 44] GL Cube — программный 3D-стек, текстурированный вращающийся куб.
window_t* app_glcube_open (void);

// [этап 45] Amply Center — пакетный менеджер.
window_t* app_amply_open  (void);

// [этап 46] Sandbox Manager — namespaces/cgroups/seccomp viewer + box launch.
window_t* app_box_open    (void);

// Spawn ELF из FAT32: создаёт окно с выводом, запускает spawn_user_task.
// На 37.6c-3 если FAT32 не примонтирован — отображается заглушка.
window_t* app_launch_elf_open(const char* fat32_path);

#endif
