// Преобразует структуры модели в текстовый .roll и табличный .csv.
#include "project.hpp"

#include <iomanip>
#include <locale>
#include <stdexcept>

namespace casting {
namespace {

// Ключи идут в фиксированном порядке. Ошибка в ключе предотвращает
// сдвиг столбцов и подстановку значения в другое поле.
void read_key(std::istream& file, const char* expected) {
    std::string key;
    if (!(file >> key) || key != expected) {
        throw std::runtime_error("Некорректный формат проекта или отсутствует параметр.");
    }
}

template<class... Values>
void read_record(std::istream& file, const char* key, Values&&... values) {
    read_key(file, key);
    if (!(file >> ... >> values)) {
        throw std::runtime_error("Некорректное значение в файле проекта.");
    }
}

template<class... Values>
void write_record(std::ostream& file, const char* key, const Values&... values) {
    file << key;
    ((file << ' ' << values), ...);
    file << '\n';
}

// Общий перечень записей для чтения и записи. Self выводится как
// Project или const Project, а visit выполняет нужную операцию.
template<class Self, class Visitor>
void project_records(Self& p, Visitor visit) {
    auto& s = p.scenario;
    visit("title", std::quoted(p.title));
    visit("source", std::quoted(p.source));
    for (auto& z : s.zones) {
        visit("zone", z.diameter_mm, z.height_mm, z.sand_mm, z.flask_mm);
    }
    for (auto* m : {&s.metal, &s.sand, &s.flask}) {
        visit("material", m->density, m->heat_capacity, m->conductivity);
    }
    visit("phase", s.solid_density, s.solidus, s.liquidus, s.latent_heat);
    visit("initial", s.metal_temperature, s.mold_temperature, s.air_temperature);
    visit("transfer", s.metal_sand_contact, s.sand_flask_contact, s.convection);
    visit("solver", s.hours, s.max_step_s, s.metal_cells);
}

}

// Классическая локаль фиксирует десятичную точку независимо от Windows.
// 17 значащих цифр сохраняют значения double при повторном открытии.
void write_project(std::ostream& file, const Project& project) {
    validate(project.scenario);
    file.imbue(std::locale::classic());
    file << std::setprecision(17);
    write_record(file, "roll-casting", model_version);
    project_records(project, [&](const char* key, const auto&... values) {
        write_record(file, key, values...);
    });
    if (!file) {
        throw std::runtime_error("Не удалось записать проект.");
    }
}

// Проверяем заголовок до интерпретации остальных параметров.
Project read_project(std::istream& file) {
    file.imbue(std::locale::classic());
    int version = 0;
    read_record(file, "roll-casting", version);
    if (version != model_version) {
        throw std::runtime_error("Версия формата проекта не поддерживается.");
    }
    Project project;
    project_records(project, [&](const char* key, auto&&... values) {
        read_record(file, key, values...);
    });
    // После последней записи допустимы только пробельные символы.
    file >> std::ws;
    if (!file.eof() || file.bad()) {
        throw std::runtime_error("Неожиданные данные в конце файла проекта.");
    }
    if (project.title.size() > 240 || project.source.size() > 720
        || project.title.find_first_of("\r\n") != std::string::npos
        || project.source.find_first_of("\r\n") != std::string::npos) {
        throw std::runtime_error("Некорректное описание проекта.");
    }
    validate(project.scenario);
    return project;
}

// UTF-8 BOM помогает распознать кириллицу, а точка с запятой разделяет
// столбцы. Состав строк определяется сохранёнными кадрами расчёта.
void write_csv(std::ostream& file, const Result& result, double depth_mm) {
    constexpr const char* labels[] = {"Нижняя шейка", "Бочка", "Верхняя шейка", "Прибыль"};
    file.imbue(std::locale::classic());
    file << std::setprecision(10) << "\xEF\xBB\xBF"
         << "Версия модели;Участок;Время с;Корка мм;Твёрдая зона м³;Двухфазная зона м³;"
            "Жидкая зона м³;Твёрдая фаза м³;Жидкая фаза м³;Дефицит объёма м³;"
            "Ось °C;У стенки °C;Глубина датчика мм;Датчик °C;Полное затвердевание с;Невязка энергии доля\n";
    for (std::size_t z = 0; z < zone_count; ++z) {
        const auto& zone = result.scenario.zones[z];
        const auto& data = result.zones[z];
        for (std::size_t i = 0; i < result.times.size(); ++i) {
            const auto& f = data.frames[i];
            file << model_version << ';' << labels[z] << ';' << result.times[i] << ';' << f.shell_mm
                 << ';' << f.solid_zone << ';' << f.mushy_zone << ';' << f.liquid_zone << ';';
            if (result.scenario.solid_density > 0) {
                file << f.solid_volume << ';' << f.liquid_volume << ';' << f.volume_deficit;
            } else {
                file << ";;";
            }
            file << ';' << f.temperature.front() << ';' << f.temperature.back() << ';';
            // Глубина может быть допустима для бочки и оказаться за осью узкой шейки.
            // Для такой шейки поля датчика остаются пустыми.
            if (depth_mm <= zone.diameter_mm / 2) {
                file << depth_mm << ';' << probe_temperature(f, zone, depth_mm);
            } else {
                file << ';';
            }
            file << ';';
            if (data.solidification_s) {
                file << *data.solidification_s;
            }
            file << ';' << data.energy_error << '\n';
        }
    }
    if (!file) {
        throw std::runtime_error("Не удалось записать результаты.");
    }
}

}
