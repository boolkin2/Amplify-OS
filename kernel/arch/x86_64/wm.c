// ============================================================================
// wm.c -- реализация оконного менеджера.
// ----------------------------------------------------------------------------
// Этап 42: интеграция с compositor.c -- тройная буферизация, alpha-blend,
// тени, скруглённые углы, анимации, dirty-rect tracking.
// ============================================================================
#include "wm.h"
#include "rune.h"
#include "window.h"
#include "doublebuf.h"
#include "graphics.h"
#include "compositor.h"
#include "cursor.h"
#include "mouse.h"
#include "keyboard.h"
#include "pit.h"
#include "serial.h"
#include "ui_overlay.h"
#include "beacon.h"
#include "constellation.h"
#include "ui_components.h"
#include "apps.h"
#include "mind_widget.h"
#include "framestat.h"
#include "heap.h"
#include "desktop.h"
#include "drm.h"
#include "disk_io.h"
#include "netif.h"
#include "netcfg.h"
#include "phantom.h"
#include "dhcp.h"
#include "idt.h"
#include "timer.h"
#include "virtio_gpu.h"
#include "virtio_input.h"
#include "ui_fx.h"
#include "ui_motion.h"
#include "ui_theme.h"

#include <stdint.h>
#include <stdbool.h>

extern void desktop_draw(const gfx_target_t* tgt);
extern bool desktop_handle_click(int mx, int my);
extern void desktop_set_pointer(int mx, int my);
extern bool desktop_handle_key(char c);

// ----------------------------------------------------------------------------
// Z-order: связный список. head = bottom, tail = top.
// ----------------------------------------------------------------------------
static window_t* g_head = 0;
static window_t* g_tail = 0;

static void list_remove(window_t* w) {
    if (w->prev) w->prev->next = w->next; else g_head = w->next;
    if (w->next) w->next->prev = w->prev; else g_tail = w->prev;
    w->prev = w->next = 0;
}

static void list_push_top(window_t* w) {
    w->prev = g_tail;
    w->next = 0;
    if (g_tail) g_tail->next = w;
    else        g_head        = w;
    g_tail = w;
}

// ----------------------------------------------------------------------------
void wm_init(void) {
    g_head = g_tail = 0;
    ui_overlay_init();
    beacon_init();
    constellation_init();
    mind_widget_init();
}

void wm_add_window(window_t* w) {
    if (!w) return;
    list_push_top(w);
    wm_focus(w);
    // Этап 42: запускаем анимацию открытия.
    if (compositor_active()) {
        compositor_anim_open(w);
    }
}

void wm_remove_window(window_t* w) {
    if (!w) return;
    bool was_top = (g_tail == w);
    list_remove(w);
    if (was_top && g_tail) {
        g_tail->focused = true;
    }
}

void wm_focus(window_t* w) {
    if (!w) return;
    // Снимаем focused с предыдущего top.
    for (window_t* x = g_head; x; x = x->next) x->focused = false;
    list_remove(w);
    list_push_top(w);
    w->focused = true;
}

window_t* wm_focused(void) { return g_tail; }

int wm_count(void) {
    int n = 0;
    for (window_t* x = g_head; x; x = x->next) n++;
    return n;
}

window_t* wm_iterate(window_t* prev) {
    return prev ? prev->next : g_head;
}

// ----------------------------------------------------------------------------
// Mouse handling: drag (если внутри title bar) или передача клика
// приложению.
// ----------------------------------------------------------------------------
static window_t* g_drag_win   = 0;
static int       g_drag_off_x = 0;
static int       g_drag_off_y = 0;
static bool      g_prev_left  = false;

