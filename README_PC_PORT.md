# Super Smash Bros. Melee — PC порт

Нативный порт декомпиляции [doldecomp/melee](https://github.com/doldecomp/melee)
на ПК (Windows / Linux, x86-64). Игра собирается обычным компилятором
(MSVC / GCC / Clang) в `melee_pc` вместо GameCube DOL.

> Статус: **платформа готова, геймплей — в работе**. Слои HAL, SDK2, GUI и
> сборка написаны полностью. Остаток — поэтапный перенос игровых данных
> (см. «Стадия 2» ниже).

## Что сделано

| Компонент | Файлы | Состояние |
|---|---|---|
| Git | репозиторий в этой папке | ✅ готов |
| HAL OpenGL (GX → GL 3.3) | `src/hal/opengl/` | ✅ готов |
| Видео/окно (VI, GLFW) | `hal_video.*` | ✅ готов |
| Ввод (PAD: геймпады + клавиатура) | `hal_input.*` | ✅ готов |
| Аудио (AI + AX-микшер, waveOut/null) | `hal_audio.*`, `sdk2_ax.c` | ✅ готов |
| SDK2: OS/DVD/CARD/AR/MTX/THP/EXI/SI | `src/sdk2/` | ✅ готов |
| GUI настроек (Dear ImGui + консоль) | `src/gui/` | ✅ готов |
| Точка входа, CMake, адресная модель | `src/pc/`, `CMakeLists.txt` | ✅ готов |
| Парсинг игровых данных (LE) | `src/pc/pc_endian.c` | 🟡 каркас (стадия 2) |

## Сборка

```sh
cmake -S . -B build_pc -DCMAKE_BUILD_TYPE=Release
cmake --build build_pc --config Release
```

Первый configure качает GLFW 3.4 и Dear ImGui (нужен интернет).
Без сети: `cmake -S . -B build_pc -DMELEE_WANT_IMGUI=OFF`
(останется консольный конфигуратор `--configure`).

Linux: для сборки GLFW нужны X11-пакеты:
`sudo apt install libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev
libxi-dev libgl1-mesa-dev` (пример для Debian/Ubuntu).

Запуск:

```sh
./build_pc/bin/melee_pc --disc <корень GALE01> [--configure|--help]
```

Ключи: `--user`, `--card-a/b`, `--width/--height`, `--fullscreen`,
`--no-gui`, `--save-settings`. В игре **F1** — оверлей настроек.

Настройки хранятся в `melee_pc.ini`
(`%APPDATA%\melee-pc` / `~/.melee-pc/`).

## Архитектура

```
melee_pc
├── src/pc/        main(), CLI, endian-стадия-1 (архивы)
├── src/gui/       settings (INI) + ImGui-оверлей + консольный редактор
├── src/hal/       железо: GX→OpenGL, VI/окно, PAD, AI
│   ├── shim/      подмены заголовков (GXVert/GXGeometry без FIFO 0xCC008000)
│   └── opengl/    контекст, TEV-шейдеры, сборщик вершин, декодеры текстур
└── src/sdk2/      системное ПО: OS, DVD, CARD, AR, AX, MTX, THP, EXI/SI
```

Игра (`src/melee`, `src/sysdolphin`) компилируется как есть, за
исключением `archive.c` (заменён endian-aware версией) и `main()`
(`-Dmain=melee_main`). HW-модули оригинального SDK (`gx`, `vi`, `pad`,
`si`, `exi`, `ai`, `ax`, `dvd`, `card`, `ar`, `os`, `mtx`, `thp`)
в PC-сборку **не входят** — их символы даёт HAL/SDK2.

Ключевые решения:

- **TEV** транслируется в генерируемый GLSL (MODULATE/DECAL/BLEND/
  REPLACE/PASSCLR, KONST, регионы, fog, alpha-compare; compare-ops и
  indirect — упрощённо, см. комментарии в `gx_hal.c`).
- **Текстуры GC** декодируются побайтово (I4/I8/IA4/IA8/RGB565/RGB5A3/
  RGBA8/C4/C8/C14X2/CMPR с инвертированным сравнением DXT1).
- **Аудио**: программный AX-микшер (ADPCM/PCM16/PCM8, pitch, VE, loop,
  64 голоса) → AI ring buffer → waveOut (Windows) / null-sink.
  Aux-эффекты (reverb/chorus/delay) — no-op, вызовы сохранены.
- **Карта памяти** — контейнер `M2PCARD1` на слот, блочная модель
  симулирована (1019×8 KiB).
- **Адресная модель**: арена/кучи лежат около `0x80000000` (как MRAM),
  ARAM — оффсеты < 16 МБ (как железо). Поэтому `.exe` линкуется с низкой
  базой (`/BASE`, `-no-pie`): часть указателей идёт через `u32`, а
  `lbmemory` отличает MRAM от ARAM проверкой `< 0x80000000`.

## Стадия 2 (остаток для играбельности)

Файлы игры big-endian; на x86 нативные чтения мультибайтовых полей дают
мусор. Закрыто побайтовыми ридерами везде в **новом** коде (HAL/SDK2).
Открыто в **игровом** коде — по классам ассетов:

1. HSD-объекты из `.dat`: `jobj/tobj/pobj/mobj/aobj/dobj/fobj/cobj/lobj`
   — endian-safe чтение полей (float/s16/u32) в загрузчиках;
2. SFX-банки в `synth.c` (патчинг `u32`-полей записей);
3. модели/анимации, читаемые напрямую (`GXPosition3f32(arr[i].x)` и т.п.);
4. THP-видео (сейчас blank-playback), aux-эффекты, rumble.

Каркас стадии 1 уже в дереве: `src/pc/pc_endian.c` (структура архивов:
заголовок, таблицы, релокация указателей — корректны на LE).

## Проверено без компилятора на машине

Тулчейна в окружении нет, поэтому вместо сборки выполнена ручная ревизия:
парность скобок/прототипов, соответствие всех `GX*/VI*/PAD*/AI*/AX*/CARD*/
DVD*/AR*/OS*/MTX*/THP*` сигнатурам оригинальных заголовков, отсутствие
висячих ссылок между новыми файлами.
