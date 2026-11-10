// Тепловой расчёт после заполнения формы. Единицы внутри сетки — СИ.
// Никаких зависимостей от интерфейса Windows.
#include "model.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace casting {
namespace {

constexpr double pi = std::numbers::pi_v<double>;

void check(double value, double minimum, double maximum, const char* name) {
    if (!std::isfinite(value) || value < minimum || value > maximum) {
        throw std::invalid_argument(name);
    }
}

// Удельная энтальпия [Дж/кг]: c·T плюс скрытая энергия жидкой доли.
// Отсчёт температуры в °C даёт общий произвольный ноль энтальпии.
double enthalpy(const Scenario& s, double t) {
    return s.metal.heat_capacity * t + s.latent_heat * (1 - solid_fraction(s, t));
}

// Кольцевая контрольная ячейка высотой со весь участок.
// Вычисленная по начальной плотности масса постоянна во времени.
struct Cell {
    double inner;
    double outer;
    Material material;
    double mass;

    double width() const { return outer - inner; }
    double volume(double height) const {
        return pi * (outer + inner) * width() * height;
    }
};

// conductance[i] [Вт/К] связывает ячейки i−1 и i. Нулевая грань лежит
// на оси симметрии, последняя связывает опоку с воздухом.
struct RadialMesh {
    std::vector<Cell> cells;
    std::vector<double> conductance;

    RadialMesh(const Scenario& s, const Zone& zone) {
        const double height = zone.height_mm / 1000;
        const auto layer = [&](double thickness, int count, Material material) {
            const double start = cells.empty() ? 0 : cells.back().outer;
            for (int i = 0; i < count; ++i) {
                Cell cell{start + thickness * i / count,
                          start + thickness * (i + 1) / count, material, 0};
                cell.mass = cell.volume(height) * material.density;
                cells.push_back(cell);
            }
        };
        // Половина диаметра из миллиметров переводится в метры. Каждая граница
        // материалов совпадает с гранью ячеек, поэтому свойства не смешиваются.
        layer(zone.diameter_mm / 2000, s.metal_cells, s.metal);
        layer(zone.sand_mm / 1000, std::max(4, s.metal_cells / 4), s.sand);
        layer(zone.flask_mm / 1000, std::max(3, s.metal_cells / 12), s.flask);

        conductance.resize(cells.size() + 1, 0);
        for (std::size_t i = 1; i < cells.size(); ++i) {
            const Cell& left = cells[i - 1];
            const Cell& right = cells[i];
            const double area = 2 * pi * right.inner * height;
            // Суммируем два удельных сопротивления от центров до грани [м²·К/Вт].
            // На границе материалов добавляем 1/h, затем делим площадь на сумму.
            double resistance = left.width() / (2 * left.material.conductivity)
                              + right.width() / (2 * right.material.conductivity);
            if (i == static_cast<std::size_t>(s.metal_cells)) {
                resistance += 1 / s.metal_sand_contact;
            }
            if (i == static_cast<std::size_t>(s.metal_cells + std::max(4, s.metal_cells / 4))) {
                resistance += 1 / s.sand_flask_contact;
            }
            conductance[i] = area / resistance;
        }
        // На внешней грани последовательно работают половина ячейки опоки и
        // конвекция. При h = 0 поверхность изолирована, проводимость уже нулевая.
        if (s.convection > 0) {
            const Cell& last = cells.back();
            const double area = 2 * pi * last.outer * height;
            conductance.back() = area / (last.width() / (2 * last.material.conductivity)
                                          + 1 / s.convection);
        }
    }
};

// Интегрирует по времени температуры металла, песка и опоки для одного
// участка. Соседние по высоте участки здесь не участвуют.
class RadialSolver {
public:
    RadialSolver(const Scenario& scenario, const Zone& zone)
        : s(scenario), mesh(s, zone), temperature(mesh.cells.size(), s.mold_temperature),
          estimate(temperature), candidate(temperature), residual(temperature),
          diagonal(temperature), correction(temperature) {
        std::fill_n(temperature.begin(), s.metal_cells, s.metal_temperature);
    }

