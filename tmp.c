/*
 * tiler.c - i3-like tiling window manager for Windows (prototype)
 *
 * Steps implemented:
 *   1. Enumerate and filter tileable windows
 *   2. Layout + SetWindowPos/DeferWindowPos, DWM border compensation,
 *      minimized/maximized handling
 *   3. SetWinEventHook + debounced retiling + clean shutdown
 *   4. Container tree (splith/splitv), focus, insert/remove/collapse
 *   5. WH_KEYBOARD_LL global hotkeys + workspaces (Alt+N jump,
 *      Alt+Shift+N move window)
 *
 * Build (MinGW):
 *   gcc -std=c90 -pedantic -Wall -o tiler.exe tiler.c -ldwmapi -luser32
 */

#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef EVENT_OBJECT_CLOAKED
#define EVENT_OBJECT_CLOAKED   0x8017
#define EVENT_OBJECT_UNCLOAKED 0x8018
#endif

#define MAX_WINDOWS     256
#define MAX_CHILDREN    16
#define NUM_WORKSPACES  9
#define DEBOUNCE_MS     50
#define NUM_HOOKS       4

#define MOD_ALT_BIT   0x01
#define MOD_SHIFT_BIT 0x02

/* ---------- types ---------- */

typedef struct {
    HWND hwnd;
    WCHAR title[256];
    int workspace;
} WindowInfo;

typedef struct {
    WindowInfo items[MAX_WINDOWS];
    int count;
} WindowList;

typedef struct {
    int left, top, right, bottom;
} Insets;

typedef enum { NODE_WINDOW, NODE_SPLIT } NodeType;
typedef enum { SPLIT_H, SPLIT_V } Orientation;

typedef struct Node {
    NodeType type;
    struct Node *parent;

    HWND hwnd;              /* valid when type == NODE_WINDOW */
    int workspace;          /* valid when type == NODE_WINDOW */

    Orientation orientation; /* valid when type == NODE_SPLIT */
    struct Node *children[MAX_CHILDREN];
    double ratio[MAX_CHILDREN];
    int child_count;
} Node;

/* ---------- globals ---------- */

static Node *g_root = NULL;
static Node *g_focused = NULL;
static int g_active_workspace = 0;

static UINT_PTR g_timer = 0;
static DWORD g_main_thread = 0;

static HHOOK g_keyboard_hook = NULL;
static unsigned int g_mods = 0;

/* ---------- forward declarations ---------- */

static int should_tile(HWND hwnd);
static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lParam);
static Insets get_border_insets(HWND hwnd);
static void apply_layout(void);
static void retile(void);
static void schedule_retile(void);
static void CALLBACK timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time);
static void CALLBACK win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                     LONG idObject, LONG idChild,
                                     DWORD thread, DWORD time);
static BOOL WINAPI ctrl_handler(DWORD type);
static LRESULT CALLBACK keyboard_proc(int code, WPARAM wParam, LPARAM lParam);
static void dispatch_key(DWORD vk);
static void set_split_orientation(Orientation o);
static void focus_cycle(int direction);
static void close_focused_window(void);
static void switch_workspace(int ws);
static void move_focused_to_workspace(int ws);
static Node *find_first_on_workspace(Node *node, int ws);

/* ---------- tree operations ---------- */

static Node *node_new(NodeType type)
{
    Node *n;

    n = (Node *)malloc(sizeof(Node));
    if (n == NULL) return NULL;
    n->type = type;
    n->parent = NULL;
    n->hwnd = NULL;
    n->workspace = 0;
    n->orientation = SPLIT_H;
    n->child_count = 0;
    return n;
}

static void redistribute_ratios(Node *container)
{
    int i;
    if (container->child_count == 0) return;
    for (i = 0; i < container->child_count; i++) {
        container->ratio[i] = 1.0 / container->child_count;
    }
}

static void add_child(Node *container, Node *child)
{
    if (container->child_count >= MAX_CHILDREN) return;
    child->parent = container;
    container->children[container->child_count] = child;
    container->child_count++;
    redistribute_ratios(container);
}

