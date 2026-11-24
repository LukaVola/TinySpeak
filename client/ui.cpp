#include "ui.h"
#include "mic_icon.h"
#include "palette.h"
#include "network.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <fstream>
#include <utility>

std::wstring windowText(HWND window) {
    int length = GetWindowTextLengthW(window);
    std::wstring result(length + 1, L'\0');
    GetWindowTextW(window, result.data(), length + 1);
    result.resize(length);
    return result;
}

void putText(HWND window, const std::wstring& value) {
    SetWindowTextW(window, value.c_str());
}

HWND control(HWND parent, const wchar_t* type, const wchar_t* label, DWORD style, int id, DWORD extended) {
    auto window = CreateWindowExW(extended, type, label, WS_CHILD | WS_VISIBLE | style,
        0, 0, 20, 20, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return window;
}

void darkFrame(HWND window) {
    const BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(window, 20, &dark, sizeof(dark)))) {
        DwmSetWindowAttribute(window, 19, &dark, sizeof(dark));
    }
}

void fillColor(HDC dc, const RECT& rectangle, COLORREF color) {
    const auto previous = SetDCBrushColor(dc, color);
    FillRect(dc, &rectangle, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetDCBrushColor(dc, previous);
}

void drawTextLine(HDC dc, RECT rectangle, const std::wstring& label, COLORREF color, UINT flags) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, label.c_str(), -1, &rectangle, flags);
}

namespace {
RECT buttonSurface(const DRAWITEMSTRUCT& item) {
    auto rect = item.rcItem;
    fillColor(item.hDC, rect, item.itemState & ODS_FOCUS ? theme::secondary : theme::edge);
    InflateRect(&rect, -1, -1);
    fillColor(item.hDC, rect, item.itemState & ODS_SELECTED ? theme::selected : theme::field);
    return rect;
}
}

void drawButton(const DRAWITEMSTRUCT& item, bool dropdown, const std::wstring& title, const std::wstring& tag) {
    auto rect = buttonSurface(item);
    const auto dc = item.hDC;
    const auto dpi = GetDpiForWindow(item.hwndItem);
    const auto scale = [dpi](int n) { return MulDiv(n, dpi, 96); };
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const auto previousFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0)));
    rect.left += scale(9);
    rect.right -= scale(9);
    if (dropdown) {
        RECT arrow{rect.right - scale(11), rect.top, rect.right, rect.bottom};
        drawTextLine(dc, arrow, L"▾", disabled ? theme::muted : theme::secondary);
        rect.right -= scale(15);
    }
    if (!tag.empty()) {
        SIZE tagSize{};
        GetTextExtentPoint32W(dc, tag.c_str(), static_cast<int>(tag.size()), &tagSize);
        RECT tagRect{std::max(rect.left, rect.right - tagSize.cx), rect.top, rect.right, rect.bottom};
        drawTextLine(dc, tagRect, tag, theme::secondary);
        rect.right = tagRect.left - scale(8);
    }
    if (rect.right > rect.left) {
        const auto label = title.empty() ? windowText(item.hwndItem) : title;
        constexpr UINT flags = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
        drawTextLine(dc, rect, label, disabled ? theme::muted : theme::text,
            flags | (dropdown ? DT_LEFT : DT_CENTER));
    }
    SelectObject(dc, previousFont);
}

void drawRoomsToggle(const DRAWITEMSTRUCT& item, bool collapsed) {
    const auto box = buttonSurface(item);
    const int dpi = GetDpiForWindow(item.hwndItem);
    const int x = (box.left + box.right) / 2, y = (box.top + box.bottom) / 2;
    const int d = MulDiv(4, dpi, 96), h = collapsed ? d : -d;
    auto pen = CreatePen(PS_SOLID, std::max(1, MulDiv(2, dpi, 96)), theme::secondary);
    auto old = SelectObject(item.hDC, pen);
    const POINT points[]{{x - h / 2, y - d}, {x + h / 2, y}, {x - h / 2, y + d}};
    Polyline(item.hDC, points, 3);
    SelectObject(item.hDC, old);
    DeleteObject(pen);
}

