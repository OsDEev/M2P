# M2P — Super Smash Bros. Melee на ПК

[![pc-port](https://github.com/OsDEev/M2P/actions/workflows/pc-port.yml/badge.svg)](https://github.com/OsDEev/M2P/actions/workflows/pc-port.yml)

Нативный порт **Super Smash Bros. Melee (GALE01)** на Windows и Linux,
построенный поверх декомпиляции [doldecomp/melee](https://github.com/doldecomp/melee).
Игра собирается обычным компилятором (MSVC / GCC / Clang) в исполняемый
файл `melee_pc` — без эмуляции, напрямую через OpenGL, GLFW и
собственный системный слой SDK2.

> ⚠️ Статус: **платформа готова, играбельность — в работе**.
> HAL, SDK2, GUI и сборка написаны и покрывают 100% SDK-функций,
> используемых игрой. Остаток — перенос игровых данных под little-endian
> (см. [Дорожную карту](#дорожная-карта)).

Для запуска нужен **ваш собственный дамп диска GALE01**
(распакованный корень диска). Бинарники игры и её ассеты в репозиторий
не входят.

## Быстрый старт

### Требования

- Windows 10+ (MSVC 2022) или Linux x86-64 (GCC 11+ / Clang 14+)
- CMake 3.20+, C99/C++17
- Системный OpenGL
- Linux: `libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev`
- Интернет при первом configure (GLFW 3.4 и Dear ImGui тянутся через FetchContent)

### Сборка

```sh
cmake -S . -B build_pc -DCMAKE_BUILD_TYPE=Release
cmake --build build_pc --config Release --parallel
```

Без сети (без ImGui-оверлея, останется консольный конфигуратор):

```sh
cmake -S . -B build_pc -DMELEE_WANT_IMGUI=OFF
```

### Запуск

```sh
./build_pc/bin/melee_pc --disc <корень диска GALE01>
```

Полезные ключи:

| Ключ | Назначение |
|---|---|
| `--disc PATH` | корень распакованного диска |
| `--user PATH` | каталог сейвов и настроек |
| `--card-a/b PATH` | образы карт памяти |
| `--width/--height N` | размер окна (`0` = авто) |
| `--fullscreen` | полноэкранный режим |
| `--no-gui` | без оверлея настроек |
| `--configure` | консольный редактор настроек |
| `--save-settings` | записать ini и выйти |
| `--help` | все опции |

## Управление

Первый контроллер — клавиатура + первый геймпад (XInput/SDL через GLFW).
Раскладка клавиатуры по умолчанию (меняется в настройках):

| Действие | Клавиша |
|---|---|
| Стик | WASD |
| C-стик | стрелки |
| A / B | J / K |
| X / Y | L / I |
| L / R | U / O |
| Z | P |
| Start | Enter |
| D-pad | T F G H |

## Настройки

- **F1** в игре — оверлей настроек (сборка с ImGui): видео, звук,
  ввод с ремаппингом, пути, системные параметры.
- Без ImGui: `melee_pc --configure` (консольное меню).
- Файл: `melee_pc.ini` (`%APPDATA%\melee-pc` / `~/.melee-pc/`).

## Архитектура

```
melee_pc
├── src/pc/        main(), CLI, endian-стадия-1, low-memory модель
├── src/gui/       INI-настройки + ImGui-оверлей + консольный редактор
├── src/hal/       «железо»: GX→OpenGL, VI/окно, PAD, AI
│   ├── shim/      подмены заголовков (FIFO 0xCC008000 не существует на ПК)
│   └── opengl/    контекст, TEV-шейдеры, сборщик вершин, декодеры текстур
└── src/sdk2/      системное ПО: OS, DVD, CARD, AR, AX, MTX, THP, EXI/SI, MCC
```

Игра (`src/melee`, `src/sysdolphin`) компилируется как есть, кроме
`archive.c` (endian-aware замена) и `main()` (`-Dmain=melee_main`).
HW-модули оригинального SDK в PC-сборку не входят — их символы дают
HAL и SDK2 (проверено свипом: покрыты все ~470 SDK-вызовов игры).

### Ключевые решения

- **TEV → GLSL**: стадии комбинирования генерируют фрагментный шейдер
  (MODULATE/DECAL/BLEND/REPLACE/PASSCLR, KONST-регистры, fog,
  alpha-compare); программы кэшируются по хэшу состояния.
- **Текстуры GameCube** декодируются побайтово (I4/I8/IA4/IA8/RGB565/
  RGB5A3/RGBA8/C4/C8/C14X2/CMPR с инвертированным сравнением DXT1).
- **Аудио**: программный AX-микшер (ADPCM/PCM, pitch, огибающие, лупы,
  64 голоса) → AI ring buffer → waveOut / null-sink.
- **Карта памяти**: контейнер `M2PCARD1` на слот.
- **XFB**: EFB блитится в окно, а game-копия конвертируется в YUYV
  (иначе переполнение буфера в 2 раза).
- **Адресная модель**: арена около `0x80000000` (как MRAM), ARAM —
  оффсеты < 16 МБ; `.exe` линкуется с низкой базой, т.к. часть
  указателей идёт через `u32`, а `lbmemory` отличает MRAM от ARAM
  проверкой `< 0x80000000`.

## Дорожная карта

- [x] Git, HAL OpenGL, SDK2, GUI настроек, CMake, CI
- [x] Endian-каркас: структура архивов (`pc_endian.c`), BE-ридеры везде
      в новом коде
- [ ] **Стадия 2 — данные**: endian-safe чтение в загрузчиках HSD
      (`jobj/tobj/pobj/mobj/aobj/...`), SFX-банках (`synth.c`), прямых
      чтениях моделей/анимаций
- [ ] THP-видео (сейчас blank-playback), aux-эффекты, rumble
- [ ] Первый загружаемый билд → отладка по CI

## Разработка

- Ветки: `master` (здесь вся работа).
- CI `.github/workflows/pc-port.yml`: Windows (MSVC, ±ImGui) + Linux (GCC).
- Верхнеуровневый decomp-флоу (`configure.py`, MWERKS) не тронут и
  продолжает работать как раньше.
- Лицензия исходников декомпиляции — как в апстриме; новый код порта —
  в тех же файлах без отдельных заголовков. Для игры нужен ваш
  легальный диск.

## Благодарности

[doldecomp/melee](https://github.com/doldecomp/melee),
[GLFW](https://www.glfw.org/), [Dear ImGui](https://github.com/ocornut/imgui).
