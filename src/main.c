#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>

#define MAX_HWND 256
#define HWND_TITLE_LEN 256

struct insets {
    int left, top, right, bottom;
};

struct hwnd_info {
    HWND hwnd;
    CHAR title[HWND_TITLE_LEN];
};

struct hwnd_list {
    struct hwnd_info items[MAX_HWND];
    int count;
};

int get_tileable_hwnd()
{
    return -1;
}

int get_hwnd_title(HWND hwnd)
{
    return -1;
}

int should_tile(HWND hwnd)
{
    LONG style;
    LONG ex_style;
    HWND owner;
    BOOL cloaked;
    HRESULT hr;
    int title_len;

    if (!IsWindowVisible(hwnd)) return 0;

    style = GetWindowLong(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) != WS_CAPTION) return 0;

    ex_style = GetWindowLong(hwnd, GWL_EXSTYLE);
    if (ex_style & WS_EX_TOOLWINDOW) return 0;

    owner = GetWindow(hwnd, GW_OWNER);
    if (owner != NULL) return 0;

    cloaked = FALSE;
    hr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (SUCCEEDED(hr) && cloaked) return 0;

    title_len = GetWindowTextLength(hwnd);
    if (title_len == 0) return 0;

    return 1;
}

static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lParam)
{
    struct hwnd_list *list;
    list = (struct hwnd_list *)lParam;

    if (should_tile(hwnd)) {
        if (list->count < MAX_HWND) {
            list->items[list->count].hwnd = hwnd;
            GetWindowText(hwnd, list->items[list->count].title, HWND_TITLE_LEN);
            list->count++;
        }
    }

    return TRUE;
}

struct insets get_border_insets(HWND hwnd)
{
    RECT raw, visual;
    struct insets insets;
    HRESULT hr;

    GetWindowRect(hwnd, &raw);

    hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &visual,
                               sizeof(visual));

    if (FAILED(hr)) {
        insets.left = 0;
        insets.top = 0;
        insets.right = 0;
        insets.bottom = 0;
    } else {
        insets.left = visual.left - raw.left;
        insets.top = visual.top - raw.top;
        insets.right = raw.right - visual.right;
        insets.bottom = raw.bottom - visual.bottom;
    }
    return insets;
}

void apply_two_column_layout(struct hwnd_list *list)
{
    RECT work_area;
    HDWP hdwp;
    int i, col_width, gap;

    gap = 8;

    if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &work_area, 0)) {
        printf("SystemParametersInfo()\n");
        return;
    }

    col_width = ((work_area.right - work_area.left) - (gap * 3)) / 2;

    hdwp = BeginDeferWindowPos(list->count);

    if (hdwp == NULL) {
        printf("BeginDeferWindowPos()\n");
        return;
    }

    for (i = 0; i < list->count; i++) {
        HWND hwnd = list->items[i].hwnd;
        if (IsIconic(hwnd) || IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    }

    for (i = 0; i < list->count; i++) {
        HWND hwnd;
        struct insets insets;
        int tgt_left, tgt_top, tgt_width, tgt_height;

        hwnd = list->items[i].hwnd;
        insets = get_border_insets(hwnd);

        tgt_left = work_area.left + gap;
        if (i % 2 != 0) tgt_left += col_width + gap;

        tgt_top = work_area.top + gap;
        tgt_width = col_width;
        tgt_height = (work_area.bottom - work_area.top) - (gap + gap);

        hdwp = DeferWindowPos(hdwp,
                              hwnd,
                              NULL,
                              tgt_left - insets.left,
                              tgt_top - insets.top,
                              tgt_width + insets.left + insets.right,
                              tgt_height + insets.top + insets.bottom,
                              SWP_NOZORDER | SWP_NOACTIVATE);

        if (hdwp == NULL) {
            printf("DeferWindowPos()\n");
            return;
        }
    }

    if (!EndDeferWindowPos(hdwp)) {
        printf("EndDeferWindowPos()\n");
        return;
    }
}

int main(void)
{
    struct hwnd_list list;
    int i;

    list.count = 0;
    EnumWindows(enum_proc, (LPARAM)&list);

    for (i = 0; i < list.count; i++) {
        printf("  %p  %s\n", (void *)list.items[i].hwnd, list.items[i].title);
    }

    apply_two_column_layout(&list);

    return 0;
}
