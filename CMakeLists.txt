# Сборка общего ядра, окна Windows и контрольной программы.
cmake_minimum_required(VERSION 3.20)
project(RollCasting VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)
# Ядро и файловый формат собираются без привязки к Win32.
add_library(casting_model model.cpp project.cpp)
target_compile_features(casting_model PUBLIC cxx_std_20)

# Компилятору MSVC явно указываем UTF-8 для кириллических строк.
if(MSVC)
    add_compile_options(/utf-8)
    target_compile_options(casting_model PRIVATE /utf-8)
endif()

# Графическая часть существует только на Windows. UNICODE выбирает
# широкосимвольные варианты функций Win32.
if(WIN32)
    add_executable(RollCasting WIN32 main.cpp view.cpp)
    target_compile_definitions(RollCasting PRIVATE UNICODE _UNICODE)
    target_link_libraries(RollCasting PRIVATE casting_model comctl32 comdlg32 gdi32)
    # Для MinGW выбираем wWinMain и статически связываем библиотеки компилятора.
    if(MINGW)
        target_link_options(RollCasting PRIVATE -municode -static)
    endif()
endif()

# Контрольные задачи запускаются отдельно через CTest.
include(CTest)
if(BUILD_TESTING)
    add_executable(casting_checks checks.cpp)
    target_link_libraries(casting_checks PRIVATE casting_model)
    add_test(NAME casting_checks COMMAND casting_checks)
endif()