static void process_mouse_inner(void) {
    const mouse_state_t* m = mouse_get_state();
    desktop_set_pointer(m->x, m->y);
    bool left = (m->buttons & MOUSE_BTN_LEFT) != 0;
    bool edge_down = !g_prev_left && left;
    bool edge_up   =  g_prev_left && !left;

    // Drag в процессе?
    if (g_drag_win && left) {
        g_drag_win->x = m->x - g_drag_off_x;
        g_drag_win->y = m->y - g_drag_off_y;
    }

    if (edge_up) {
        g_drag_win = 0;
    }

    if (edge_down) {
        // [этап 49] AMPLIFY MIND — самый верхний слой.
        if (mind_widget_is_open()) {
            (void)mind_widget_handle_click(m->x, m->y);
            g_prev_left = left;
            return;
        }
        if (constellation_is_open()) {
            (void)constellation_handle_click(m->x, m->y);
            g_prev_left = left;
            return;
        }
        if (beacon_is_open()) {
            (void)beacon_handle_click(m->x, m->y);
            g_prev_left = left;
            return;
        }
        if (ui_overlay_is_open()) {
            (void)ui_overlay_handle_click(m->x, m->y);
            g_prev_left = left;
            return;
        }
        // 1) Поиск окна под курсором (top → bottom).
        window_t* target = 0;
        for (window_t* w = g_tail; w; w = w->prev) {
            if (!w->visible) continue;
            int total_h = window_title_height(w) + w->height;
            if (m->x >= w->x && m->x < w->x + w->width &&
                m->y >= w->y && m->y < w->y + total_h) {
                target = w;
                break;
            }
        }

        if (target) {
            wm_focus(target);

            // 2) Close-button?
            if (window_hit_close(target, m->x, m->y)) {
                // Этап 42: запускаем анимацию закрытия вместо мгновенного скрытия.
                if (compositor_active() && !compositor_is_animating(target)) {
                    compositor_anim_close(target);
                    serial_puts("[wm] window close anim started\n");
                } else {
                    target->close_requested = true;
                    target->visible = false;
                    serial_puts("[wm] window close requested\n");
                }
            }
            // 3) Title bar → drag.
            else if (window_hit_titlebar(target, m->x, m->y)) {
                g_drag_win   = target;
                g_drag_off_x = m->x - target->x;
                g_drag_off_y = m->y - target->y;
            }
            // 4) Content → передаём приложению.
            else if (window_hit_content(target, m->x, m->y)) {
                if (target->on_click) {
                    int lx = m->x - target->x;
                    int ly = m->y - target->y - window_title_height(target);
                    target->on_click(target, lx, ly);
                }
            }
        } else {
            bool consumed = false;
            if (!consumed) {
                desktop_handle_click(m->x, m->y);
            }
        }
    }

    g_prev_left = left;
}

// [41.2] Обёртка: сообщает, изменило ли событие мыши СЦЕНУ (нажатие/отпускание
// кнопки, сдвиг перетаскиваемого окна). Простое движение курсора сцену не
// меняет — тогда WM рисует только курсор. Смену hover-слота rail ловит ключ
// кэша рабочего стола (desk_stale), её здесь учитывать не надо.
// Нажатие определяем по g_prev_left до/после: сам снимок кнопок берёт
// process_mouse_inner, а состояние мыши меняется из IRQ в любой момент.
static bool process_mouse(void) {
    bool      prev_left = g_prev_left;
    window_t* drag      = g_drag_win;
    int       dx = drag ? drag->x : 0, dy = drag ? drag->y : 0;
    process_mouse_inner();
    // [48.2] Колесо: прокрутка достаётся верхнему окну под указателем (если
    // оно её понимает и поверх нет оверлея). Это изменение сцены.
    int wheel = mouse_take_wheel();
    if (wheel && !mind_widget_is_open() && !constellation_is_open() &&
        !beacon_is_open() && !ui_overlay_is_open()) {
        const mouse_state_t* m = mouse_get_state();
        for (window_t* w = g_tail; w; w = w->prev) {
            if (!w->visible) continue;
            int total_h = window_title_height(w) + w->height;
            if (m->x < w->x || m->x >= w->x + w->width || m->y < w->y || m->y >= w->y + total_h) continue;
            if (w->on_scroll) { w->on_scroll(w, wheel); return true; }
            break;
        }
    }
    if (g_prev_left != prev_left) return true;
    if (drag && drag == g_drag_win && (drag->x != dx || drag->y != dy)) return true;
    return false;
}

// ----------------------------------------------------------------------------
// Этап F: сбор закрытых окон. До этого этапа window_destroy() не
// вызывался нигде: каждое закрытое окно навсегда оставляло в heap
// свой content-buffer (width*height*4 байт — до ~5.8 МБ у workspace-окна)
// и своё состояние. 50 циклов open/close съедали всю память.
// Окно уничтожается только когда анимация закрытия доиграла.
// ----------------------------------------------------------------------------
static void reap_closed_windows(void) {
    window_t* w = g_head;
    while (w) {
        window_t* next = w->next;
        if (w->close_requested && !compositor_is_animating(w)) {
            if (g_drag_win == w) g_drag_win = 0;
            compositor_forget_window(w);
            constellation_forget_window(w);
            wm_remove_window(w);
            window_destroy(w);
            serial_puts("[wm] window destroyed (memory released)\n");
        }
        w = next;
    }
}

