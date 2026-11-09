// ============================================================================
// locale.c — C-locale (только "C" и "POSIX"). Этап 52 — полноценные локали.
// ============================================================================
#include <locale.h>
#include <stddef.h>

static char* g_current_locale = (char*)"C";

char* setlocale(int category, const char* locale) {
    (void)category;
    if (locale == NULL) return g_current_locale;
    return g_current_locale;
}

static struct lconv g_lconv = {
    .decimal_point  = (char*)".",
    .thousands_sep  = (char*)"",
    .grouping       = (char*)"",
    .int_curr_symbol= (char*)"",
    .currency_symbol= (char*)"",
    .mon_decimal_point=(char*)"",
    .mon_thousands_sep=(char*)"",
    .mon_grouping  = (char*)"",
    .positive_sign = (char*)"",
    .negative_sign = (char*)"",
    .int_frac_digits = -1,
    .frac_digits     = -1,
    .p_cs_precedes   = -1,
    .p_sep_by_space  = -1,
    .n_cs_precedes   = -1,
    .n_sep_by_space  = -1,
    .p_sign_posn     = -1,
    .n_sign_posn     = -1,
};

struct lconv* localeconv(void) { return &g_lconv; }
