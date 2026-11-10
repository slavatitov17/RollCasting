// Вход программы Windows. Преобразует ввод в проект, запускает ядро,
// обрабатывает команды и передаёт результат слою отрисовки.
#include "project.hpp"
#include "view.hpp"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace {

using casting::Project;
using casting::Result;
using ui::decimal;

// Windows сообщает о событиях через ID элемента и код уведомления.
constexpr int run_id = 100, open_id = 101, save_id = 102, csv_id = 103;
constexpr int pin_id = 104, demo_id = 105, tab_id = 106, zone_id = 107, probe_id = 108;
constexpr int timeline_id = 109, input_id = 200;
constexpr int left_width = 360, result_left = 392;
constexpr int summary_top = 119, summary_height = 125, panels_top = 282;

// Файлы хранят текст UTF-8, а функции Win32 с суффиксом W принимают UTF-16.
std::wstring wide(const std::string& input) {
    if (input.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                          static_cast<int>(input.size()), nullptr, 0);
    if (!count) throw std::runtime_error("Текст проекта должен быть в UTF-8.");
    std::wstring output(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                        static_cast<int>(input.size()), output.data(), count);
    return output;
}

std::string utf8(const std::wstring& input) {
    if (input.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                                          static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Некорректный символ в тексте проекта.");
    std::string output(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
                        output.data(), count, nullptr, nullptr);
    return output;
}

std::wstring value(HWND control) {
    std::wstring str(GetWindowTextLengthW(control) + 1, L'\0');
    const int length = GetWindowTextW(control, str.data(), static_cast<int>(str.size()));
    str.resize(length);
    return str;
}

// Ввод принимает точку и запятую. Хвост после числа и бесконечные
// значения считаются ошибкой, чтобы параметры не менялись молча.
std::optional<double> parse_number(std::wstring str) {
    std::replace(str.begin(), str.end(), L',', L'.');
    std::wistringstream input(str);
    input.imbue(std::locale::classic());
    double number;
    if (!(input >> number) || !std::isfinite(number)) return std::nullopt;
    input >> std::ws;
    return input.eof() ? std::optional<double>(number) : std::nullopt;
}

std::wstring edit_number(double number) {
    std::wostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(17) << number;
    auto text = output.str();
    // Use the shortest displayed precision that round-trips to the same value.
    for (int precision = 1; precision < 17; ++precision) {
        std::wostringstream shorter;
        shorter.imbue(std::locale::classic());
        shorter << std::setprecision(precision) << number;
        auto candidate = shorter.str();
        if (candidate.find(L'e') == std::wstring::npos && parse_number(candidate) == number) {
            text = std::move(candidate);
            break;
        }
    }
    std::replace(text.begin(), text.end(), L'.', L',');
    return text;
}

// Стандартный диалог Windows. Отмена возвращает пустой путь.
std::wstring file_dialog(HWND owner, bool save, bool csv = false) {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = csv ? L"Таблица результатов (*.csv)\0*.csv\0"
                             : L"Проект валка (*.roll)\0*.roll\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrDefExt = csv ? L"csv" : L"roll";
    dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST
                   | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return (save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog)) ? path : L"";
}