// ============================================================================
// [41.2-B] Кадры двух видов.
//
// До 41.2 любой повод (сдвиг мыши на пиксель, тик 100 мс) означал: нарисовать
// рабочий стол с нуля, все окна, курсор и отдать хосту все 8 МБ кадра —
// 17-21 мс под KVM, ~45 кадров/с при движении мыши и 15 % CPU в простое.
// Теперь:
//   * слой рабочего стола лежит в кэше g_desk и перерисовывается, только когда
//     меняется его ключ (минута, подсвеченный слот rail, тема) — desk_stale;
//   * курсор не часть сцены: он кладётся поверх с сохранением фона, и сдвиг
//     мыши — это render_cursor: вернуть фон, нарисовать спрайт, вывести два
//     прямоугольника 12x19;
//   * render_scene рисует сцену (копия кэша + окна + оверлеи) только по
//     настоящему поводу и выводит на экран только изменившуюся область, когда
//     она известна (окна), иначе весь кадр.
// Приложения рисуют содержимое заново в каждом кадре сцены (on_draw), о своих
// изменениях WM не сообщают — поэтому при видимых окнах остаётся обновление
// раз в 100 мс; пустой рабочий стол в простое не перерисовывается вовсе.
// ============================================================================
static gfx_target_t g_desk        = {0, 0, 0, 0};   // кэш слоя рабочего стола
static uint64_t     g_desk_key    = 0;
static bool         g_desk_valid  = false;
static bool         g_desk_moving = false;   // кэш снят посреди анимации rail
static bool         g_desk_no_mem = false;   // kmalloc не дал буфер — рисуем как раньше
static bool         g_scene_dirty = false;   // wm_invalidate()

// Область окон в предыдущем кадре сцены: её тоже надо вывести (окно ушло,
// сдвинулось или закрылось — на его месте теперь фон).
static int  g_prev_x = 0, g_prev_y = 0, g_prev_w = 0, g_prev_h = 0;
static bool g_prev_full = true;
// Значение doublebuf_present_count() после нашего последнего кадра.
static uint64_t g_my_presents = 0;

// [41.2-D] Аппаратный курсор virtio-gpu. Включается, только если указатель
// абсолютный (virtio-планшет): тогда клиент консоли рисует курсор сам ровно
// под указателем хоста. С относительной PS/2-мышью положение курсора гостя
// клиенту неизвестно — там остаётся программный курсор в кадре.
// Выключить принудительно: строка `hwcursor=0` в /disk/Sanctum/perf.conf.
static bool g_hw_cursor = false;

static void hw_cursor_setup(void) {
    static uint32_t img[VIRTIO_GPU_CURSOR_SIZE * VIRTIO_GPU_CURSOR_SIZE];
    if (!drm_active() || !virtio_input_has_tablet()) return;
    if (fs_conf_has("hwcursor=0")) {
        serial_puts("[wm] hardware cursor disabled by perf.conf\n");
        return;
    }
    cursor_render_argb(img, VIRTIO_GPU_CURSOR_SIZE);
    const mouse_state_t* m = mouse_get_state();
    if (virtio_gpu_cursor_define(img, 0, 0, m->x, m->y)) {
        g_hw_cursor = true;
        serial_puts("[wm] hardware cursor: virtio-gpu cursor queue\n");
    }
}

void wm_invalidate(void) { g_scene_dirty = true; }

// [48.2] Шаг кадров анимации. Любое «движение» (окна, оверлеи, вход на стол)
// рисуется с периодом 16 мс: раньше кадр анимации шёл на каждый проход цикла,
// а цикл просыпался по границам 10 мс — выходило то 100, то 50 кадров/с в
// зависимости от того, уложился ли кадр в 10 мс.
#define WM_FRAME_MS 16
static uint64_t g_last_scene_ms = 0;

// [48.2] Вход на рабочий стол. Последний кадр до WM (экран входа, заставка,
// разблокировка диска) снимается в буфер и первые ~полсекунды лежит поверх
// рабочего стола: уезжает вверх и растворяется. При reduced_motion — сразу стол.
static uint32_t*       g_enter_snap = 0;
static int             g_enter_w = 0, g_enter_h = 0;
static ui_transition_t g_enter_tr;
static uint32_t        g_enter_frames = 0;
static uint64_t        g_enter_t0 = 0;

