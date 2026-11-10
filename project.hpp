// Контракт хранения проектов и выгрузки результатов.
#pragma once

#include "model.hpp"

#include <iosfwd>
#include <string>

namespace casting {

// Проект хранит параметры и их происхождение. Температурная история
// восстанавливается при открытии вычислением той же версии модели.
struct Project {
    Scenario scenario;
    std::string title = "Демонстрационный валок";
    std::string source = "Условные значения для проверки программы";
};

// Запись версионированного проекта. Поток задаёт вызывающая сторона.
void write_project(std::ostream& file, const Project& project);
// Чтение с проверкой порядка полей, версии и диапазонов.
Project read_project(std::istream& file);
// Одна строка CSV на кадр участка. depth_mm — глубина от боковой стенки.
void write_csv(std::ostream& file, const Result& result, double depth_mm);

}