void drawMicrophoneButton(const DRAWITEMSTRUCT& item, bool muted) {
    const auto rect = buttonSurface(item);
    const int rows = static_cast<int>(microphonePixels.size());
    const int columns = static_cast<int>(microphonePixels.front().size());
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    const int pixel = std::max(1, std::min({MulDiv(2, GetDpiForWindow(item.hwndItem), 96), width / (columns + 2), height / rows}));
    const LONG x = rect.left + (rect.right - rect.left - columns * pixel) / 2;
    const LONG y = rect.top + (rect.bottom - rect.top - rows * pixel) / 2;
    const auto cell = [&](int col, int row, COLORREF fill) {
        const LONG left = x + col * pixel, top = y + row * pixel;
        fillColor(item.hDC, {left, top, left + pixel, top + pixel}, fill);
    };
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < columns; ++col) {
            if (microphonePixels[row][col] == '1') cell(col, row, theme::text);
        }
        if (muted) cell(columns - row, row, theme::profile[0]);
    }
}

std::wstring percentText(int value) { return std::to_wstring(value) + L"%"; }

void configureSlider(HWND slider, SliderScale scale, int value, int pageStep) {
    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, scale.trackMaximum()));
    SendMessageW(slider, TBM_SETLINESIZE, 0, 1);
    SendMessageW(slider, TBM_SETPAGESIZE, 0, pageStep);
    SendMessageW(slider, TBM_SETPOS, TRUE, scale.position(value));
}

LRESULT drawTrackbar(const NMCUSTOMDRAW& item, COLORREF surface) {
    if (item.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    const auto bar = item.hdr.hwndFrom;
    RECT area{}, track{}, thumb{};
    GetClientRect(bar, &area);
    SendMessageW(bar, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&track));
    SendMessageW(bar, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    fillColor(item.hdc, area, surface);
    const int center = (thumb.top + thumb.bottom) / 2;
    const int position = (thumb.left + thumb.right) / 2;
    const int thickness = MulDiv(3, GetDpiForWindow(bar), 96);
    RECT channel{track.left, center - thickness / 2, track.right, center - thickness / 2 + thickness};
    fillColor(item.hdc, channel, theme::edge);
    channel.right = position;
    fillColor(item.hdc, channel, theme::secondary);
    InflateRect(&thumb, -1, -3);
    fillColor(item.hdc, thumb, GetFocus() == bar ? theme::white : theme::text);
    return CDRF_SKIPDEFAULT;
}

namespace {
HMENU makeMenu(std::vector<MenuEntry>& entries, HFONT font, HBRUSH brush) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return nullptr;
    MENUINFO info{sizeof(info)};
    info.fMask = MIM_BACKGROUND;
    info.hbrBack = brush;
    SetMenuInfo(menu, &info);
    for (UINT index = 0; index < entries.size(); ++index) {
        auto& entry = entries[index];
        entry.font = font;
        MENUITEMINFOW row{sizeof(row)};
        row.fMask = MIIM_FTYPE | MIIM_ID | MIIM_DATA | MIIM_STATE | MIIM_STRING;
        row.fType = MFT_OWNERDRAW;
        row.fState = entry.enabled && !entry.separator ? MFS_ENABLED : MFS_DISABLED;
        row.wID = entry.id;
        row.dwItemData = reinterpret_cast<ULONG_PTR>(&entry);
        row.dwTypeData = entry.label.data();
        if (!entry.children.empty()) {
            row.fMask |= MIIM_SUBMENU;
            row.hSubMenu = makeMenu(entry.children, font, brush);
            if (!row.hSubMenu) { DestroyMenu(menu); return nullptr; }
        }
        if (!InsertMenuItemW(menu, index, TRUE, &row)) {
            if (row.hSubMenu) DestroyMenu(row.hSubMenu);
            DestroyMenu(menu);
            return nullptr;
        }
    }
    return menu;
}
}