static void enter_begin(void) {
    const gfx_target_t* t = compositor_target();
    if (!t || !t->pixels || ui_theme_reduced_motion() || !compositor_active()) return;
    size_t bytes = (size_t)t->height * (size_t)t->pitch;
    g_enter_snap = (uint32_t*)kmalloc(bytes);
    if (!g_enter_snap) return;                  // памяти нет — без перехода
    size_t qwords = bytes / 8;
    void* d = g_enter_snap; const void* s = t->pixels;
    __asm__ __volatile__("rep movsq" : "+D"(d), "+S"(s), "+c"(qwords) : : "memory");
    g_enter_w = t->width; g_enter_h = t->height;
    g_enter_frames = 0;
    g_enter_t0 = timer_get_uptime_ms();
    ui_transition_start(&g_enter_tr, 520, 0, UI_EASE_OUT_CUBIC);
}

static bool enter_active(void) { return g_enter_snap != 0; }

// Наложить уходящий экран поверх сцены. true — переход ещё идёт.
static bool enter_draw(const gfx_target_t* tgt) {
    if (!g_enter_snap) return false;
    bool running = ui_transition_active(&g_enter_tr);
    if (!running || tgt->width != g_enter_w || tgt->height != g_enter_h) {
        kfree(g_enter_snap);
        g_enter_snap = 0;
        fx_log_anim("desktop", "enter", "open", g_enter_frames,
                    (uint32_t)(timer_get_uptime_ms() - g_enter_t0));
        return false;
    }
    uint8_t p = ui_transition_progress(&g_enter_tr);
    g_enter_frames++;
    fx_blit(tgt, 0, -(tgt->height / 8) * p / 255, g_enter_snap, g_enter_w, g_enter_w, g_enter_h,
            (uint8_t)(255 - p), false);
    return true;
}

static bool desk_stale(void) {
    return !g_desk_valid || g_desk_moving || desktop_state_key() != g_desk_key;
}

// Привести кэш в актуальное состояние. true — слой перерисован (ущерб — весь экран).
static bool desk_prepare(const gfx_target_t* tgt) {
    // Режим сменили (drm_set_mode) — кэш старого размера выбрасываем.
    if (g_desk.pixels && (g_desk.width != tgt->width || g_desk.height != tgt->height)) {
        kfree(g_desk.pixels);
        g_desk.pixels = 0;
        g_desk_valid  = false;
    }
    if (!g_desk.pixels && !g_desk_no_mem) {
        g_desk.pixels = (uint32_t*)kmalloc((size_t)tgt->width * (size_t)tgt->height * 4);
        if (!g_desk.pixels) {
            g_desk_no_mem = true;
            serial_puts("[wm] desktop layer cache: kmalloc failed, drawing the desktop every frame\n");
        } else {
            g_desk.width  = tgt->width;
            g_desk.height = tgt->height;
            g_desk.pitch  = tgt->width * 4;
        }
    }
    if (g_desk_no_mem) return true;
    if (!desk_stale()) return false;
    // Ключ берём ДО отрисовки: desktop_draw сам запускает анимацию подсветки
    // rail, и пока она идёт (g_desk_moving), слой считается устаревшим — так
    // последний кадр анимации тоже попадёт в кэш.
    g_desk_key = desktop_state_key();
    desktop_draw(&g_desk);
    g_desk_moving = ui_components_motion_active();
    g_desk_valid  = true;
    return true;
}

static void rect_union(int* x, int* y, int* w, int* h, int bx, int by, int bw, int bh) {
    if (bw <= 0 || bh <= 0) return;
    if (*w <= 0 || *h <= 0) { *x = bx; *y = by; *w = bw; *h = bh; return; }
    int x1 = (*x + *w > bx + bw) ? *x + *w : bx + bw;
    int y1 = (*y + *h > by + bh) ? *y + *h : by + bh;
    if (bx < *x) *x = bx;
    if (by < *y) *y = by;
    *w = x1 - *x; *h = y1 - *y;
}

static bool rect_inside(int x, int y, int w, int h, int ox, int oy, int ow, int oh) {
    return x >= ox && y >= oy && x + w <= ox + ow && y + h <= oy + oh;
}

