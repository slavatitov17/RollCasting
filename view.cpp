// Визуализация готовых данных. Рисование не меняет температуры или результат.
#include "view.hpp"

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace ui {
namespace {

constexpr COLORREF white = RGB(255, 255, 255);
constexpr COLORREF muted = RGB(82, 103, 124);
constexpr COLORREF grid = RGB(222, 230, 239);
constexpr COLORREF solid = RGB(46, 111, 172);
constexpr COLORREF mushy = RGB(238, 178, 55);
constexpr COLORREF liquid = RGB(216, 87, 59);
constexpr COLORREF sensor = RGB(0, 126, 111);
constexpr COLORREF baseline_color = RGB(91, 76, 146);

// Кисть GDI нужна только на время заливки прямоугольника.
void fill(HDC dc, int x, int y, int w, int h, COLORREF color) {
    RECT rect{x, y, x + w, y + h};
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void text(HDC dc, int x, int y, const std::wstring& str, COLORREF color = ink) {
    SetTextColor(dc, color);
    TextOutW(dc, x, y, str.c_str(), static_cast<int>(str.size()));
}

void polyline(HDC dc, const std::vector<POINT>& points, COLORREF color, int style = PS_SOLID,
              int width = 1) {
    HPEN pen = CreatePen(style, width, color);
    auto old = SelectObject(dc, pen);
    Polyline(dc, points.data(), static_cast<int>(points.size()));
    SelectObject(dc, old);
    DeleteObject(pen);
}

// Участки размещаем снизу вверх. Радиусы и высоты масштабируются отдельно,
// поэтому разрез показывает относительные размеры, но не служит чертежом.
void draw_casting(HDC dc, RECT r, const View& v, HFONT regular, HFONT bold) {
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    SelectObject(dc, bold);
    text(dc, r.left + 14, r.top + 12, L"Разрез отливки");
    SelectObject(dc, regular);
    text(dc, r.left + 14, r.top + 36, L"Фазы в выбранный момент", muted);
    const auto& s = v.result.scenario;
    double total_height = 0, largest_diameter = 0;
    for (const auto& z : s.zones) {
        total_height += z.height_mm;
        largest_diameter = std::max(largest_diameter, z.diameter_mm);
    }
    const double vertical_scale = (h - 160.0) / total_height;
    const double radial_scale = (w - 84.0) / largest_diameter;
    const int axis = r.left + (w - 44) / 2;
    const int bottom = r.bottom - 70;
    int last_y = bottom;
    double elevation = 0;
    for (std::size_t z = 0; z < casting::zone_count; ++z) {
        const auto& zone = s.zones[z];
        const auto& frame = v.result.zones[z].frames[v.frame];
        elevation += zone.height_mm;
        const int top = bottom - static_cast<int>(std::lround(elevation * vertical_scale));
        const double radius = zone.diameter_mm * radial_scale / 2;
        const int edge = axis - static_cast<int>(std::lround(radius));
        if (z == v.zone) {
            fill(dc, r.left + 10, top, w - 20, std::max(1, last_y - top), RGB(228, 239, 249));
        }
        const auto n = static_cast<int>(frame.temperature.size());
        // Каждое кольцо отображается полосой с двух сторон от оси.
        // Цвет определяется фазовым состоянием ячейки в выбранном кадре.
        for (int cell = 0; cell < n; ++cell) {
            const auto phase = casting::solid_fraction(s, frame.temperature[cell]);
            const auto color = phase == 1 ? solid : phase == 0 ? liquid : mushy;
            const int a = static_cast<int>(std::lround(radius * cell / n));
            const int b = static_cast<int>(std::lround(radius * (cell + 1) / n));
            fill(dc, axis + a, top, b - a, last_y - top, color);
            fill(dc, axis - b, top, b - a, last_y - top, color);
        }
        polyline(dc, {{edge, last_y}, {edge, top}, {2 * axis - edge, top},
                      {2 * axis - edge, last_y}, {edge, last_y}}, ink);
        last_y = top;
    }
    // Schematic runner, separate from the radial thermal domain.
    const int riser = r.right - 26;
    polyline(dc, {{riser - 7, last_y}, {riser, last_y + 12}, {riser + 7, last_y}},
             muted, PS_SOLID, 2);
    polyline(dc, {{riser, last_y + 12}, {riser, bottom + 14}, {axis, bottom + 14}, {axis, bottom}},
             muted, PS_SOLID, 2);
    text(dc, r.left + 14, r.bottom - 47, L"Сифон и питатель — схема", muted);
    const int legend = r.bottom - 23;
    fill(dc, r.left + 14, legend + 3, 10, 10, solid);
    text(dc, r.left + 30, legend, L"Твёрдая");
    fill(dc, r.left + 91, legend + 3, 10, 10, mushy);
    text(dc, r.left + 106, legend, L"Двухфазная");
    fill(dc, r.left + 191, legend + 3, 10, 10, liquid);
    text(dc, r.left + 206, legend, L"Жидкая");
}

// График строится по сохранённым кадрам. Датчики на оси и стенке используют
// крайние ячейки. Пользовательский датчик интерполируется по глубине.
void draw_chart(HDC dc, RECT r, const View& v, HFONT regular, HFONT bold) {
    const auto& current = v.result.zones[v.zone];
    const auto& geometry = v.result.scenario.zones[v.zone];
    const int x = r.left + 14, y = r.top + 9;
    SelectObject(dc, bold);
    text(dc, x, y, std::wstring(L"Кривые охлаждения · ") + zone_names[v.zone]);
    SelectObject(dc, regular);
    const auto& frame = current.frames[v.frame];
    text(dc, x, y + 29, current.solidification_s
        ? L"Конец затвердевания  " + duration(*current.solidification_s)
        : L"Конец затвердевания за пределами расчёта", muted);
    text(dc, x, y + 56, L"У стенки", solid);
    text(dc, x + 85, y + 56, L"На оси", liquid);
    text(dc, x + 155, y + 56, L"Датчик", sensor);
    if (v.baseline) {
        text(dc, x + 225, y + 56, L"Базовый датчик", baseline_color);
    }
    double minimum = v.result.scenario.solidus;
    double maximum = v.result.scenario.liquidus;
    double end = v.result.times.back();
    const auto extend = [&](const casting::Result& result) {
        for (const auto& frame : result.zones[v.zone].frames) {
            for (double t : frame.temperature) {
                minimum = std::min(minimum, t);
                maximum = std::max(maximum, t);
            }
        }
    };
    extend(v.result);
    // Базовую кривую сравниваем на той же глубине, только если точка
    // помещается внутри базового участка.
    const bool compare_probe = v.baseline && v.probe_mm <= v.baseline->scenario.zones[v.zone].diameter_mm / 2;
    if (compare_probe) {
        extend(*v.baseline);
        end = std::max(end, v.baseline->times.back());
    }
    minimum = 50 * std::floor((minimum - 10) / 50);
    maximum = 50 * std::ceil((maximum + 10) / 50);
    const int left = r.left + 66, right = r.right - 38, top = y + 104, bottom = r.bottom - 73;
    // Перевод физических времени и температуры в пиксели. По вертикали
    // температура растёт вверх, а экранная координата — вниз.
    const auto tx = [&](double t) { return left + static_cast<int>(std::lround((right - left) * t / end)); };
    const auto ty = [&](double t) {
        return bottom - static_cast<int>(std::lround((bottom - top) * (t - minimum) / (maximum - minimum)));
    };
    text(dc, left - 29, top - 21, L"°C", muted);
    const int vertical_ticks = std::clamp((bottom - top) / 27, 1, 4);
    for (int i = 0; i <= vertical_ticks; ++i) {
        const double t = minimum + (maximum - minimum) * i / vertical_ticks;
        const int pos = ty(t);
        polyline(dc, {{left, pos}, {right, pos}}, grid);
        text(dc, x, pos - 9, decimal(t, 0), muted);
    }
    for (int i = 0; i <= 4; ++i) {
        const double t = end * i / 4;
        const int pos = tx(t);
        const auto tick = decimal(t / 3600, end < 3600 ? 2 : 1) + L" ч";
        SIZE extent{};
        GetTextExtentPoint32W(dc, tick.c_str(), static_cast<int>(tick.size()), &extent);
        text(dc, pos - extent.cx / 2, bottom + 9, tick, muted);
    }
    for (const double t : {v.result.scenario.solidus, v.result.scenario.liquidus}) {
        polyline(dc, {{left, ty(t)}, {right, ty(t)}}, muted, PS_DOT);
    }
    // Вертикальная линия связывает график с выбранным кадром.
    const int cursor = tx(v.result.times[v.frame]);
    polyline(dc, {{cursor, top}, {cursor, bottom}}, muted, PS_DASH);

    const auto curve = [&](const casting::Result& result, auto read, COLORREF color, int style = PS_SOLID) {
        std::vector<POINT> points;
        const auto& frames = result.zones[v.zone].frames;
        for (std::size_t i = 0; i < result.times.size(); ++i) {
            points.push_back({tx(result.times[i]), ty(read(frames[i]))});
        }
        polyline(dc, points, color, style, style == PS_SOLID ? 2 : 1);
    };
    curve(v.result, [](const auto& f) { return f.temperature.back(); }, solid);
    curve(v.result, [](const auto& f) { return f.temperature.front(); }, liquid);
    curve(v.result, [&](const auto& f) { return casting::probe_temperature(f, geometry, v.probe_mm); }, sensor);
    if (compare_probe) {
        curve(*v.baseline, [&](const auto& f) {
            return casting::probe_temperature(f, v.baseline->scenario.zones[v.zone], v.probe_mm);
        }, baseline_color, PS_DASH);
    }
    const double value = casting::probe_temperature(frame, geometry, v.probe_mm);
    std::wstring detail = L"Датчик  " + decimal(value, 1) + L" °C";
    if (v.result.scenario.solid_density > 0) {
        detail += L"  ·  Дефицит  " + decimal(frame.volume_deficit, 4) + L" м³";
    }
    text(dc, x, bottom + 32, detail, muted);
    std::wstring note = L"Горизонтальный пунктир — солидус и ликвидус";
    if (v.baseline && !compare_probe) {
        note = L"Точка датчика вне радиуса базового валка";
    } else if (compare_probe && current.solidification_s && v.baseline->zones[v.zone].solidification_s) {
        const double difference = *current.solidification_s - *v.baseline->zones[v.zone].solidification_s;
        note = L"Разница с базой  " + decimal(difference / 60, 1) + L" мин";
    }
    text(dc, x, bottom + 48, note, muted);
}

}

// Формат представления числа для русскоязычного окна.
std::wstring decimal(double value, int digits) {
    wchar_t buffer[96]{};
    std::swprintf(buffer, std::size(buffer), L"%.*f", digits, value);
    std::wstring output(buffer);
    std::replace(output.begin(), output.end(), L'.', L',');
    return output;
}

std::wstring duration(double seconds) {
    const auto whole = static_cast<long>(std::lround(seconds));
    return std::to_wstring(whole / 3600) + L" ч " + std::to_wstring((whole / 60) % 60)
        + L" мин " + std::to_wstring(whole % 60) + L" с";
}

// Разрез и график используют тот же кадр, что и таблица всех отделений.
void draw_result(HDC dc, RECT area, const View& view, HFONT regular, HFONT bold) {
    const int casting_width = std::clamp<int>((area.right - area.left) / 3, 258, 380);
    const int chart_left = area.left + casting_width + 20;
    fill(dc, area.left, area.top, casting_width, area.bottom - area.top, white);
    fill(dc, chart_left, area.top, area.right - chart_left, area.bottom - area.top, white);
    draw_casting(dc, {area.left, area.top, chart_left - 20, area.bottom}, view, regular, bold);
    draw_chart(dc, {chart_left, area.top, area.right, area.bottom}, view, regular, bold);
}

}