unsigned showMenu(HWND owner, HWND anchor, std::vector<MenuEntry> entries, bool alignRight, const POINT* at) {
    auto brush = CreateSolidBrush(theme::field);
    auto font = reinterpret_cast<HFONT>(SendMessageW(anchor, WM_GETFONT, 0, 0));
    if (!font) font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto menu = makeMenu(entries, font, brush);
    if (!menu) { DeleteObject(brush); return 0; }
    RECT rect{};
    GetWindowRect(anchor, &rect);
    if (at) rect = {at->x, at->y, at->x, at->y};
    TPMPARAMS params{sizeof(params), rect};
    unsigned id = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_VERTICAL |
        (alignRight ? TPM_RIGHTALIGN : TPM_LEFTALIGN), alignRight ? rect.right : rect.left, rect.bottom, owner, &params);
    DestroyMenu(menu);
    DeleteObject(brush);
    return id;
}

bool measureMenu(HWND owner, MEASUREITEMSTRUCT& item) {
    if (item.CtlType != ODT_MENU || !item.itemData) return false;
    const auto& entry = *reinterpret_cast<const MenuEntry*>(item.itemData);
    int dpi = GetDpiForWindow(owner);
    const auto dc = GetDC(owner);
    const auto previous = SelectObject(dc, entry.font);
    SIZE size{};
    GetTextExtentPoint32W(dc, entry.label.c_str(), static_cast<int>(entry.label.size()), &size);
    SelectObject(dc, previous);
    ReleaseDC(owner, dc);
    item.itemHeight = MulDiv(entry.separator ? 7 : 25, dpi, 96);
    item.itemWidth = size.cx + MulDiv(32, dpi, 96);
    return true;
}

bool drawMenu(const DRAWITEMSTRUCT& item) {
    if (item.CtlType != ODT_MENU || !item.itemData) return false;
    const auto& entry = *reinterpret_cast<const MenuEntry*>(item.itemData);
    auto rect = item.rcItem;
    fillColor(item.hDC, rect, item.itemState & ODS_SELECTED && entry.enabled ? theme::selected : theme::field);
    if (entry.separator) {
        rect.top += (rect.bottom - rect.top) / 2;
        rect.bottom = rect.top + 1;
        rect.left += 5;
        rect.right -= 5;
        fillColor(item.hDC, rect, theme::edge);
        return true;
    }
    rect.left += 13;
    rect.right -= 10;
    auto previous = SelectObject(item.hDC, entry.font);
    drawTextLine(item.hDC, rect, entry.label, entry.enabled ? theme::text : theme::muted);
    SelectObject(item.hDC, previous);
    return true;
}