// Сдвиг курсора без изменения сцены.
static void render_cursor(void) {
    const gfx_target_t* tgt = compositor_target();
    if (!tgt) return;
    uint64_t fs_frame0 = fs_now();
    const mouse_state_t* m = mouse_get_state();
    if (g_hw_cursor) {
        // Аппаратный курсор: одна команда в очередь курсора, без ожидания.
        virtio_gpu_cursor_move(m->x, m->y);
        fs_add(FS_CURSOR, fs_frame0);
        fs_add(FS_FRAME, fs_frame0);
        fs_frame_done();
        return;
    }
    int ox = 0, oy = 0, ow = 0, oh = 0;
    bool had = cursor_overlay_rect(&ox, &oy, &ow, &oh);
    cursor_overlay_hide(tgt);
    cursor_overlay_show(tgt, m->x, m->y);
    fs_add(FS_CURSOR, fs_frame0);

    int nx = 0, ny = 0, nw = 0, nh = 0;
    bool has = cursor_overlay_rect(&nx, &ny, &nw, &nh);
    // Близкие прямоугольники выводим одним куском (одна пара команд хосту
    // вместо двух), далёкие — раздельно, чтобы не гонять пустую середину.
    int ux = ox, uy = oy, uw = had ? ow : 0, uh = had ? oh : 0;
    if (has) rect_union(&ux, &uy, &uw, &uh, nx, ny, nw, nh);
    if (had && has && uw * uh > 4 * CURSOR_W * CURSOR_H) {
        compositor_present_rect(ox, oy, ow, oh);
        compositor_present_rect(nx, ny, nw, nh);
    } else {
        compositor_present_rect(ux, uy, uw, uh);
    }
    compositor_frame_done(true);
    g_my_presents = doublebuf_present_count();
    fs_add(FS_FRAME, fs_frame0);
    fs_frame_done();
}

// Кадр сцены: рабочий стол (из кэша) + окна + оверлеи + курсор.
static void render_scene(void) {
    const gfx_target_t* tgt = compositor_target();
    if (!tgt) return;

    uint64_t fs_frame0 = fs_now();
    g_scene_dirty = false;
    g_last_scene_ms = timer_get_uptime_ms();

    compositor_begin_frame();
    // Анимации окон продвигаем строго раз за кадр. Анимированное окно меняет
    // размер и прозрачность — проще и надёжнее вывести кадр целиком.
    bool full = compositor_update_animations();
    bool win_anim = full;            // [48.2] идёт анимация какого-то окна

    // Позицию прежнего курсора запоминаем до того, как сцена его затрёт.
    int cx = 0, cy = 0, cw = 0, ch = 0;
    bool cur_had = cursor_overlay_rect(&cx, &cy, &cw, &ch);
    cursor_overlay_forget();

    if (desk_prepare(tgt)) full = true;
    if (g_desk_no_mem) {
        desktop_draw(tgt);
    } else {
        // Весь слой — одной копией: rep movsq (на современных CPU микрокод
        // разворачивает его в широкие пересылки).
        size_t qwords = ((size_t)tgt->height * (size_t)tgt->pitch) / 8;
        void* dst = tgt->pixels; const void* src = g_desk.pixels;
        __asm__ __volatile__("rep movsq" : "+D"(dst), "+S"(src), "+c"(qwords) : : "memory");
    }

    // [48.2] Окно, целиком закрытое непрозрачным workspace-окном выше, не
    // рисуем вовсе (терминал под браузером: его on_draw и копия 2 Мпикс шли
    // в никуда). cover — верхнее такое окно; всё, что ниже и внутри него, —
    // пропускаем.
    window_t* cover = 0;
    for (window_t* w = g_tail; w; w = w->prev) {
        if (!w->visible || compositor_is_animating(w)) continue;
        if (w->chrome != WINDOW_CHROME_CLASSIC && w->opacity == 255) { cover = w; break; }
    }
    // [48.2] Пока идёт анимация (оверлей, окно, вход на стол), неподвижные
    // окна показываются готовыми буферами — без on_draw на каждом шаге.
    compositor_set_frozen(win_anim || fx_motion_active() || enter_active());

    // Окна снизу вверх -- через compositor (тени, AA, alpha). Каждое окно само
    // помечает свой прямоугольник (compositor_mark_dirty).
    bool below_cover = cover != 0;
    for (window_t* w = g_head; w; w = w->next) {
        if (w == cover) below_cover = false;
        if (!w->visible && !compositor_is_animating(w)) continue;
        if (below_cover && !compositor_is_animating(w) &&
            rect_inside(w->x - 16, w->y - 16, w->width + 32,
                        window_title_height(w) + w->height + 40,
                        cover->x, cover->y, cover->width, cover->height)) continue;
        compositor_render_window(w, tgt);
    }
    compositor_set_frozen(false);

    // Drawer/modal/toast, Beacon, Constellation, MIND — выше окон. Их
    // прямоугольники WM не знает: пока что-то из них открыто, кадр — целиком.
    // [48.2] «Открыто» теперь включает и исчезающий оверлей (*_visible).
    if (ui_overlay_visible() || beacon_visible() || constellation_visible() ||
        mind_widget_visible()) full = true;
    ui_overlay_draw(tgt);
    beacon_draw(tgt);
    constellation_draw(tgt);
    mind_widget_draw(tgt);
    if (enter_draw(tgt)) full = true;
    fs_add(FS_SCENE, fs_frame0);

    uint64_t fs_cur0 = fs_now();
    const mouse_state_t* m = mouse_get_state();
    if (g_hw_cursor) virtio_gpu_cursor_move(m->x, m->y);
    else             cursor_overlay_show(tgt, m->x, m->y);
    fs_add(FS_CURSOR, fs_cur0);

    // Вывод: изменившаяся область = окна этого кадра + окна прошлого кадра
    // (+ старый и новый курсор). Если хоть что-то из этого «весь экран» —
    // один полный вывод.
    int bx = 0, by = 0, bw = 0, bh = 0;
    if (!compositor_frame_bbox(&bx, &by, &bw, &bh)) full = true;
    bool present_full = full || g_prev_full;
    if (present_full) {
        compositor_present_full();
    } else {
        int ux = bx, uy = by, uw = bw, uh = bh;
        rect_union(&ux, &uy, &uw, &uh, g_prev_x, g_prev_y, g_prev_w, g_prev_h);
        int nx = 0, ny = 0, nw = 0, nh = 0;
        bool cur_has = cursor_overlay_rect(&nx, &ny, &nw, &nh);
        // Курсор: внутри области окон — уже покрыт; иначе отдельными кусками.
        if (uw > 0 && uh > 0) compositor_present_rect(ux, uy, uw, uh);
        if (cur_had && !rect_inside(cx, cy, cw, ch, ux, uy, uw, uh))
            compositor_present_rect(cx, cy, cw, ch);
        if (cur_has && !rect_inside(nx, ny, nw, nh, ux, uy, uw, uh))
            compositor_present_rect(nx, ny, nw, nh);
    }
    compositor_frame_done(!present_full);
    g_prev_full = full;
    g_prev_x = bx; g_prev_y = by; g_prev_w = bw; g_prev_h = bh;
    g_my_presents = doublebuf_present_count();

    fs_add(FS_FRAME, fs_frame0);
    fs_frame_done();
}