    // Сохраняем только ячейки металла и сводные величины для экрана и CSV.
    Frame sample(const Zone& zone) const {
        Frame frame;
        frame.temperature.assign(temperature.begin(), temperature.begin() + s.metal_cells);
        for (int i = 0; i < s.metal_cells; ++i) {
            const Cell& cell = mesh.cells[i];
            const double volume = cell.volume(zone.height_mm / 1000);
            const double fraction = solid_fraction(s, temperature[i]);
            if (fraction == 1) {
                frame.solid_zone += volume;
            } else if (fraction == 0) {
                frame.liquid_zone += volume;
            } else {
                frame.mushy_zone += volume;
            }
            // В двухфазной ячейке масса разбивается по f_s. Объёмы фаз равны m/ρ.
            // Дефицит объёма здесь не меняет геометрию сетки или тепловые потоки.
            if (s.solid_density > 0) {
                frame.solid_volume += cell.mass * fraction / s.solid_density;
                frame.liquid_volume += cell.mass * (1 - fraction) / s.metal.density;
                frame.volume_deficit += cell.mass * fraction
                    * (1 / s.metal.density - 1 / s.solid_density);
            }
        }
        // Корка растёт от наружной стенки. Останавливаемся на первой ячейке,
        // которая ещё не полностью твёрдая.
        for (int i = s.metal_cells - 1; i >= 0 && temperature[i] <= s.solidus; --i) {
            frame.shell_mm += mesh.cells[i].width() * 1000;
        }
        return frame;
    }

    double hottest_metal() const {
        return *std::max_element(temperature.begin(), temperature.begin() + s.metal_cells);
    }

    // Энтальпия всей расчётной области: металл, песок и опока.
    double stored_energy() const {
        double total = 0;
        for (std::size_t i = 0; i < temperature.size(); ++i) {
            total += mesh.cells[i].mass * specific_energy(i, temperature[i]);
        }
        return total;
    }

    double outgoing_energy = 0;
    std::optional<double> finished;

    // Температура принятого состояния меняется только после успешного решения.
    // При трудной нелинейной итерации вместо одного шага выполняем два половинных.
    void advance(double start, double dt) {
        const double previous = hottest_metal();
        if (!solve_step(dt)) {
            if (dt < 0.05) {
                throw std::runtime_error("Не удалось решить тепловую задачу. Уменьшите шаг времени.");
            }
            advance(start, dt / 2);
            advance(start + dt / 2, dt / 2);
            return;
        }
        temperature.swap(estimate);
        // Поток за шаг берём в новом состоянии, согласованно с неявной схемой.
        outgoing_energy += dt * mesh.conductance.back() * (temperature.back() - s.air_temperature);
        const double current = hottest_metal();
        // Полное затвердевание фиксируем по самой горячей ячейке металла.
        // Момент пересечения солидуса оцениваем линейной интерполяцией внутри шага.
        if (!finished && current <= s.solidus) {
            finished = start + dt * (previous - s.solidus) / (previous - current);
        }
    }

private:
    const Scenario& s;
    RadialMesh mesh;
    std::vector<double> temperature, estimate, candidate, residual, diagonal, correction;

    double specific_energy(std::size_t i, double t) const {
        if (i < static_cast<std::size_t>(s.metal_cells)) {
            return enthalpy(s, t);
        }
        return mesh.cells[i].material.heat_capacity * t;
    }

    // Невязка R_i = m_i[H(T_i)−H(T_i^стар)] + Δt·P_i, где P_i — сумма
    // потоков из ячейки. При верном новом состоянии R_i близка к нулю.
    double evaluate(const std::vector<double>& state, double dt) {
        double norm = 0;
        for (std::size_t i = 0; i < state.size(); ++i) {
            const double left = i == 0 ? state[i] : state[i - 1];
            const double right = i + 1 == state.size() ? s.air_temperature : state[i + 1];
            const double power = mesh.conductance[i] * (state[i] - left)
                               + mesh.conductance[i + 1] * (state[i] - right);
            const double change = specific_energy(i, state[i]) - specific_energy(i, temperature[i]);
            residual[i] = mesh.cells[i].mass * change + dt * power;
            // Масштаб в Дж/К позволяет сравнить кольца разной массы и проводимости.
            // Возвращаем максимальную по ячейкам нормированную невязку.
            const double scale = mesh.cells[i].mass * mesh.cells[i].material.heat_capacity
                                 + dt * (mesh.conductance[i] + mesh.conductance[i + 1]);
            norm = std::max(norm, std::abs(residual[i]) / scale);
        }
        return norm;
    }