// Сначала полностью записываем временный файл в той же папке.
// Заменяем целевой файл только после успешной записи и закрытия потока.
void atomic_write(const std::wstring& path, const auto& writer) {
    const std::wstring temp = path + L".tmp-" + std::to_wstring(GetCurrentProcessId());
    try {
        std::ofstream file{std::filesystem::path(temp), std::ios::binary};
        if (!file) throw std::runtime_error("Не удалось открыть файл для записи.");
        writer(file);
        file.close();
        if (!file || !MoveFileExW(temp.c_str(), path.c_str(),
                                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Не удалось сохранить файл.");
        }
    } catch (...) {
        DeleteFileW(temp.c_str());
        throw;
    }
}

// В окне различаются вводимые значения, последний успешный результат
// и снимок базового расчёта для сравнения.
class Window {
public:
    HWND handle = nullptr;

    // Создаём органы управления, заполняем поля и запускаем демонстрационный расчёт.
    void create() {
        regular = make_font(15, FW_NORMAL);
        bold = make_font(18, FW_SEMIBOLD);
        brush = CreateSolidBrush(ui::background);
        button(L"Рассчитать", run_id, 22, 111);
        button(L"Открыть…", open_id, 141, 88);
        button(L"Сохранить…", save_id, 237, 96);
        button(L"CSV…", csv_id, 341, 70);
        button(L"Закрепить базу", pin_id, 419, 123);
        button(L"Демо", demo_id, 550, 67);
        tabs = add(WC_TABCONTROLW, L"", WS_TABSTOP | TCS_BUTTONS, 22, 58, left_width, 29, tab_id);
        for (auto title : {L"Геометрия", L"Материалы", L"Условия"}) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t*>(title);
            TabCtrl_InsertItem(tabs, TabCtrl_GetItemCount(tabs), &item);
        }
        create_fields();
        label(L"Участок", result_left, 61, 63);
        zone_box = add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                       result_left + 65, 56, 150, 200, zone_id);
        for (auto name : ui::zone_names) {
            SendMessageW(zone_box, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
        }
        SendMessageW(zone_box, CB_SETCURSEL, 1, 0);
        label(L"Датчик от стенки, мм", result_left + 232, 61, 153);
        probe = edit(0, result_left + 388, 56, 66, probe_id);
        SetWindowTextW(probe, L"90");
        summary_title = label(L"Состояние всех отделений", result_left, 93, 470);
        SendMessageW(summary_title, WM_SETFONT, reinterpret_cast<WPARAM>(bold), TRUE);
        summary = add(WC_LISTVIEWW, L"", LVS_REPORT | LVS_NOSORTHEADER | WS_BORDER,
                      result_left, summary_top, 680, summary_height);
        ListView_SetExtendedListViewStyle(summary, LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        const wchar_t* headings[] = {L"Отделение", L"Корка, мм", L"Тв. зона", L"Двухфазная",
                                     L"Жидк. зона", L"Тв. фаза", L"Жидк. фаза"};
        for (int i = 0; i < static_cast<int>(std::size(headings)); ++i) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_FMT;
            column.pszText = const_cast<wchar_t*>(headings[i]);
            column.fmt = i == 0 ? LVCFMT_LEFT : LVCFMT_RIGHT;
            ListView_InsertColumn(summary, i, &column);
        }
        for (int i = 0; i < static_cast<int>(casting::zone_count); ++i) {
            LVITEMW row{};
            row.mask = LVIF_TEXT;
            row.iItem = i;
            row.pszText = const_cast<wchar_t*>(ui::zone_names[i]);
            ListView_InsertItem(summary, &row);
        }
        label(L"Объёмы в м³. Двухфазная зона содержит твёрдый металл и расплав.",
              result_left, 252, 660);
        time_label = label(L"После заливки", result_left, 0, 151);
        timeline = add(TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP, 0, 0, 120, 26, timeline_id);
        position_label = label(L"", 0, 0, 130);
        footer = label(L"", 22, 0, 900);
        fill_fields(Project{});
        select_tab();
        compute();
    }

    void layout(int width, int height) const {
        const int table_width = width - result_left - 20;
        MoveWindow(summary, result_left, summary_top, table_width, summary_height, TRUE);
        RECT client{};
        GetClientRect(summary, &client);
        const int columns_width = std::max<int>(0, client.right - 4);
        constexpr int weights[] = {18, 13, 12, 15, 14, 13, 15};
        int offset = 0;
        for (int i = 0; i < static_cast<int>(std::size(weights)); ++i) {
            const int column_width = columns_width * weights[i] / 100;
            ListView_SetColumnWidth(summary, i,
                i + 1 == static_cast<int>(std::size(weights)) ? columns_width - offset : column_width);
            offset += column_width;
        }
        MoveWindow(time_label, result_left, height - 60, 149, 25, TRUE);
        MoveWindow(timeline, result_left + 151, height - 64, width - result_left - 302, 29, TRUE);
        MoveWindow(position_label, width - 147, height - 60, 129, 25, TRUE);
        MoveWindow(footer, 22, height - 27, width - 44, 22, TRUE);
        InvalidateRect(handle, nullptr, FALSE);
    }

    // Вкладки управляют видимостью групп полей, введённые значения сохраняются.
    void select_tab() const {
        const auto selected = TabCtrl_GetCurSel(tabs);
        for (std::size_t i = 0; i < pages.size(); ++i) {
            for (HWND control : pages[i]) {
                ShowWindow(control, static_cast<int>(i) == selected ? SW_SHOW : SW_HIDE);
            }
        }
    }

    // Ввод помечает результат как устаревший. Кнопки запускают вычисление,
    // файловые операции или сравнение. Ошибки выводятся в диалоге.
    void command(int id, int notification) {
        if (id == input_id && notification == EN_CHANGE && !loading) {
            dirty = true;
            pending = true;
            show_status();
            return;
        }
        if ((id == zone_id && notification == CBN_SELCHANGE)
            || (id == probe_id && notification == EN_CHANGE)) {
            refresh();
            return;
        }
        if (notification != BN_CLICKED) return;
        try {
            switch (id) {
            case run_id:
                compute();
                break;
            case open_id:
                open();
                break;
            case save_id:
                save();
                break;
            case csv_id:
                export_csv();
                break;
            case pin_id:
                if (baseline) {
                    baseline.reset();
                } else if (result) {
                    baseline = std::make_unique<Result>(*result);
                }
                SetWindowTextW(GetDlgItem(handle, pin_id), baseline ? L"Убрать базу" : L"Закрепить базу");
                refresh();
                break;
            case demo_id:
                if (confirm_discard()) {
                    fill_fields(Project{});
                    compute();
                }
                break;
            }
        } catch (const std::exception& e) {
            report(e);
        }
    }

    bool confirm_discard() const {
        return !dirty || MessageBoxW(handle, L"В проекте есть несохранённые изменения. Продолжить?",
                                     L"Проект валка", MB_OKCANCEL | MB_ICONQUESTION) == IDOK;
    }

    // Перемещение ползунка меняет кадр и перерисовывает результат без пересчёта.
    void refresh() const {
        if (!result || !timeline) return;
        const auto frame = frame_index();
        const auto time = ui::duration(result->times[frame]);
        SetWindowTextW(position_label, time.c_str());
        SetWindowTextW(summary_title, (L"Состояние всех отделений  ·  " + time).c_str());
        // Все строки используют один кадр, даже когда график показывает другой участок.
        SendMessageW(summary, WM_SETREDRAW, FALSE, 0);
        for (int z = 0; z < static_cast<int>(casting::zone_count); ++z) {
            const auto& state = result->zones[z].frames[frame];
            const bool volumes = result->scenario.solid_density > 0;
            std::wstring cells[] = {decimal(state.shell_mm, 1), decimal(state.solid_zone, 3),
                decimal(state.mushy_zone, 3), decimal(state.liquid_zone, 3),
                volumes ? decimal(state.solid_volume, 3) : L"-",
                volumes ? decimal(state.liquid_volume, 3) : L"-"};
            for (int col = 0; col < static_cast<int>(std::size(cells)); ++col) {
                ListView_SetItemText(summary, z, col + 1, cells[col].data());
            }
        }
        SendMessageW(summary, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(summary, nullptr, TRUE);
        RECT area{};
        GetClientRect(handle, &area);
        area.left = result_left;
        area.top = panels_top;
        area.bottom -= 66;
        InvalidateRect(handle, &area, FALSE);
        show_status();
    }

    // Собираем кадр в памяти и копируем в окно одним действием.
    // Это уменьшает мерцание при изменении времени и размеров окна.
    void paint() const {
        PAINTSTRUCT ps{};
        const HDC screen = BeginPaint(handle, &ps);
        RECT rect{};
        GetClientRect(handle, &rect);
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP buffer = CreateCompatibleBitmap(screen, rect.right, rect.bottom);
        if (!dc || !buffer) {
            if (buffer) DeleteObject(buffer);
            if (dc) DeleteDC(dc);
            EndPaint(handle, &ps);
            return;
        }
        const auto old = SelectObject(dc, buffer);
        const auto old_font = SelectObject(dc, regular);
        FillRect(dc, &rect, brush);
        RECT divider{22, 50, rect.right - 20, 51};
        HBRUSH divider_brush = CreateSolidBrush(RGB(199, 202, 206));
        FillRect(dc, &divider, divider_brush);
        DeleteObject(divider_brush);
        SetBkMode(dc, TRANSPARENT);
        if (result) {
            ui::draw_result(dc, {result_left, panels_top, rect.right - 20, rect.bottom - 70},
                {*result, baseline.get(), selected_zone(), frame_index(), probe_depth()}, regular, bold);
        }
        const auto& p = ps.rcPaint;
        BitBlt(screen, p.left, p.top, p.right - p.left, p.bottom - p.top,
               dc, p.left, p.top, SRCCOPY);
        SelectObject(dc, old_font);
        SelectObject(dc, old);
        DeleteObject(buffer);
        DeleteDC(dc);
        EndPaint(handle, &ps);
    }

    LRESULT background_for(HDC dc) const {
        SetTextColor(dc, ui::ink);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(brush);
    }

    void report(const std::exception& e) const {
        MessageBoxW(handle, wide(e.what()).c_str(), L"Ошибка проекта", MB_OK | MB_ICONERROR);
    }

    ~Window() {
        for (auto font : {regular, bold}) if (font) DeleteObject(font);
        if (brush) DeleteObject(brush);
    }

private:
    HFONT regular = nullptr, bold = nullptr;
    HBRUSH brush = nullptr;
    HWND tabs = nullptr, zone_box = nullptr, timeline = nullptr, probe = nullptr;
    HWND summary = nullptr, summary_title = nullptr;
    HWND title = nullptr, source = nullptr, footer = nullptr, time_label = nullptr, position_label = nullptr;
    std::array<std::vector<HWND>, 3> pages;
    Project draft;
    Project calculated;
    // loading подавляет события при заполнении формы. dirty предупреждает о
    // несохранённых входах, pending запрещает экспорт устаревшего расчёта.
    bool loading = false, dirty = false, pending = false;
    std::unique_ptr<Result> result, baseline;

    struct Field {
        HWND control;
        std::wstring name;
        double* value;
    };
    std::vector<Field> fields;
    HWND cells = nullptr;

    static HFONT make_font(int height, int weight) {
        return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
    }

    HWND add(const wchar_t* type, const wchar_t* text, DWORD style, int x, int y,
             int w, int h, int id = 0, DWORD ex_style = 0) {
        HWND control = CreateWindowExW(ex_style, type, text, WS_CHILD | WS_VISIBLE | style,
            x, y, w, h, handle, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(regular), TRUE);
        return control;
    }

    void track(HWND control, int page) {
        if (page >= 0) pages[page].push_back(control);
    }

    HWND label(const wchar_t* text, int x, int y, int width, int page = -1, int height = 22) {
        HWND control = add(L"STATIC", text, SS_LEFT, x, y, width, height);
        track(control, page);
        return control;
    }

    HWND edit(int page, int x, int y, int w, int id = input_id, int limit = 24) {
        HWND control = add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, x, y, w, 25, id, WS_EX_CLIENTEDGE);
        SendMessageW(control, EM_SETLIMITTEXT, limit, 0);
        // The virtual probe stays visible on all three tabs.
        if (id != probe_id) track(control, page);
        return control;
    }

    void button(const wchar_t* text, int id, int x, int w) {
        add(L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, x, 12, w, 32, id);
    }

    void field(int page, int x, int y, int w, const wchar_t* name, double& number) {
        fields.push_back({edit(page, x, y, w), name, &number});
    }

    void row(int page, int y, const wchar_t* name, double& number) {
        label(name, 24, y + 3, 244, page);
        field(page, 270, y, 100, name, number);
    }

    // Связываем каждое числовое поле с соответствующим параметром draft.
    // Вкладки группируют геометрию, свойства материалов и режим расчёта.
    void create_fields() {
        auto& s = draft.scenario;
        label(L"Название расчёта", 24, 100, left_width, 0);
        title = edit(0, 24, 123, 346, input_id, 80);
        label(L"Источник и статус исходных данных", 24, 161, left_width, 0);
        source = edit(0, 24, 184, 346, input_id, 240);
        label(L"Размеры участков, мм", 24, 223, left_width, 0);
        for (int i = 0; i < 4; ++i) {
            constexpr const wchar_t* headings[] = {L"Диам.", L"Высота", L"Песок", L"Опока"};
            label(headings[i], 127 + i * 61, 250, 60, 0);
        }
        for (std::size_t z = 0; z < casting::zone_count; ++z) {
            const int y = 277 + static_cast<int>(z) * 35;
            constexpr const wchar_t* short_names[] = {L"Нижн. шейка", L"Бочка", L"Верхн. шейка", L"Прибыль"};
            label(short_names[z], 24, y + 3, 102, 0);
            auto& zone = s.zones[z];
            field(0, 127, y, 57, L"Диаметр", zone.diameter_mm);
            field(0, 188, y, 57, L"Высота", zone.height_mm);
            field(0, 249, y, 57, L"Толщина песка", zone.sand_mm);
            field(0, 310, y, 57, L"Толщина опоки", zone.flask_mm);
        }
        label(L"Время 0 - окончание заполнения формы.\r\nОхлаждение только через боковые стенки.",
              24, 429, 345, 0, 55);

        label(L"Свойства материалов (постоянные)", 24, 104, 346, 1);
        label(L"ρ, кг/м³", 132, 134, 72, 1);
        label(L"c, Дж/(кг·К)", 208, 134, 88, 1);
        label(L"k, Вт/(м·К)", 293, 134, 88, 1);
        int y = 158;
        int i = 0;
        constexpr const wchar_t* names[] = {L"Расплав", L"Песок", L"Опока"};
        for (auto* m : {&s.metal, &s.sand, &s.flask}) {
            label(names[i++], 24, y + 3, 105, 1);
            field(1, 132, y, 72, L"Плотность", m->density);
            field(1, 208, y, 78, L"Теплоёмкость", m->heat_capacity);
            field(1, 293, y, 77, L"Теплопроводность", m->conductivity);
            y += 34;
        }
        row(1, 277, L"Плотность твёрдой фазы, кг/м³", s.solid_density);
        row(1, 311, L"Солидус, °C", s.solidus);
        row(1, 345, L"Ликвидус, °C", s.liquidus);
        row(1, 379, L"Теплота кристаллизации, Дж/кг", s.latent_heat);
        label(L"Плотность твёрдой фазы 0 отключает\r\nоценку фазовых объёмов и дефицита.",
              24, 428, 345, 1, 53);

        row(2, 105, L"Начальная температура металла, °C", s.metal_temperature);
        row(2, 140, L"Начальная температура формы, °C", s.mold_temperature);
        row(2, 175, L"Температура воздуха, °C", s.air_temperature);
        label(L"Коэффициенты теплообмена, Вт/(м²·К)", 24, 221, 346, 2);
        row(2, 247, L"Контакт металл - песок", s.metal_sand_contact);
        row(2, 282, L"Контакт песок - опока", s.sand_flask_contact);
        row(2, 317, L"Наружная теплоотдача", s.convection);
        row(2, 368, L"Длительность расчёта, ч", s.hours);
        row(2, 403, L"Максимальный шаг времени, с", s.max_step_s);
        label(L"Число ячеек по радиусу металла", 24, 441, 244, 2);
        cells = edit(2, 270, 438, 100);
    }

    // Подстановка проекта в поля. Заполняем их без пометки о ручном изменении.
    void fill_fields(Project project) {
        const auto name = wide(project.title);
        const auto citation = wide(project.source);
        draft = std::move(project);
        loading = true;
        for (const auto& entry : fields) {
            SetWindowTextW(entry.control, edit_number(*entry.value).c_str());
        }
        SetWindowTextW(title, name.c_str());
        SetWindowTextW(source, citation.c_str());
        SetWindowTextW(cells, std::to_wstring(draft.scenario.metal_cells).c_str());
        loading = false;
        dirty = false;
        pending = true;
        show_status();
    }

    double numeric(HWND control, const std::wstring& name) {
        const auto number = parse_number(value(control));
        if (number) return *number;
        for (std::size_t page = 0; page < pages.size(); ++page) {
            if (std::find(pages[page].begin(), pages[page].end(), control) != pages[page].end()) {
                TabCtrl_SetCurSel(tabs, page);
                select_tab();
                break;
            }
        }
        SetFocus(control);
        throw std::runtime_error(utf8(L"Проверьте числовое поле «" + name + L"»."));
    }

    // Читаем значения всех вкладок, включая скрытые, и проверяем проект.
    Project read_fields() {
        for (const auto& entry : fields) {
            *entry.value = numeric(entry.control, entry.name);
        }
        const double count = numeric(cells, L"Число ячеек");
        if (count < 8 || count > 360 || count != std::floor(count)) {
            throw std::runtime_error("Число ячеек должно быть целым числом от 8 до 360.");
        }
        draft.scenario.metal_cells = static_cast<int>(count);
        draft.title = utf8(value(title));
        draft.source = utf8(value(source));
        casting::validate(draft.scenario);
        return draft;
    }

    // Заменяем показанный результат после успешного прогона.
    // Устанавливаем диапазон ползунка по фактическому числу кадров.
    void publish(const Project& project, std::unique_ptr<Result> next) {
        result = std::move(next);
        calculated = project;
        pending = false;
        SendMessageW(timeline, TBM_SETRANGEMAX, TRUE, result->times.size() - 1);
        const auto page = std::lower_bound(result->times.begin(), result->times.end(), 3600)
                          - result->times.begin();
        SendMessageW(timeline, TBM_SETPAGESIZE, 0, std::max<std::ptrdiff_t>(1, page));
        SendMessageW(timeline, TBM_SETPOS, TRUE, 0);
        refresh();
    }

    // Снимок входов передаётся в ядро. Исключение оставляет предыдущий результат.
    void compute() {
        const auto project = read_fields();
        const HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        try {
            publish(project, std::make_unique<Result>(casting::simulate(project.scenario)));
        } catch (...) {
            SetCursor(previous);
            throw;
        }
        SetCursor(previous);
    }

    // Сначала читаем и рассчитываем новый файл. После успеха меняем поля окна.
    void open() {
        if (!confirm_discard()) return;
        const auto path = file_dialog(handle, false);
        if (path.empty()) return;
        std::ifstream file{std::filesystem::path(path), std::ios::binary};
        if (!file) throw std::runtime_error("Не удалось открыть проект.");
        const auto project = casting::read_project(file);
        auto next = std::make_unique<Result>(casting::simulate(project.scenario));
        fill_fields(project);
        publish(project, std::move(next));
    }

    void save() {
        const auto project = read_fields();
        const auto path = file_dialog(handle, true);
        if (path.empty()) return;
        atomic_write(path, [&](auto& file) { casting::write_project(file, project); });
        dirty = false;
        show_status();
    }

    // Экспортируем согласованный с полями результат и проверяем глубину датчика.
    void export_csv() {
        if (!result) return;
        if (pending) throw std::runtime_error("Параметры изменились. Сначала пересчитайте проект.");
        const auto depth = parse_number(value(probe));
        if (!depth || *depth < 0 || *depth > result->scenario.zones[selected_zone()].diameter_mm / 2) {
            throw std::runtime_error("Перед экспортом введите допустимую глубину датчика.");
        }
        const auto path = file_dialog(handle, true, true);
        if (path.empty()) return;
        atomic_write(path, [&](auto& file) { casting::write_csv(file, *result, *depth); });
    }

    std::size_t selected_zone() const {
        return static_cast<std::size_t>(std::clamp<LRESULT>(SendMessageW(zone_box, CB_GETCURSEL, 0, 0), 0, 3));
    }

    std::size_t frame_index() const {
        return static_cast<std::size_t>(SendMessageW(timeline, TBM_GETPOS, 0, 0));
    }

    double probe_depth() const {
        const double radius = result->scenario.zones[selected_zone()].diameter_mm / 2;
        return std::clamp(parse_number(value(probe)).value_or(0), 0.0, radius);
    }

    void show_status() const {
        if (!footer) return;
        std::wstring message;
        if (pending) {
            message = L"Параметры изменены. Нажмите «Рассчитать», чтобы обновить результаты.";
        } else if (result) {
            const double radius = result->scenario.zones[selected_zone()].diameter_mm / 2;
            const auto depth = parse_number(value(probe));
            if (!depth || *depth < 0 || *depth > radius) {
                message = L"Введите глубину датчика от 0 до " + decimal(radius, 1) + L" мм.";
            } else {
                message = L"Расчёт  ·  " + wide(calculated.title) + L"  ·  " + wide(calculated.source);
            }
        }
        SetWindowTextW(footer, message.c_str());
    }
};