// [48.2] Насос кадров (см. wm.h).
void wm_pump(void) {
    if (!compositor_active()) return;
    mouse_poll();
    while (keyboard_has_char()) {
        char c = keyboard_get_char();
        if (c == 27) net_cancel();
    }
    if (mouse_has_event()) {
        mouse_clear_event();
        // Кнопку запоминаем, но щелчок не раздаём: иначе нажатие, сделанное во
        // время загрузки, «сработало» бы фронтом сразу после неё.
        g_prev_left = (mouse_get_state()->buttons & MOUSE_BTN_LEFT) != 0;
        (void)mouse_take_wheel();
    }
    if (g_hw_cursor) virtio_gpu_cursor_flush();
    render_scene();
}

// Запасной путь без композитора (doublebuf есть, compositor_init не прошёл):
// как до 41.2 — вся сцена и полный вывод на каждый кадр.
static void render_legacy(void) {
    const gfx_target_t* tgt = doublebuf_target();
    if (!tgt) return;
    uint64_t fs_frame0 = fs_now();
    desktop_draw(tgt);
    for (window_t* w = g_head; w; w = w->next) {
        if (!w->visible) continue;
        window_render(w, tgt);
    }
    ui_overlay_draw(tgt);
    beacon_draw(tgt);
    constellation_draw(tgt);
    mind_widget_draw(tgt);
    fs_add(FS_SCENE, fs_frame0);
    const mouse_state_t* m = mouse_get_state();
    cursor_draw(tgt, m->x, m->y);
    doublebuf_swap();
    fs_add(FS_FRAME, fs_frame0);
    fs_frame_done();
}