    // Начинаем с температур предыдущего шага. Метод Ньютона строит линейную
    // поправку к текущему приближению, после чего проверяется исходная невязка.
    bool solve_step(double dt) {
        estimate = temperature;
        double norm = evaluate(estimate, dt);
        for (int iteration = 0; iteration < 35 && norm > 1e-8; ++iteration) {
            // Newton correction of the discrete enthalpy balance, solved by elimination.
            for (std::size_t i = 0; i < estimate.size(); ++i) {
                const Cell& cell = mesh.cells[i];
                double capacity = cell.material.heat_capacity;
                // Производная H по T увеличена на L/(T_liq−T_sol) только внутри
                // интервала кристаллизации. Это наклон энтальпии, а не добавочная энергия.
                if (i < static_cast<std::size_t>(s.metal_cells)
                    && estimate[i] > s.solidus && estimate[i] < s.liquidus) {
                    capacity += s.latent_heat / (s.liquidus - s.solidus);
                }
                const double lower = -dt * mesh.conductance[i];
                diagonal[i] = cell.mass * capacity + dt * (mesh.conductance[i] + mesh.conductance[i + 1]);
                correction[i] = -residual[i];
                // Прямой ход прогонки исключает левую переменную из текущего уравнения.
                // В матрице всего три диагонали из-за связи только с соседями.
                if (i > 0) {
                    const double factor = lower / diagonal[i - 1];
                    diagonal[i] -= factor * lower;
                    correction[i] -= factor * correction[i - 1];
                }
            }
            // Обратный ход: вычисляем поправки от последней ячейки к первой.
            for (std::size_t i = estimate.size(); i-- > 0;) {
                if (i + 1 < estimate.size()) {
                    correction[i] += dt * mesh.conductance[i + 1] * correction[i + 1];
                }
                correction[i] /= diagonal[i];
            }
            bool accepted = false;
            // Сокращаем ньютоновскую поправку, пока невязка не уменьшится.
            // Это особенно полезно около изломов энтальпии при солидусе и ликвидусе.
            for (double damping = 1; damping >= 1.0 / 1024; damping /= 2) {
                for (std::size_t i = 0; i < estimate.size(); ++i) {
                    candidate[i] = estimate[i] + damping * correction[i];
                }
                const double next = evaluate(candidate, dt);
                if (next < norm) {
                    estimate.swap(candidate);
                    norm = next;
                    accepted = true;
                    break;
                }
            }
            if (!accepted) {
                return false;
            }
        }
        return norm <= 1e-8;
    }
};

}

// Пределы защищают от некорректных исходных данных и слишком долгого
// запуска. Они являются ограничениями программы, а не нормами литья.
void validate(const Scenario& s) {
    for (const auto& z : s.zones) {
        check(z.diameter_mm, 10, 10000, "Диаметр участка должен быть от 10 до 10000 мм.");
        check(z.height_mm, 10, 30000, "Высота участка должна быть от 10 до 30000 мм.");
        check(z.sand_mm, 1, 3000, "Толщина песчаной формы должна быть от 1 до 3000 мм.");
        check(z.flask_mm, 1, 1000, "Толщина опоки должна быть от 1 до 1000 мм.");
    }
    for (auto material : {s.metal, s.sand, s.flask}) {
        check(material.density, 100, 30000, "Плотность должна быть от 100 до 30000 кг/м³.");
        check(material.heat_capacity, 10, 10000, "Теплоёмкость должна быть от 10 до 10000 Дж/(кг·К).");
        check(material.conductivity, 0.01, 500, "Теплопроводность должна быть от 0,01 до 500 Вт/(м·К).");
    }
    if (s.solid_density != 0) {
        check(s.solid_density, 100, 30000, "Плотность твёрдой фазы должна быть от 100 до 30000 кг/м³ или 0.");
    }
    check(s.solidus, 600, 1999, "Солидус должен быть от 600 до 1999 °C.");
    check(s.liquidus, s.solidus + 1, 2000, "Ликвидус должен быть выше солидуса минимум на 1 °C и не выше 2000 °C.");
    check(s.latent_heat, 0, 1000000, "Теплота кристаллизации должна быть от 0 до 1000000 Дж/кг.");
    check(s.metal_temperature, s.liquidus, 2200, "Начальная температура металла должна быть между ликвидусом и 2200 °C.");
    check(s.mold_temperature, -100, s.solidus - 1, "Начальная температура формы должна быть от −100 °C до значения ниже солидуса.");
    check(s.air_temperature, -100, s.solidus - 1, "Температура воздуха должна быть от −100 °C до значения ниже солидуса.");
    check(s.metal_sand_contact, 1, 100000, "Коэффициент контакта металл - песок должен быть от 1 до 100000 Вт/(м²·К).");
    check(s.sand_flask_contact, 1, 100000, "Коэффициент контакта песок - опока должен быть от 1 до 100000 Вт/(м²·К).");
    check(s.convection, 0, 10000, "Коэффициент наружной теплоотдачи должен быть от 0 до 10000 Вт/(м²·К).");
    check(s.hours, 0.01, 168, "Длительность расчёта должна быть от 0,01 до 168 ч.");
    check(s.max_step_s, 0.1, 300, "Максимальный шаг должен быть от 0,1 до 300 с.");
    check(s.metal_cells, 8, 360, "Число ячеек по радиусу металла должно быть от 8 до 360.");
    if (std::ceil(s.hours * 3600 / s.max_step_s) * s.metal_cells * zone_count > 10e6) {
        throw std::invalid_argument("Слишком большая задача. Сократите время расчёта или укрупните шаг.");
    }
}