namespace {
struct FormBrushes {
    HBRUSH background = CreateSolidBrush(theme::background);
    HBRUSH field = CreateSolidBrush(theme::field);
    ~FormBrushes() { DeleteObject(background); DeleteObject(field); }
};

LRESULT CALLBACK darkCheck(HWND box, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(box, &paint);
        RECT rect{};
        GetClientRect(box, &rect);
        fillColor(dc, rect, theme::background);
        const int size = MulDiv(14, GetDpiForWindow(box), 96);
        const int top = (rect.bottom - size) / 2;
        RECT square{0, top, size, top + size};
        fillColor(dc, square, GetFocus() == box ? theme::secondary : theme::edge);
        InflateRect(&square, -1, -1);
        fillColor(dc, square, theme::field);
        const auto font = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(box, WM_GETFONT, 0, 0)));
        if (SendMessageW(box, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            drawTextLine(dc, square, L"✓", theme::white, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        }
        rect.left += size + 8;
        drawTextLine(dc, rect, windowText(box), theme::text);
        SelectObject(dc, font);
        EndPaint(box, &paint);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    const auto result = DefSubclassProc(box, message, w, l);
    if (message == BM_SETCHECK || message == WM_SETFOCUS || message == WM_KILLFOCUS ||
        message == WM_LBUTTONUP || message == WM_KEYUP) InvalidateRect(box, nullptr, FALSE);
    return result;
}

struct LogPages {
    static constexpr std::streamoff pageSize = 256 * 1024;
    std::ifstream file;
    std::streamoff length, current = 0;
    bool failed = false;

    explicit LogPages(const std::filesystem::path& path) : file(path, std::ios::binary | std::ios::ate), length(file.tellg()) {}
    std::streamoff last() const { return length > 0 ? (length - 1) / pageSize : 0; }
    std::streamoff boundary(std::streamoff page) {
        if (!page) return 0;
        if (page > last()) return length;
        const auto edge = page * pageSize;
        // Prefer complete chat lines, even for long messages. Never split a UTF-8 character.
        constexpr int lookBack = 16384;
        file.seekg(edge - lookBack);
        std::string tail(lookBack, '\0');
        file.read(tail.data(), lookBack);
        if (auto nl = tail.rfind('\n'); nl != std::string::npos) return edge - lookBack + static_cast<std::streamoff>(nl) + 1;
        auto start = edge;
        while (start < length && (file.peek() & 0xc0) == 0x80) { file.get(); ++start; }
        return start;
    }
    void display(HWND window) {
        file.clear();
        const auto start = boundary(current), end = boundary(current + 1);
        std::string bytes(static_cast<std::size_t>(end - start), '\0');
        file.seekg(start);
        file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!file) { failed = true; EndDialog(window, IDCANCEL); return; }
        if (!current && bytes.starts_with("\xef\xbb\xbf")) bytes.erase(0, 3);
        auto edit = GetDlgItem(window, 501);
        putText(edit, wide(bytes));
        SendMessageW(edit, EM_SETSEL, 0, 0);
        EnableWindow(GetDlgItem(window, 502), current > 0);
        EnableWindow(GetDlgItem(window, 503), current < last());
    }
};

std::wstring keyLabel(int key) {
    switch (key) {
    case VK_LBUTTON: return L"Left mouse button";
    case VK_RBUTTON: return L"Right mouse button";
    case VK_MBUTTON: return L"Middle mouse button";
    case VK_XBUTTON1: return L"Mouse 4";
    case VK_XBUTTON2: return L"Mouse 5";
    }
    UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX);
    LONG bits = (scan & 0xff) << 16;
    if (scan & 0xff00) bits |= 1L << 24;
    wchar_t label[80]{};
    if (GetKeyNameTextW(bits, label, ARRAYSIZE(label))) return label;
    return L"Key " + std::to_wstring(key);
}

void armKey(Field& field) {
    SetFocus(field.input);
    field.capturing = true;
    putText(field.input, L"Press a key or mouse button…");
}

LRESULT CALLBACK keyCapture(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& field = *reinterpret_cast<Field*>(data);
    if (msg == WM_KILLFOCUS) {
        field.capturing = false;
        field.capturedKey = 0;
        putText(window, keyLabel(field.selected));
    }
    if (msg == WM_GETDLGCODE && (field.capturing || field.capturedKey || wp == VK_RETURN))
        return DLGC_WANTALLKEYS;
    if (msg == WM_CHAR || msg == WM_SYSCHAR) return 0;
    if (msg == WM_KEYUP || msg == WM_SYSKEYUP) {
        if (field.capturedKey == static_cast<int>(wp)) { field.capturedKey = 0; return 0; }
    }
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        if (field.capturedKey == static_cast<int>(wp)) return 0;
        if (!field.capturing && wp == VK_RETURN) {
            if (!(lp & (1L << 30))) { armKey(field); field.capturedKey = VK_RETURN; }
            return 0;
        }
        if (field.capturing && wp > 0 && wp < 255) {
            int key = static_cast<int>(wp);
            if (key == VK_SHIFT) key = MapVirtualKeyW((lp >> 16) & 0xff, MAPVK_VSC_TO_VK_EX);
            if (key == VK_CONTROL) key = (lp & (1L << 24)) ? VK_RCONTROL : VK_LCONTROL;
            if (key == VK_MENU) key = (lp & (1L << 24)) ? VK_RMENU : VK_LMENU;
            field.selected = key;
            field.capturedKey = static_cast<int>(wp);
            field.capturing = false;
            putText(window, keyLabel(key));
            return 0;
        }
    }
    if (field.capturing && (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN)) {
        field.selected = msg == WM_LBUTTONDOWN ? VK_LBUTTON : msg == WM_RBUTTONDOWN ? VK_RBUTTON :
            msg == WM_MBUTTONDOWN ? VK_MBUTTON : HIWORD(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
        field.capturing = false;
        putText(window, keyLabel(field.selected));
        return TRUE;
    }
    return DefSubclassProc(window, msg, wp, lp);
}

