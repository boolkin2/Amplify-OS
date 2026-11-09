# libc-x86_64 — C-библиотека AMPLIFY-OS для Long Mode

Это стандартная библиотека C проекта AMPLIFY-OS для архитектуры
x86_64. Собрана своими силами (без musl/newlib-tarball), поверх
нашего syscall-интерфейса `aos_*` (`kernel/arch/x86_64/syscall.h`).

## Running vs Target

| Слой | Где сейчас | Где будет в 57 |
|------|------------|----------------|
| Host-GCC | WSL2 Ubuntu x86_64 Linux | AMPLIFY-OS x86_64 self-hosted |
| Target   | AMPLIFY-OS x86_64         | тот же                         |
| sysroot  | `build/sysroot/` в хосте  | `/disk/nexus/sysroot/` в цели  |
| libc.a   | WSL2-собранный            | self-hosted-собранный          |
| libc.so  | WSL2-собранный            | self-hosted-собранный          |

**Самохостинг libc откладывается до этапа 57** (`os swap` с
полноценным инструментарием внутри AMPLIFY-OS). До этого GCC
запускается из WSL, и результирующие артефакты используются
target-ядром. Этот boundary явно зафиксирован здесь и в
`CLAUDE.md` п. 15.

## Покрытие POSIX

Полный статус после этапа 40:

### Реально (функционально работает)

| Категория | Функции |
|-----------|---------|
| I/O низкого уровня | `read`, `write`, `close`, `lseek`, `open`(+O_CREAT/O_TRUNC/O_APPEND/O_EXCL) |
| Работа с FILE*    | `fopen/fclose/fread/fwrite/fseek/ftell/fflush/fgets/puts/fputs/fputc/fgetc/setbuf/setvbuf/perror` + `ungetc` (1-символьный pushback) |
| printf-движок     | `printf/fprintf/vprintf/vfprintf/sprintf/snprintf/vsnprintf/vsprintf` — d/i/u/x/X/o/p/s/c/%, width, precision, flags, ll/l/z/h |
| malloc/free       | first-fit + coalesce поверх sbrk (64 KB grow-chunk) |
| string            | strlen/strcpy/strcmp/strncmp/strchr/strstr/memcpy/memmove/memset/memcmp и т.д. |
| stat              | `stat/fstat/lstat/mkdir` через VFS |
| dup / fcntl       | `dup/dup2`, `fcntl(F_GETFL/F_SETFL/F_DUPFD/F_GETFD/F_SETFD)` |
| pthread           | `pthread_create/join` (через AOS_SYS_CLONE), `pthread_mutex_*` (fast CAS + slow futex), `pthread_self` (FS:[0]), `pthread_once` (CAS) |
| signals           | `signal/sigaction/kill/raise` + user-side sigreturn trampoline, SIGSEGV из page fault |
| sockets           | `socket/bind/connect/send/recv/sendto/recvfrom/shutdown` (TCP client + UDP) поверх tcp.c/udp.c |
| mmap              | `mmap(MAP_ANON)` поверх sbrk |
| setjmp / longjmp  | полная реализация на ассемблере |
| math              | sqrt/sin/cos/tan/exp/log/pow/atan/atan2/asin/acos/fmod + f-варианты (Taylor) |
| time              | gettimeofday, clock_gettime, nanosleep, gmtime/localtime/strftime/mktime |
| unistd-процесс    | `getpid/getuid/sbrk/brk/sleep/usleep` |
| stdio             | stdin/stdout/stderr, буферизация _IOFBF/_IOLBF/_IONBF |
| byte-order        | htons/ntohs/htonl/ntohl, inet_addr/inet_ntoa/inet_pton/inet_ntop |
| locale            | только "C" (заглушка под setlocale) |
| ctype             | isdigit/isalpha/isspace и т.д. |

### Stub-и / deferred

| Категория | Статус | Отложено до |
|-----------|--------|-------------|
| `fork/exec*`    | -ENOSYS | stage 45 (sandboxes) |
| `pipe`          | -ENOSYS | stage 45 |
| `chdir/fchdir`  | -ENOSYS | stage 45 |
| `unlink/rmdir/access/ftruncate` | -ENOSYS | stage 45 |
| `listen/accept` (TCP server) | -ENOSYS | stage 46+ |
| POSIX `shm_open` | отсутствует | stage 45 |
| `chmod/fchmod/umask` | no-op (возвращают 0) | — |
| `siginfo_t`, sigaltstack | отсутствуют | stage 45 |
| real-time signals (SIGRTMIN..SIGRTMAX) | не реализуются | — |
| `pthread_cond_*` | stub-over-mutex | stage 45 |
| `pthread_rwlock_*`, `pthread_barrier_*` | stub | stage 45 |
| LD_PRELOAD, версионирование символов | не реализуется | — |
| IPv6 / AF_UNIX / SOCK_RAW | -EAFNOSUPPORT | stage 46+ |
| `%f` / `%e` / `%g` в printf | не реализовано | stage 43 (3D/GL потребует) |
| полный Unicode UTF-8 | CP1251 only | stage 52 |

## Сборка

Из корня репозитория:

```bash
make ARCH=x86_64 sysroot   # только libc.a + libc.so + crt0.o
make ARCH=x86_64           # полная сборка ядра + userspace + ISO
```

Результаты:

- `build/sysroot/usr/include/` — заголовки (stdio.h, pthread.h, …)
- `build/sysroot/usr/lib/libc.a` — static libc (≈85 KB)
- `build/sysroot/usr/lib/libc.so` — shared libc (≈75 KB, ELF64 PIC)
- `build/sysroot/usr/lib/crt0.o` — ENTRY для ELF-программ
- `build/sysroot/usr/lib/user.ld` — линкер-скрипт

## Пример использования sysroot

```bash
gcc --sysroot=build/sysroot -nostdinc \
    -isystem build/sysroot/usr/include \
    -m64 -ffreestanding -fno-pie -no-pie -fno-stack-protector \
    -mcmodel=large -mno-red-zone \
    -nostdlib -static \
    -Wl,-T,userspace/x86_64/user.ld -Wl,-z,max-page-size=0x1000 \
    build/sysroot/usr/lib/crt0.o hello.c \
    -L build/sysroot/usr/lib -lc \
    -o hello.elf
```

Получается ELF64-бинарь для user-mode AMPLIFY-OS. Копируется в
initrd — запускается из шелла командой `launch /hello.elf`.

## Тестирование

```bash
bash scripts/test_stage40_libc.sh
```

Проверяет все 31 критерий приёмки этапа 40.

## Стабильность ABI

Номера syscalls `0x100..0x10C` **заморожены** (см. requirements,
Requirement 2 Phase 2 в `.kiro/specs/stage-40-libc-finalize/`).
Новые syscalls добавлялись только в диапазоны `0x110+`:

- `0x110..0x115` — stat, fstat, mkdir, dup, dup2, fcntl
- `0x116..0x118` — signal/kill/sigreturn
- `0x120..0x124` — clone, futex_wait, futex_wake, set_tid, join
- `0x130..0x13B` — sockets

Зеркало в `libc-x86_64/src/syscall/aos_arch.h`.