static void remove_child(Node *container, Node *child)
{
    int i, idx;

    idx = -1;
    for (i = 0; i < container->child_count; i++) {
        if (container->children[i] == child) { idx = i; break; }
    }
    if (idx == -1) return;

    for (i = idx; i < container->child_count - 1; i++) {
        container->children[i] = container->children[i + 1];
    }
    container->child_count--;
    redistribute_ratios(container);
}

static Node *find_leaf(Node *node, HWND hwnd)
{
    int i;
    Node *found;

    if (node == NULL) return NULL;
    if (node->type == NODE_WINDOW) {
        return (node->hwnd == hwnd) ? node : NULL;
    }
    for (i = 0; i < node->child_count; i++) {
        found = find_leaf(node->children[i], hwnd);
        if (found != NULL) return found;
    }
    return NULL;
}

static Node *first_leaf(Node *node)
{
    if (node == NULL) return NULL;
    if (node->type == NODE_WINDOW) return node;
    if (node->child_count == 0) return NULL;
    return first_leaf(node->children[0]);
}

static void collapse_empty(Node *container)
{
    Node *parent;

    if (container == NULL || container == g_root) return;
    if (container->child_count > 0) return;

    parent = container->parent;
    remove_child(parent, container);
    free(container);
    collapse_empty(parent);
}

static void insert_window(HWND hwnd)
{
    Node *leaf;
    Node *container;

    leaf = node_new(NODE_WINDOW);
    if (leaf == NULL) return;
    leaf->hwnd = hwnd;
    leaf->workspace = g_active_workspace;

    if (g_root == NULL) {
        g_root = node_new(NODE_SPLIT);
        if (g_root == NULL) { free(leaf); return; }
        g_root->orientation = SPLIT_H;
    }

    container = (g_focused != NULL) ? g_focused->parent : g_root;
    add_child(container, leaf);
    g_focused = leaf;
}

static void remove_window(HWND hwnd)
{
    Node *n;
    Node *parent;

    n = find_leaf(g_root, hwnd);
    if (n == NULL) return;

    parent = n->parent;
    remove_child(parent, n);

    if (g_focused == n) {
        g_focused = first_leaf(g_root);
    }
    free(n);
    collapse_empty(parent);
}

static void collect_leaves(Node *node, HWND *out, int *count)
{
    int i;

    if (node == NULL) return;
    if (node->type == NODE_WINDOW) {
        out[*count] = node->hwnd;
        (*count)++;
        return;
    }
    for (i = 0; i < node->child_count; i++) {
        collect_leaves(node->children[i], out, count);
    }
}

static int list_contains(WindowList *list, HWND hwnd)
{
    int i;
    for (i = 0; i < list->count; i++) {
        if (list->items[i].hwnd == hwnd) return 1;
    }
    return 0;
}

static void sync_tree(WindowList *list)
{
    static HWND leaf_hwnds[MAX_WINDOWS];
    int leaf_count;
    int i;

    leaf_count = 0;
    collect_leaves(g_root, leaf_hwnds, &leaf_count);

    for (i = 0; i < leaf_count; i++) {
        if (!list_contains(list, leaf_hwnds[i])) {
            remove_window(leaf_hwnds[i]);
        }
    }
    for (i = 0; i < list->count; i++) {
        if (find_leaf(g_root, list->items[i].hwnd) == NULL) {
            insert_window(list->items[i].hwnd);
        }
    }
}

/* ---------- window enumeration / filtering ---------- */

static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lParam)
{
    WindowList *list;

    list = (WindowList *)lParam;

    if (should_tile(hwnd) && list->count < MAX_WINDOWS) {
        list->items[list->count].hwnd = hwnd;
        GetWindowTextW(hwnd, list->items[list->count].title, 256);
        list->count++;
    }
    return TRUE;
}