struct Form {
    const wchar_t* title;
    std::vector<Field>* fields;
    LogPages* log;
    FormBrushes brushes;

    static INT_PTR CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_INITDIALOG) {
            auto* self = reinterpret_cast<Form*>(lParam);
            SetWindowLongPtrW(window, DWLP_USER, lParam);
            return self->init(window);
        }
        auto* self = reinterpret_cast<Form*>(GetWindowLongPtrW(window, DWLP_USER));
        if (self && (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC ||
            message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX || message == WM_CTLCOLORBTN)) {
            const bool field = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
            auto dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, theme::text);
            SetBkColor(dc, field ? theme::field : theme::background);
            return reinterpret_cast<INT_PTR>(field ? self->brushes.field : self->brushes.background);
        }
        if (message == WM_MEASUREITEM) {
            auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (item.CtlType == ODT_COMBOBOX) {
                item.itemHeight = MulDiv(23, GetDpiForWindow(window), 96);
                return TRUE;
            }
        }
        if (self && self->fields && (message == WM_HSCROLL || message == WM_NOTIFY)) {
            const auto note = message == WM_NOTIFY ? reinterpret_cast<NMHDR*>(lParam) : nullptr;
            const auto control = note ? note->hwndFrom : reinterpret_cast<HWND>(lParam);
            const auto found = std::find_if(self->fields->begin(), self->fields->end(),
                [control](const Field& f) { return f.type == Field::Slider && f.input == control; });
            if (found != self->fields->end()) {
                if (note && note->code == NM_CUSTOMDRAW) {
                    SetWindowLongPtrW(window, DWLP_MSGRESULT, drawTrackbar(*reinterpret_cast<NMCUSTOMDRAW*>(lParam), theme::background));
                    return TRUE;
                }
                if (message == WM_HSCROLL) {
                    const int position = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
                    if (position != found->sliderScale.position(found->selected)) {
                        found->selected = found->sliderScale.value(position);
                        putText(found->valueLabel, found->sliderText());
                        if (found->onChange) found->onChange(found->selected);
                    }
                    return TRUE;
                }
            }
        }
        if (self && message == WM_DRAWITEM) {
            auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (item.CtlType == ODT_BUTTON) {
                drawButton(item, false);
                return TRUE;
            }
            if (item.CtlType == ODT_COMBOBOX && self->fields) {
                const auto found = std::find_if(self->fields->begin(), self->fields->end(),
                    [&](const auto& f) { return f.input == item.hwndItem; });
                auto rect = item.rcItem;
                fillColor(item.hDC, rect, item.itemState & ODS_SELECTED ? theme::selected : theme::field);
                if (found == self->fields->end() || item.itemID >= found->choices.size()) return TRUE;
                rect.left += 7;
                if (found->swatches) {
                    RECT swatch{rect.left, (rect.top + rect.bottom) / 2 - 5,
                        rect.left + 10, (rect.top + rect.bottom) / 2 + 5};
                    fillColor(item.hDC, swatch, theme::secondary);
                    InflateRect(&swatch, -1, -1);
                    fillColor(item.hDC, swatch, found->swatches[item.itemID]);
                    rect.left += 18;
                }
                drawTextLine(item.hDC, rect, found->choices[item.itemID], theme::text);
                return TRUE;
            }
        }
        if (self && self->log && message == WM_COMMAND && (LOWORD(wParam) == 502 || LOWORD(wParam) == 503)) {
            auto& pages = *self->log;
            pages.current = std::clamp<std::streamoff>(pages.current + (LOWORD(wParam) == 502 ? -1 : 1), 0, pages.last());
            pages.display(window);
            return TRUE;
        }
        if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wParam) == IDCANCEL)) {
            EndDialog(window, IDCANCEL);
            return TRUE;
        }
        if (self && self->fields && message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && lParam) {
            for (auto& field : *self->fields) {
                if (field.input != reinterpret_cast<HWND>(lParam)) continue;
                if (field.type == Field::Key) { armKey(field); return TRUE; }
                if (field.type == Field::Action && field.onClick) {
                    field.onClick(window);
                    return TRUE;
                }
                if (field.type == Field::Check && field.onChange) {
                    field.selected = SendMessageW(field.input, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    field.onChange(field.selected);
                    return TRUE;
                }
            }
        }
        if (message == WM_COMMAND && LOWORD(wParam) == IDOK && self) {
            if (self->fields) {
                for (auto& f : *self->fields) {
                    if (!f.visible) continue;
                    if (f.type == Field::Text) f.text = windowText(f.input);
                    if (f.type == Field::Choice) f.selected = static_cast<int>(SendMessageW(f.input, CB_GETCURSEL, 0, 0));
                    if (f.type == Field::Check) f.selected = SendMessageW(f.input, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    if (f.type == Field::Slider) {
                        const int position = static_cast<int>(SendMessageW(f.input, TBM_GETPOS, 0, 0));
                        if (position != f.sliderScale.position(f.selected)) f.selected = f.sliderScale.value(position);
                    }
                }
            }
            EndDialog(window, IDOK);
            return TRUE;
        }
        return FALSE;
    }

    INT_PTR init(HWND window) const {
        SetWindowTextW(window, title);
        darkFrame(window);
        RECT bounds{};
        GetClientRect(window, &bounds);
        RECT unit{0, 0, 4, 8};
        MapDialogRect(window, &unit);
        const int x = unit.right * 3, y = unit.bottom, button = unit.right * 21;
        const int height = y * 3 / 2 + 2;
        int row = y;
        if (log) {
            auto text = control(window, L"EDIT", L"", WS_TABSTOP | ES_READONLY | ES_MULTILINE |
                ES_AUTOVSCROLL | WS_VSCROLL, 501, WS_EX_CLIENTEDGE);
            MoveWindow(text, x, y, bounds.right - x * 2, bounds.bottom - y * 5, TRUE);
            if (log->last()) {
                auto previous = control(window, L"BUTTON", L"Previous page", WS_TABSTOP | BS_OWNERDRAW, 502);
                auto next = control(window, L"BUTTON", L"Next page", WS_TABSTOP | BS_OWNERDRAW, 503);
                MoveWindow(previous, x, bounds.bottom - y * 3, button, height, TRUE);
                MoveWindow(next, x * 2 + button, bounds.bottom - y * 3, button, height, TRUE);
            }
            log->display(window);
        } else {
            for (auto& f : *fields) {
                if (!f.visible) continue;
                if (f.type != Field::Check && f.type != Field::Action) {
                    auto label = control(window, L"STATIC", f.label.c_str(), SS_LEFT, 0);
                    MoveWindow(label, x, row, bounds.right - x * 2, y + 3, TRUE);
                    row += y + 3;
                }
                DWORD style = WS_TABSTOP;
                if (f.type == Field::Text || f.type == Field::ReadOnly) {
                    if (f.type == Field::ReadOnly) style |= ES_READONLY;
                    f.input = control(window, L"EDIT", f.text.c_str(), style | ES_AUTOHSCROLL, 100 + row, WS_EX_CLIENTEDGE);
                    SendMessageW(f.input, EM_LIMITTEXT, 512, 0);
                } else if (f.type == Field::Key) {
                    f.input = control(window, L"BUTTON", keyLabel(f.selected).c_str(), style | BS_OWNERDRAW, 100 + row);
                    SetWindowSubclass(f.input, keyCapture, 1, reinterpret_cast<DWORD_PTR>(&f));
                } else if (f.type == Field::Choice) {
                    f.input = control(window, L"COMBOBOX", L"", style | CBS_DROPDOWNLIST |
                        CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, 100 + row);
                    SetWindowTheme(f.input, L"", L"");
                    for (const auto& choice : f.choices) SendMessageW(f.input, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.c_str()));
                    SendMessageW(f.input, CB_SETCURSEL, f.selected, 0);
                } else if (f.type == Field::Check) {
                    f.input = control(window, L"BUTTON", f.label.c_str(), style | BS_AUTOCHECKBOX, 100 + row);
                    SetWindowSubclass(f.input, darkCheck, 1, 0);
                    SendMessageW(f.input, BM_SETCHECK, f.selected ? BST_CHECKED : BST_UNCHECKED, 0);
                } else if (f.type == Field::Action) {
                    f.input = control(window, L"BUTTON", f.label.c_str(), style | BS_OWNERDRAW, 100 + row);
                } else {
                    f.input = control(window, TRACKBAR_CLASSW, f.label.c_str(), style | TBS_NOTICKS, 100 + row);
                    configureSlider(f.input, f.sliderScale, f.selected, f.sliderPageStep);
                    const auto value = f.sliderText();
                    f.valueLabel = control(window, L"STATIC", value.c_str(), SS_RIGHT | SS_CENTERIMAGE, 0);
                    int s = unit.right * 13;
                    MoveWindow(f.valueLabel, bounds.right - x - s, row, s, height, TRUE);
                    MoveWindow(f.input, x, row, bounds.right - x * 2 - s - 8, height, TRUE);
                    row += height + y / 3;
                    continue;
                }
                MoveWindow(f.input, x, row, bounds.right - x * 2, f.type == Field::Choice ? height * 8 : height, TRUE);
                row += height + y / 3;
            }
        }
        auto okay = control(window, L"BUTTON", log ? L"Close" : L"OK", WS_TABSTOP | BS_OWNERDRAW, IDOK);
        MoveWindow(okay, bounds.right - x - button, bounds.bottom - y * 3, button, height, TRUE);
        SendMessageW(window, DM_SETDEFID, IDOK, 0);
        if (!log) {
            auto cancel = control(window, L"BUTTON", L"Cancel", WS_TABSTOP | BS_OWNERDRAW, IDCANCEL);
            MoveWindow(cancel, bounds.right - x * 2 - button * 2, bounds.bottom - y * 3, button, height, TRUE);
            const auto first = std::find_if(fields->begin(), fields->end(), [](const Field& f) { return f.visible; });
            if (first != fields->end()) {
                SetFocus(first->input);
                if (first->type == Field::Text) SendMessageW(first->input, EM_SETSEL, 0, -1);
                return FALSE;
            }
        }
        return TRUE;
    }
};
}