// Доставляем все накопленные клавиатурные символы в focused window
// (если у него есть on_key callback). Возвращает true если был ввод.
static bool process_keyboard(void) {
    bool got = false;
    while (keyboard_has_char()) {
        char c = keyboard_get_char();
        if (!c) break;
        // [этап 49] Ctrl+Space открывает MIND откуда угодно; пока панель
        // открыта, весь ввод её.
        if (c == KBD_KEY_MIND) { mind_widget_toggle(); got = true; continue; }
        if (mind_widget_handle_key(c)) { got = true; continue; }
        if (constellation_handle_key(c)) { got = true; continue; }
        if (beacon_handle_key(c)) { got = true; continue; }
        if (c == KBD_KEY_SUPER_TAB) { constellation_show(); got = true; continue; }
        if (c == KBD_KEY_SUPER_BEACON) { beacon_show(); got = true; continue; }
        if (c == KBD_KEY_SUPER_PULSE) { (void)app_term_open(); got = true; continue; }
        if (c == KBD_KEY_SUPER_HOME) {
            window_t* f = wm_focused();
            if (f) f->visible = false;
            got = true;
            continue;
        }
        if (ui_overlay_handle_key(c)) { got = true; continue; }
        window_t* f = wm_focused();
        if (f && f->visible && f->on_key) f->on_key(f, c);
        else desktop_handle_key(c);
        got = true;
    }
    return got;
}