static int should_tile(HWND hwnd)
{
    LONG style;
    LONG ex_style;
    BOOL cloaked;
    HRESULT hr;

    if (!IsWindowVisible(hwnd)) return 0;
    if (IsIconic(hwnd)) return 0; /* minimized: out of the layout */

    style = GetWindowLongW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) != WS_CAPTION) return 0;

    ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex_style & WS_EX_TOOLWINDOW) return 0;

    if (GetWindow(hwnd, GW_OWNER) != NULL) return 0;

    cloaked = FALSE;
    hr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (SUCCEEDED(hr) && cloaked) return 0;

    if (GetWindowTextLengthW(hwnd) == 0) return 0;

    return 1;
}

/* ---------- layout ---------- */

static Insets get_border_insets(HWND hwnd)
{
    RECT raw;
    RECT visual;
    Insets insets;
    HRESULT hr;

    GetWindowRect(hwnd, &raw);
    hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                &visual, sizeof(visual));
    if (FAILED(hr)) {
        insets.left = insets.top = insets.right = insets.bottom = 0;
        return insets;
    }

    insets.left = visual.left - raw.left;
    insets.top = visual.top - raw.top;
    insets.right = raw.right - visual.right;
    insets.bottom = raw.bottom - visual.bottom;
    return insets;
}

static void restore_zoomed(Node *node)
{
    int i;
    if (node == NULL) return;
    if (node->type == NODE_WINDOW) {
        if (node->workspace == g_active_workspace && IsZoomed(node->hwnd)) {
            ShowWindow(node->hwnd, SW_RESTORE);
        }
        return;
    }
    for (i = 0; i < node->child_count; i++) {
        restore_zoomed(node->children[i]);
    }
}

static void layout_node(Node *node, RECT rect, HDWP *hdwp, int gap)
{
    int i, n, pos, extent, avail;
    Insets insets;
    RECT child_rect;

    if (node->type == NODE_WINDOW) {
        if (node->workspace != g_active_workspace) {
            return; /* hidden on another workspace: don't position it */
        }

        insets = get_border_insets(node->hwnd);
        *hdwp = DeferWindowPos(
            *hdwp, node->hwnd, NULL,
            rect.left - insets.left, rect.top - insets.top,
            (rect.right - rect.left) + insets.left + insets.right,
            (rect.bottom - rect.top) + insets.top + insets.bottom,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }

    n = node->child_count;
    if (n == 0) return;

    if (node->orientation == SPLIT_H) {
        avail = (rect.right - rect.left) - gap * (n - 1);
        pos = rect.left;
        for (i = 0; i < n; i++) {
            extent = (i == n - 1) ? (rect.right - pos)
                                   : (int)(node->ratio[i] * avail);
            child_rect.left = pos;
            child_rect.right = pos + extent;
            child_rect.top = rect.top;
            child_rect.bottom = rect.bottom;
            layout_node(node->children[i], child_rect, hdwp, gap);
            if (*hdwp == NULL) return;
            pos += extent + gap;
        }
    } else {
        avail = (rect.bottom - rect.top) - gap * (n - 1);
        pos = rect.top;
        for (i = 0; i < n; i++) {
            extent = (i == n - 1) ? (rect.bottom - pos)
                                   : (int)(node->ratio[i] * avail);
            child_rect.top = pos;
            child_rect.bottom = pos + extent;
            child_rect.left = rect.left;
            child_rect.right = rect.right;
            layout_node(node->children[i], child_rect, hdwp, gap);
            if (*hdwp == NULL) return;
            pos += extent + gap;
        }
    }
}

static void apply_layout(void)
{
    RECT work_area, inner;
    HDWP hdwp;
    int gap;

    gap = 8;
    if (g_root == NULL || g_root->child_count == 0) return;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0)) return;

    restore_zoomed(g_root);

    inner.left = work_area.left + gap;
    inner.top = work_area.top + gap;
    inner.right = work_area.right - gap;
    inner.bottom = work_area.bottom - gap;

    hdwp = BeginDeferWindowPos(32);
    if (hdwp == NULL) return;
    layout_node(g_root, inner, &hdwp, gap);
    if (hdwp != NULL) EndDeferWindowPos(hdwp);
}