std::filesystem::path chooseTextExport(HWND parent, const wchar_t* title, const wchar_t* filename) {
    wchar_t path[32768]{};
    lstrcpynW(path, filename, ARRAYSIZE(path));
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = parent;
    dialog.lpstrFilter = L"Text file (*.txt)\0*.txt\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = ARRAYSIZE(path);
    dialog.lpstrDefExt = L"txt";
    dialog.lpstrTitle = title;
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&dialog) ? std::filesystem::path(path) : std::filesystem::path{};
}

bool form(HWND owner, const wchar_t* title, std::vector<Field>& fields) {
    int height = 36;
    for (const auto& f : fields) if (f.visible) height += f.type == Field::Check || f.type == Field::Action ? 17 : 27;
    EmptyDialog layout(278, static_cast<short>(height));
    Form data{title, &fields, nullptr};
    return DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &layout.dialog, owner, Form::procedure,
        reinterpret_cast<LPARAM>(&data)) == IDOK;
}

bool readChatLog(HWND owner, const wchar_t* title, const std::filesystem::path& file, bool startAtEnd) {
    LogPages pages(file);
    if (pages.length < 0) return false;
    if (startAtEnd) pages.current = pages.last();
    EmptyDialog layout(350, 230);
    Form data{title, nullptr, &pages};
    const auto result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &layout.dialog, owner, Form::procedure,
        reinterpret_cast<LPARAM>(&data));
    return result != -1 && !pages.failed;
}
