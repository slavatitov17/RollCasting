// Публичный интерфейс расчётного ядра. Здесь объявлены входы и результаты,
// которыми пользуются окно, чтение проекта и контрольные задачи.
#pragma once

#include <array>
#include <optional>
#include <vector>

namespace casting {

inline constexpr int model_version = 1;
inline constexpr std::size_t zone_count = 4;

// Размеры одного цилиндрического участка в миллиметрах.
// Четыре участка независимы: обмен теплом между ними опущен.
struct Zone {
    double diameter_mm;
    double height_mm;
    double sand_mm;
    double flask_mm;
};

// Постоянные свойства: плотность [кг/м³], теплоёмкость [Дж/(кг·К)],
// теплопроводность [Вт/(м·К)]. Для металла плотность относится к расплаву.
struct Material {
    double density;
    double heat_capacity;
    double conductivity;
};

// Исходные данные одного запуска. Значения по умолчанию демонстрационные,
// их нельзя считать паспортом конкретной марки стали или плавки.
struct Scenario {
    // Order is from the bottom of the casting to the top.
    std::array<Zone, zone_count> zones{{
        {620, 850, 110, 25}, {1450, 2000, 90, 30},
        {680, 900, 110, 25}, {780, 500, 150, 25}
    }};
    Material metal{7000, 750, 30};
    Material sand{1600, 1050, 0.9};
    Material flask{7850, 500, 45};
    double solid_density = 7450; // Zero disables the volume-change estimate.
    // Солидус и ликвидус в °C, скрытая теплота в Дж/кг.
    double solidus = 1370;
    double liquidus = 1460;
    double latent_heat = 260000;
    // Температуры в момент окончания заливки. На старте металл жидкий.
    double metal_temperature = 1530;
    double mold_temperature = 25;
    double air_temperature = 25;
    // Три коэффициента теплообмена в Вт/(м²·К): два контакта и наружная конвекция.
    double metal_sand_contact = 350;
    double sand_flask_contact = 300;
    double convection = 18;
    double hours = 60;
    double max_step_s = 30;
    int metal_cells = 72;
};

// Один сохранённый момент времени. Сумма трёх геометрических зон равна
// исходному объёму участка. Фазовые объёмы используют массы и плотности.
struct Frame {
    std::vector<double> temperature; // Metal only, from the axis to the surface.
    double solid_zone = 0;
    double mushy_zone = 0;
    double liquid_zone = 0;
    // Толщина непрерывной твёрдой корки от стенки в миллиметрах.
    double shell_mm = 0;
    // Оценочные объёмы фаз и их дефицит относительно начального объёма в м³.
    // При отключённой оценке интерфейс и экспорт скрывают эти значения.
    double solid_volume = 0;
    double liquid_volume = 0;
    double volume_deficit = 0;
};

// История участка. Время полного затвердевания отсутствует, если за
// расчётный горизонт хотя бы одна ячейка осталась выше солидуса.
struct ZoneResult {
    std::vector<Frame> frames;
    std::optional<double> solidification_s;
    double energy_error = 0;
};

// Снимок входов и общий вектор времени в секундах. Индекс кадра одинаков
// для всех участков, поэтому один ползунок управляет всей отливкой.
struct Result {
    Scenario scenario;
    std::vector<double> times;
    std::array<ZoneResult, zone_count> zones;
};

// Проверка диапазонов, порядка температур и предельного объёма вычислений.
void validate(const Scenario& scenario);
// Массовая доля твёрдой фазы от 0 до 1.
double solid_fraction(const Scenario& scenario, double temperature);
// Линейная интерполяция по центрам ячеек. Глубина отсчитывается от стенки.
double probe_temperature(const Frame& frame, const Zone& zone, double depth_mm);
// Расчёт всех участков с независимыми радиальными сетками.
Result simulate(const Scenario& scenario);

}