/* ---------- retiling / event hooks (step 3) ---------- */

static void retile(void)
{
    static WindowList list;

    list.count = 0;
    EnumWindows(enum_proc, (LPARAM)&list);
    sync_tree(&list);
    apply_layout();
}

static void schedule_retile(void)
{
    g_timer = SetTimer(NULL, g_timer, DEBOUNCE_MS, timer_proc);
}

static void CALLBACK timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
    (void)hwnd; (void)msg; (void)time;
    KillTimer(NULL, id);
    g_timer = 0;
    retile();
}

static void CALLBACK win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                     LONG idObject, LONG idChild,
                                     DWORD thread, DWORD time)
{
    (void)hook; (void)event; (void)thread; (void)time;

    if (hwnd == NULL || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) {
        return;
    }
    if (IsWindow(hwnd) && GetAncestor(hwnd, GA_PARENT) != GetDesktopWindow()) {
        return;
    }
    schedule_retile();
}

static BOOL WINAPI ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        PostThreadMessage(g_main_thread, WM_QUIT, 0, 0);
        return TRUE;
    }
    return FALSE;
}

/* ---------- commands used by hotkeys (step 4/5) ---------- */

static void set_split_orientation(Orientation o)
{
    if (g_focused != NULL && g_focused->parent != NULL) {
        g_focused->parent->orientation = o;
        apply_layout();
    }
}

static void focus_cycle(int direction)
{
    Node *p;
    int idx, i, next;

    if (g_focused == NULL || g_focused->parent == NULL) return;
    p = g_focused->parent;
    if (p->child_count < 2) return;

    idx = -1;
    for (i = 0; i < p->child_count; i++) {
        if (p->children[i] == g_focused) { idx = i; break; }
    }
    if (idx == -1) return;

    next = (idx + direction + p->child_count) % p->child_count;
    g_focused = p->children[next];
    SetForegroundWindow(g_focused->hwnd);
}

static void close_focused_window(void)
{
    if (g_focused != NULL) {
        PostMessage(g_focused->hwnd, WM_CLOSE, 0, 0);
    }
}

/* ---------- workspaces (step 5) ---------- */

static void set_workspace_visibility(Node *node, int target_ws)
{
    int i;
    if (node == NULL) return;
    if (node->type == NODE_WINDOW) {
        if (node->workspace == target_ws) {
            ShowWindow(node->hwnd, SW_SHOWNA);
        } else {
            ShowWindow(node->hwnd, SW_HIDE);
        }
        return;
    }
    for (i = 0; i < node->child_count; i++) {
        set_workspace_visibility(node->children[i], target_ws);
    }
}

static Node *find_first_on_workspace(Node *node, int ws)
{
    int i;
    Node *found;

    if (node == NULL) return NULL;
    if (node->type == NODE_WINDOW) {
        return (node->workspace == ws) ? node : NULL;
    }
    for (i = 0; i < node->child_count; i++) {
        found = find_first_on_workspace(node->children[i], ws);
        if (found != NULL) return found;
    }
    return NULL;
}

static void switch_workspace(int ws)
{
    if (ws < 0 || ws >= NUM_WORKSPACES || ws == g_active_workspace) return;

    g_active_workspace = ws;
    set_workspace_visibility(g_root, ws);
    apply_layout();

    g_focused = find_first_on_workspace(g_root, ws);
    if (g_focused != NULL) {
        SetForegroundWindow(g_focused->hwnd);
    }
}

static void move_focused_to_workspace(int ws)
{
    if (g_focused == NULL) return;
    if (ws < 0 || ws >= NUM_WORKSPACES || ws == g_active_workspace) return;

    g_focused->workspace = ws;
    ShowWindow(g_focused->hwnd, SW_HIDE);
    g_focused = find_first_on_workspace(g_root, g_active_workspace);
    apply_layout();
}

/* ---------- low-level keyboard hook (step 5) ---------- */

