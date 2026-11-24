#pragma once

#include "slider_scale.h"

#include <windows.h>
#include <commctrl.h>
#include <functional>
#include <filesystem>
#include <string>
#include <vector>

std::wstring windowText(HWND window);
void putText(HWND window, const std::wstring& value);
HWND control(HWND parent, const wchar_t* type, const wchar_t* label, DWORD style, int id, DWORD extended = 0);
void darkFrame(HWND window);
void fillColor(HDC dc, const RECT& rectangle, COLORREF color);
void drawTextLine(HDC dc, RECT rectangle, const std::wstring& label, COLORREF color,
    UINT flags = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
void drawButton(const DRAWITEMSTRUCT& item, bool dropdown, const std::wstring& title = {}, const std::wstring& tag = {});
void drawMicrophoneButton(const DRAWITEMSTRUCT& item, bool muted);
LRESULT drawTrackbar(const NMCUSTOMDRAW& item, COLORREF surface);
std::wstring percentText(int value);
void configureSlider(HWND slider, SliderScale scale, int value, int pageStep = 1);

struct MenuEntry {
    unsigned id;
    std::wstring label;
    bool enabled = true;
    bool separator = false;
    HFONT font = nullptr;
};

unsigned showMenu(HWND owner, HWND anchor, std::vector<MenuEntry> entries, bool alignRight, const POINT* at = nullptr);
bool measureMenu(HWND owner, MEASUREITEMSTRUCT& item);
bool drawMenu(const DRAWITEMSTRUCT& item);

struct Field {
    enum Type { Text, Choice, Check, Slider, Action };
    Type type;
    std::wstring label;
    std::wstring text;
    std::vector<std::wstring> choices;
    int selected = 0;
    HWND input = nullptr;
    const COLORREF* swatches = nullptr;
    HWND valueLabel = nullptr;
    std::function<void(int)> onChange;
    SliderScale sliderScale{100};
    int sliderPageStep = 10;
    std::wstring sliderSuffix = L"%";
    bool visible = true;
    std::function<void(HWND)> onClick;
    std::wstring sliderText() const {
        return (sliderScale.minimum < 0 && selected > 0 ? L"+" : L"") + std::to_wstring(selected) + sliderSuffix;
    }
};

bool form(HWND owner, const wchar_t* title, std::vector<Field>& fields);
bool readChatLog(HWND owner, const wchar_t* title, const std::filesystem::path& file, bool startAtEnd = false);