// Оконная процедура направляет сообщения Windows объекту Window.
// Один указатель хранится в пользовательских данных окна.
LRESULT CALLBACK procedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NCCREATE) {
        auto* window = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        window->handle = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    }
    auto* window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!window) return DefWindowProcW(hwnd, msg, w, l);
    switch (msg) {
    case WM_CREATE:
        try {
            window->create();
        } catch (const std::exception& e) {
            window->report(e);
            return -1;
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(l);
        info->ptMinTrackSize = {1120, 680};
        return 0;
    }
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) window->layout(LOWORD(l), HIWORD(l));
        return 0;
    case WM_COMMAND:
        window->command(LOWORD(w), HIWORD(w));
        return 0;
    case WM_NOTIFY:
        if (w == tab_id && reinterpret_cast<NMHDR*>(l)->code == TCN_SELCHANGE) window->select_tab();
        return 0;
    case WM_HSCROLL:
        window->refresh();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        window->paint();
        return 0;
    case WM_CTLCOLORSTATIC:
        return window->background_for(reinterpret_cast<HDC>(w));
    case WM_CLOSE:
        if (window->confirm_discard()) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

}

// Регистрируем класс и запускаем цикл сообщений до закрытия окна.
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    const wchar_t class_name[] = L"RollCasting";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = procedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    window_class.lpszClassName = class_name;
    if (!RegisterClassW(&window_class)) return 1;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = std::min<int>(1210, work.right - work.left);
    const int height = std::min<int>(760, work.bottom - work.top);
    Window window;
    HWND main = CreateWindowExW(WS_EX_CONTROLPARENT, class_name, L"RollCasting",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, work.left + (work.right - work.left - width) / 2,
        work.top + (work.bottom - work.top - height) / 2, width, height,
        nullptr, nullptr, instance, &window);
    if (!main) return 1;
    // Разворачиваем стандартное окно, сохраняя системные кнопки и обычную рамку.
    ShowWindow(main, SW_SHOWMAXIMIZED);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(main, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}