void wm_main_loop(void) {
    serial_puts("[wm] main loop start\n");

    uint64_t last = 0;          // время последнего кадра сцены, тики PIT
    bool     first = true;
    fs_init();                  // [41.2] perf.conf: отчёт счётчиков в serial
    hw_cursor_setup();          // [41.2-D] после fs_init: читает perf.conf
    if (fs_conf_has("diskbench=1")) disk_bench_report();   // [41.2-F] замер диска
    {
        // [48.2] perf.conf `dhcplease=N`: потолок срока аренды DHCP в секундах,
        // чтобы продление (T1) наступало за секунды — для теста.
        static char v[16];
        if (fs_conf_value("animslow=", v, sizeof(v))) {
            uint32_t n = 0;
            for (int i = 0; v[i] >= '0' && v[i] <= '9'; i++) n = n * 10 + (uint32_t)(v[i] - '0');
            ui_motion_set_slow(n);
        }
        if (fs_conf_value("dhcplease=", v, sizeof(v))) {
            uint32_t n = 0;
            for (int i = 0; v[i] >= '0' && v[i] <= '9'; i++) n = n * 10 + (uint32_t)(v[i] - '0');
            if (n) dhcp_set_lease_cap(n);
        }
    }
    {
        static char url[160];                              // [41.2-G] замер сети
        if (fs_conf_value("netbench=", url, sizeof(url))) net_bench_report(url);
    }
    enter_begin();              // [48.2] переход «экран входа -> рабочий стол»
    for (;;) {
        fs_loop_tick();         // закрывает секундное окно счётчиков
        uint64_t fs_in0 = fs_now();

        // Polling-fallback для мыши: читаем 8042 буфер вручную (на случай
        // если IRQ12 не доставляется через UEFI/IOAPIC routing). Сама
        // функция внутри делает CLI/STI, чтобы не гоняться с mouse_irq.
        mouse_poll();

        bool kbd_event = process_keyboard();

        // [этап 49] MIND: забрать токены с ядра-дирижёра / догрузить модель.
        mind_widget_tick();

        // [48.2] Сеть: повторы DHCP, продление аренды, запасная статика.
        netcfg_tick();

        // [50.2] phantom: вывод консоли гостя; без гостя — одна проверка.
        // [50.5] Он же читает образ гостя в память по частям; следом — окно
        // гостя (экран, захват клавиатуры).
        phantom_tick();
        app_phantom_tick();

        // [51.3] Программа WebAssembly на другом ядре: то, чего ему нельзя (диск,
        // окно, память), делаем за неё здесь; без программы — одна проверка.
        rune_tick();
        // [51.4] Окно Rune: конец программы, новый вывод, счётчики.
        app_rune_tick();

        // [41.2-D] Дослать позицию курсора, если очередь курсора была занята.
        if (g_hw_cursor) virtio_gpu_cursor_flush();

        // HOTFIX (15.05.2026): только РИД-ОНЛИ peek — прогресс анимаций
        // двигает compositor_update_animations() внутри кадра сцены.
        bool anim_active = compositor_active() && compositor_peek_animation_active();

        // Окна, просящие непрерывный рендер (3D-куб, видео, игры), и видимые
        // окна вообще: приложения рисуют содержимое в on_draw и о своих
        // изменениях не сообщают, поэтому при видимых окнах сцену обновляем
        // раз в 100 мс, как до 41.2.
        bool want_animated = false, live = false;
        for (window_t* win = wm_iterate(0); win; win = wm_iterate(win)) {
            if (!win->visible) continue;
            live = true;
            if (win->wants_continuous_redraw) { want_animated = true; break; }
        }
        if (ui_overlay_visible() || beacon_visible() || constellation_visible() ||
            mind_widget_visible()) live = true;

        // [48.2] Движение на экране: каждую проверку вызываем безусловно —
        // *_motion_active() сами доводят исчезновение оверлея до конца.
        bool m_overlay = ui_overlay_motion_active();
        bool m_beacon  = beacon_motion_active();
        bool m_const   = constellation_motion_active();
        bool m_rail    = ui_components_motion_active();
        bool m_mind    = mind_widget_active();
        bool motion    = anim_active || m_overlay || m_beacon || m_const || m_rail ||
                         m_mind || enter_active();
        bool frame_due = timer_get_uptime_ms() - g_last_scene_ms >= WM_FRAME_MS;

        // [41.2] Флаг события мыши снимаем ДО чтения её состояния: пакет,
        // пришедший позже, снова его поднимет. Раньше флаг снимался после
        // обработки, и пакет, попавший в это окно, терялся до следующего
        // события — при кадрах «только курсор» курсор застревал бы не там.
        bool moved = mouse_has_event();
        bool scene = first || kbd_event || want_animated || g_scene_dirty ||
                     (motion && frame_due);
        if (moved) {
            mouse_clear_event();
            if (process_mouse()) scene = true;
        }
        // Экраном пользовался кто-то кроме WM (экран блокировки, шторка):
        // сохранённый фон курсора и «прошлая область» недействительны.
        if (compositor_active() && doublebuf_present_count() != g_my_presents) {
            cursor_overlay_forget();
            g_prev_full = true;
            scene = true;
        }
        // Часы, подсветка rail, тема — через ключ кэша рабочего стола.
        if (!scene && compositor_active() && desk_stale()) scene = true;
        // Обновление 10 кадров/с — только когда на экране есть «живое».
        if (!scene && live && pit_ticks() - last >= 10) scene = true;

        if (scene || moved) fs_add(FS_INPUT, fs_in0);
        if (!compositor_active()) {
            // Без композитора — старое поведение: кадр по событию или раз в 100 мс.
            if (scene || moved || pit_ticks() - last >= 10) {
                render_legacy();
                reap_closed_windows();
                last = pit_ticks();
            }
        } else if (scene) {
            render_scene();
            reap_closed_windows();
            last = pit_ticks();
        } else if (moved) {
            render_cursor();
        }
        first = false;

        // Время от hlt до следующего пробуждения — простой BSP (в него же
        // попадает обработчик разбудившего прерывания: он короткий).
        // [41.2] С таймером LAPIC BSP просыпается каждую миллисекунду. Гонять
        // на каждый тик весь проход цикла (опрос 8042 — это порты, под KVM
        // выход в гипервизор) незачем: спим дальше, пока не придёт настоящее
        // прерывание (ввод, virtio) или не наступит граница 10 мс — на ней
        // цикл работает как раньше (анимации, MIND, обновление окон).
        uint64_t fs_sl0 = fs_now();
        uint64_t ev = irq_event_count();
        // [50.5] Пока phantom читает образ гостя, не спим вовсе: чтение идёт
        // порциями из этого цикла, и сон до границы 10 мс на медленном диске
        // (IDE) растянул бы его в полтора раза. Кадры и ввод при этом идут —
        // порция чтения ограничена по времени.
        if (phantom_loading()) continue;
        // [51.3] Программа ждёт услугу — не спим. Проверка и hlt — с запретом
        // прерываний между ними (sti действует после следующей команды): иначе
        // её «разбуди меня», пришедшее в этот промежуток, пропало бы до
        // следующего тика.
        __asm__ __volatile__("cli");
        if (rune_wants_service()) { __asm__ __volatile__("sti"); continue; }
        // [48.2] Пока идёт анимация, спим не до границы 10 мс, а до срока
        // следующего кадра (16 мс от начала предыдущего).
        do {
            __asm__ __volatile__("sti; hlt");
        } while (timer_ms_active() && irq_event_count() == ev &&
                 (motion ? timer_get_uptime_ms() - g_last_scene_ms < WM_FRAME_MS
                         : (timer_get_uptime_ms() % 10) != 0));
        fs_add(FS_SLEEP, fs_sl0);
    }
}