static int vk_to_workspace(DWORD vk)
{
    if (vk >= '1' && vk <= '9') {
        return (int)(vk - '1');
    }
    return -1;
}

static int is_bound_key(DWORD vk)
{
    switch (vk) {
    case 'H': case 'V': case 'J': case 'K': case 'Q':
        return 1;
    default:
        return vk_to_workspace(vk) != -1;
    }
}

static void dispatch_key(DWORD vk)
{
    int ws;

    if (!(g_mods & MOD_ALT_BIT)) return;

    ws = vk_to_workspace(vk);
    if (ws != -1) {
        if (g_mods & MOD_SHIFT_BIT) {
            move_focused_to_workspace(ws);
        } else {
            switch_workspace(ws);
        }
        return;
    }

    switch (vk) {
    case 'H': set_split_orientation(SPLIT_H); break;
    case 'V': set_split_orientation(SPLIT_V); break;
    case 'J': focus_cycle(1); break;
    case 'K': focus_cycle(-1); break;
    case 'Q': close_focused_window(); break;
    default: break;
    }
}

static LRESULT CALLBACK keyboard_proc(int code, WPARAM wParam, LPARAM lParam)
{
    KBDLLHOOKSTRUCT *info;
    int is_keydown;
    int swallow;

    if (code != HC_ACTION) {
        return CallNextHookEx(g_keyboard_hook, code, wParam, lParam);
    }

    info = (KBDLLHOOKSTRUCT *)lParam;
    is_keydown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

    if (info->vkCode == VK_MENU || info->vkCode == VK_LMENU || info->vkCode == VK_RMENU) {
        if (is_keydown) g_mods |= MOD_ALT_BIT; else g_mods &= ~MOD_ALT_BIT;
    }
    if (info->vkCode == VK_SHIFT || info->vkCode == VK_LSHIFT || info->vkCode == VK_RSHIFT) {
        if (is_keydown) g_mods |= MOD_SHIFT_BIT; else g_mods &= ~MOD_SHIFT_BIT;
    }

    swallow = 0;
    if (is_keydown && (g_mods & MOD_ALT_BIT)) {
        swallow = is_bound_key(info->vkCode);
    }

    if (is_keydown) {
        dispatch_key(info->vkCode);
    }

    if (swallow) {
        return 1;
    }
    return CallNextHookEx(g_keyboard_hook, code, wParam, lParam);
}

/* ---------- main ---------- */

int main(void)
{
    static const DWORD ranges[NUM_HOOKS][2] = {
        { EVENT_OBJECT_CREATE,        EVENT_OBJECT_HIDE        },
        { EVENT_OBJECT_CLOAKED,       EVENT_OBJECT_UNCLOAKED    },
        { EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND  },
        { EVENT_SYSTEM_MOVESIZEEND,   EVENT_SYSTEM_MOVESIZEEND  }
    };
    HWINEVENTHOOK hooks[NUM_HOOKS];
    MSG msg;
    int i;

    g_main_thread = GetCurrentThreadId();
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    for (i = 0; i < NUM_HOOKS; i++) {
        hooks[i] = SetWinEventHook(
            ranges[i][0], ranges[i][1],
            NULL,
            win_event_proc,
            0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (hooks[i] == NULL) {
            fprintf(stderr, "SetWinEventHook %d failed\n", i);
        }
    }

    g_keyboard_hook = SetWindowsHookEx(WH_KEYBOARD_LL, keyboard_proc, NULL, 0);
    if (g_keyboard_hook == NULL) {
        fprintf(stderr, "SetWindowsHookEx failed\n");
        return 1;
    }

    retile();
    printf("Running. Alt+H/V split, Alt+J/K focus, Alt+Q close,\n");
    printf("Alt+1-9 switch workspace, Alt+Shift+1-9 move window. Ctrl+C to quit.\n");

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(g_keyboard_hook);
    for (i = 0; i < NUM_HOOKS; i++) {
        if (hooks[i] != NULL) {
            UnhookWinEvent(hooks[i]);
        }
    }
    return 0;
}