// Линейное приближение для смеси. Для конкретной стали эту кривую
// следует определять по данным для фактического состава.
double solid_fraction(const Scenario& s, double temperature) {
    if (temperature <= s.solidus) return 1;
    if (temperature >= s.liquidus) return 0;
    return (s.liquidus - temperature) / (s.liquidus - s.solidus);
}

// Глубина от стенки переводится в дробный индекс от оси. Между центрами
// работает интерполяция, за крайними центрами берётся ближайшая ячейка.
double probe_temperature(const Frame& frame, const Zone& zone, double depth_mm) {
    const double count = static_cast<double>(frame.temperature.size());
    const double index = std::clamp((1 - 2 * depth_mm / zone.diameter_mm) * count - 0.5,
                                    0.0, count - 1);
    const auto i = static_cast<std::size_t>(index);
    return std::lerp(frame.temperature[i], frame.temperature[std::min(i + 1, frame.temperature.size() - 1)],
                     index - static_cast<double>(i));
}

// Сохраняем расчётные состояния на общей для всех участков шкале времени.
// В длинных прогонах прореживаем только вывод, сохраняя шаг интегрирования.
Result simulate(const Scenario& s) {
    validate(s);
    Result result;
    result.scenario = s;
    const double end = s.hours * 3600;
    const int steps = static_cast<int>(std::ceil(end / s.max_step_s));
    const int intervals = std::min(steps, 10000);
    const auto saved_step = [=](int frame) {
        return static_cast<int>(static_cast<long long>(frame) * steps / intervals);
    };
    const auto step_time = [=](int step) { return end * step / steps; };
    result.times.reserve(intervals + 1);
    for (int frame = 0; frame <= intervals; ++frame) {
        result.times.push_back(step_time(saved_step(frame)));
    }
    for (std::size_t z = 0; z < zone_count; ++z) {
        RadialSolver solver(s, s.zones[z]);
        auto& output = result.zones[z];
        const double initial_energy = solver.stored_energy();
        output.frames.reserve(result.times.size());
        output.frames.push_back(solver.sample(s.zones[z]));
        int frame = 1;
        for (int step = 1; step <= steps; ++step) {
            const double start = step_time(step - 1);
            solver.advance(start, step_time(step) - start);
            if (step == saved_step(frame)) {
                output.frames.push_back(solver.sample(s.zones[z]));
                ++frame;
            }
        }
        output.solidification_s = solver.finished;
        // Глобальный баланс: энергия, оставшаяся в системе, плюс отданная наружу.
        // Малая невязка проверяет реализацию, но не точность исходных свойств.
        output.energy_error = std::abs(solver.stored_energy() + solver.outgoing_energy - initial_energy)
                              / std::max(1.0, std::abs(initial_energy));
    }
    return result;
}

}
