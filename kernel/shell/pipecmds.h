// ============================================================================
// pipecmds.h — Команды-«канальники» MyOS shell (Этап 29)
// ----------------------------------------------------------------------------
// Эти команды специально написаны под абстракцию sh_io: они одинаково хорошо
// работают как в одиночку (вывод → VGA), так и внутри конвейера
// (вывод → pipe для следующей команды).
//
// Имена — оригинальные, в духе словаря MyOS, а НЕ как в Linux:
//
//   spill <путь>             — «вылить» содержимое файла  (≈ cat)
//   sift <подстрока>         — отфильтровать строки stdin (≈ grep)
//   holler <текст>           — крикнуть текст один раз     (≈ echo)
//   tally                    — подсчитать строки stdin     (≈ wc -l)
//   crown <N>                — взять первые N строк        (≈ head)
//   sole                     — оставить только уникальные  (≈ uniq)
//
//   plumes                   — список зарегистр. задач     (≈ ps)
//   hush  <pid>              — отправить SIG_INT
//   smite <pid>              — отправить SIG_KILL
//
//   bind <name>              — демо: создать LISTENER-сокет (Unix Domain)
//   speak <name> <текст>     — демо: connect+send «name»
// ============================================================================

#ifndef SHELL_PIPECMDS_H
#define SHELL_PIPECMDS_H

#include "shell.h"

// «Контекстные» команды. Все они используют sh_io для вывода и (если нужно)
// для ввода из in_pipe. Принимают argc/argv в стандартной shell-форме.

void cmd_spill (int argc, char args[][SHELL_MAX_ARG_LEN]);
void cmd_sift  (int argc, char args[][SHELL_MAX_ARG_LEN]);
void cmd_holler(int argc, char args[][SHELL_MAX_ARG_LEN]);
void cmd_tally (int argc, char args[][SHELL_MAX_ARG_LEN]);
void cmd_crown (int argc, char args[][SHELL_MAX_ARG_LEN]);
void cmd_sole  (int argc, char args[][SHELL_MAX_ARG_LEN]);

// Команды управления задачами/сигналами
void cmd_plumes(int argc, char args[][SHELL_MAX_ARG_LEN]);
void cmd_hush  (int argc, char args[][SHELL_MAX_ARG_LEN]);
void cmd_smite (int argc, char args[][SHELL_MAX_ARG_LEN]);

// Демонстрационные команды Unix Domain Sockets
void cmd_bind  (int argc, char args[][SHELL_MAX_ARG_LEN]);
void cmd_speak (int argc, char args[][SHELL_MAX_ARG_LEN]);

// Возвращает true, если name — одна из наших pipe-команд.
// Используется парсером, чтобы выбрать правильный диспетчер.
bool pipecmds_is_known(const char* name);

// Запустить pipe-команду по имени. Возвращает true если команда нашлась.
bool pipecmds_dispatch(int argc, char args[][SHELL_MAX_ARG_LEN]);

#endif // SHELL_PIPECMDS_H
