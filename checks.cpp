// Автоматические проверки расчётного ядра. Код завершения 1 означает,
// что хотя бы одна проверка не прошла.
#include "project.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <sstream>
#include <stdexcept>

using namespace casting;

void require(bool condition, const char* label) {
    if (!condition) {
        throw std::runtime_error(label);
    }
}

int main() try {
    // Круговой тест формата: запись, чтение и проверка кириллицы, кавычек и чисел.
    Project project;
    project.title = "Валок \"А\"";
    std::stringstream file;
    write_project(file, project);
    const Project copy = read_project(file);
    require(copy.title == project.title && copy.scenario.zones[1].diameter_mm == 1450,
            "project round trip");
    // Моделируем повреждённый файл, добавив лишние данные после последней записи.
    file.clear();
    file.seekg(0);
    file.seekp(0, std::ios::end);
    file << "unexpected";
    try {
        read_project(file);
        throw std::logic_error("accepted trailing data");
    } catch (const std::runtime_error&) {
    }

    // Демонстрационный прогон проверяет время, энергию и баланс объёмов
    // на каждом участке, включая предельные положения датчика.
    const Result result = simulate(project.scenario);
    require(result.times.front() == 0 && result.times.back() == project.scenario.hours * 3600, "time axis");
    require(result.times.size() == 7201 && result.times[1] == 30,
            "demo timeline must expose the first 30-second frame");
    for (std::size_t z = 0; z < zone_count; ++z) {
        const auto& zone = result.scenario.zones[z];
        const double volume = std::numbers::pi * std::pow(zone.diameter_mm / 2000, 2)
                              * zone.height_mm / 1000;
        const auto& data = result.zones[z];
        require(data.energy_error < 1e-7, "energy conservation");
        require(std::abs(data.frames.front().liquid_zone - volume) < 1e-10, "initial volume");
        require(data.solidification_s.has_value(), "complete solidification within demo run");
        for (const auto& frame : data.frames) {
            require(std::abs(frame.solid_zone + frame.mushy_zone + frame.liquid_zone - volume) < 1e-10,
                    "sum of geometric volumes");
            require(std::abs(frame.solid_volume + frame.liquid_volume + frame.volume_deficit - volume) < 1e-10,
                    "sum of phase volumes and deficit");
            require(frame.shell_mm <= zone.diameter_mm / 2 + 1e-9, "shell thickness");
            require(probe_temperature(frame, zone, 0) == frame.temperature.back(), "wall probe");
            require(probe_temperature(frame, zone, zone.diameter_mm / 2) == frame.temperature.front(),
                    "axis probe");
        }
    }
    // Быстрая проверка, что отчёт содержит названия участков.
    std::stringstream csv;
    write_csv(csv, result, 400);
    require(csv.str().find("Бочка") != std::string::npos, "CSV has zone names");

    // Уточняем сетку и шаг одновременно. Проверяем изменение расчётного
    // времени затвердевания, а не совпадение с промышленным измерением.
    Scenario refined = project.scenario;
    refined.metal_cells *= 2;
    refined.max_step_s /= 2;
    const Result finer = simulate(refined);
    require(finer.times.size() == 10001 && finer.times.back() == result.times.back(),
            "thinned timeline must retain the final state");
    double max_delta = 0;
    for (std::size_t z = 0; z < zone_count; ++z) {
        const double reference = finer.zones[z].solidification_s.value();
        max_delta = std::max(max_delta, std::abs(result.zones[z].solidification_s.value() / reference - 1));
    }
    require(max_delta < 0.02, "time and radial mesh convergence");

    // При нулевой наружной теплоотдаче суммарная энергия сохраняется.
    // Внутри расчётной области продолжается теплообмен металла и формы.
    Scenario insulated = project.scenario;
    insulated.hours = 1;
    insulated.convection = 0;
    const auto closed = simulate(insulated);
    for (const auto& zone : closed.zones) {
        require(zone.energy_error < 1e-7, "insulated system energy conservation");
    }
    // Плотность твёрдой фазы влияет только на оценку объёма.
    insulated.solid_density = 0;
    const auto no_density = simulate(insulated);
    require(no_density.zones[0].frames.back().temperature == closed.zones[0].frames.back().temperature,
            "phase density is post-processing only");

    // Near-planar limit of a large cylinder with a nearly isothermal outer wall.
    Scenario stefan;
    stefan.zones.fill({10000, 1000, 1, 1});
    stefan.sand = stefan.flask = {100, 10, 500};
    stefan.metal_sand_contact = stefan.sand_flask_contact = 100000;
    stefan.convection = 10000;
    stefan.mold_temperature = stefan.air_temperature = 1000;
    stefan.liquidus = stefan.solidus + 1;
    stefan.metal_temperature = stefan.liquidus;
    stefan.hours = 1;
    stefan.max_step_s = 4;
    stefan.metal_cells = 360;
    const auto near_planar = simulate(stefan);
    // Для однофазного плоского приближения число Стефана сравнивает явную
    // и скрытую теплоты. Ищем параметр подобия λ методом бисекции.
    const double stefan_number = stefan.metal.heat_capacity
        * (stefan.solidus - stefan.air_temperature) / stefan.latent_heat;
    double lo = 0, hi = 2;
    for (int i = 0; i < 60; ++i) {
        const double x = (lo + hi) / 2;
        if (x * std::exp(x * x) * std::erf(x) * std::sqrt(std::numbers::pi) < stefan_number) {
            lo = x;
        } else {
            hi = x;
        }
    }
    const double diffusivity = stefan.metal.conductivity / (stefan.metal.density * stefan.metal.heat_capacity);
    const double analytical_mm = 1000 * (lo + hi) * std::sqrt(diffusivity * 3600);
    const double computed_mm = near_planar.zones[1].frames.back().shell_mm;
    const double stefan_error = std::abs(computed_mm / analytical_mm - 1);
    require(stefan_error < 0.10, "near-planar Stefan benchmark");

    std::cout << "All checks passed\nMesh/time refinement: " << 100 * max_delta << "%"
              << "\nStefan thickness: " << computed_mm << " mm vs " << analytical_mm
              << " mm (" << 100 * stefan_error << "%)\n";
    for (const auto& zone : result.zones) {
        std::cout << zone.solidification_s.value() / 3600 << " h, energy residual "
                  << zone.energy_error << '\n';
    }
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
