// Интерфейс отрисовки. Только этот слой и окно знают о Win32.
#pragma once

#include "model.hpp"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace ui {

inline constexpr const wchar_t* zone_names[] = {L"Нижняя шейка", L"Бочка", L"Верхняя шейка", L"Прибыль"};
inline constexpr COLORREF background = RGB(244, 247, 251);
inline constexpr COLORREF ink = RGB(23, 43, 61);

std::wstring decimal(double value, int digits = 2);
std::wstring duration(double seconds);

// Параметры текущего отображения. Нулевая baseline означает, что пользователь
// ещё не закрепил исходный расчёт. Данные результатов доступны только для чтения.
struct View {
    const casting::Result& result;
    const casting::Result* baseline;
    std::size_t zone;
    std::size_t frame;
    double probe_mm;
};

// Рисует разрез, показатели и температурный график в области окна.
void draw_result(HDC dc, RECT area, const View& view, HFONT regular, HFONT bold);

}
