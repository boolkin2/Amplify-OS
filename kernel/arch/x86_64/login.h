// ============================================================================
// login.h — минимальный текстовый экран входа AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Цели подэтапа 37.6c-1: Показать AMPLIFY-OS-логотип и спросить
// «AMPLIFY operator: name / passphrase». Принимаются ВСЕ непустые
// комбинации (полноценная FNV-1a хешированная БД пользователей —
// часть подэтапа 37.6c-4 вместе с security/users.c).
//
// Поддерживается «гостевой» вход: пустой пароль = guest.
// ============================================================================
#ifndef AOS_X86_64_LOGIN_H
#define AOS_X86_64_LOGIN_H

#include <stdint.h>
#include <stdbool.h>

#define LOGIN_NAME_MAX  32
#define LOGIN_PASS_MAX  64

typedef struct {
    char     name[LOGIN_NAME_MAX];
    bool     is_guest;
    uint32_t login_ms;     // момент входа (timer_get_uptime_ms)
} login_session_t;

// Заблокировать поток до успешного логина. Заполняет out_session, не
// возвращается до ввода имени.
void login_run(login_session_t* out_session);

// [этап 48] Единый вход после разблокировки зашифрованного тома: если
// passphrase подходит к учётной записи (установщик создаёт администратора
// с тем же паролем, что и у тома), сессия открывается без экрана входа.
// true — вошли; false — нужен обычный login_run().
bool login_try_passphrase(const char* passphrase, login_session_t* out_session);

// Получить активную сессию (NULL до login_run).
const login_session_t* login_current(void);

#endif
