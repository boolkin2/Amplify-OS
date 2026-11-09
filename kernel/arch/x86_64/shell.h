// ============================================================================
// shell.h — interactive AMPLIFY-OS shell для x86_64.
// ----------------------------------------------------------------------------
// Минимальный читатель команд из PS/2-keyboard, сравнение с таблицей
// встроенных команд, выполнение.  Сохраняет фирменную «мистическо-
// механическую» стилистику AMPLIFY-OS:
//
//   guide   — help
//   shine   — clear screen
//   voice   — echo
//   scan    — ls (VFS file list)
//   read    — cat (читать файл)
//   embers  — last serial log lines (TODO)
//   pulse   — ping (TODO)
//   freeze  — halt
//
// Читает ввод через keyboard_wait_char(), печатает через fb_console.
// Не возвращается.
// ============================================================================
#ifndef AOS_X86_64_SHELL_H
#define AOS_X86_64_SHELL_H

void shell_run(void) __attribute__((noreturn));

// Выполнить одну команду (без бесконечного цикла) — используется GUI-терминалом.
// Линия может быть модифицирована (in-place tokenize).
void shell_exec_line(char* line);

#endif
