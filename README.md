# NwRemover

плагин для after effects, автоматически находит мат в аудиодорожке и заменяет его. распознавание через локальный whisper.cpp или groq api

## возможности

- два бэкенда, локальный whisper.cpp (приоритет) и groq api
- пресеты платформ youtube, tiktok, reels, twitch, задают строгость словаря
- замена на бип, тишину, реверс, питч-шифт или свой звук
- свой звук замены, любой wav, зацикливается на зону мата
- ручной мут ползунком, глушит выбранный момент поверх автодетекций
- словарь русского мата и английского, три уровня строгости, whitelist
- пул groq-ключей с ротацией и fallback на 429/401
- питч бипа, свип, форма волны, громкость, fade
- padding и merge зазоров между вырезками
- вырезки сохраняются в проекте

## установка

- скачай NwRemover.aex из релиза
- положи в `C:\Program Files\Adobe\Adobe After Effects 20XX\Support Files\Plug-ins`
- скачай модели whisper через `download_models.cmd base`, tiny и base нужны для старта
- модели кладутся в `D:\AEplugin\models` или в каталог `models` рядом с плагином

## использование

- примени эффект NwRemover (категория Audio) к аудиослою
- выбери backend, модель, язык
- выбери пресет платформы или настрой строгость вручную
- нажми Analyze либо включи Auto-analyze on render
- выбери замену и её настройки
- для своего звука пропиши `replacement_sound` в NwRemover.json
- для groq впиши ключи в NwRemover.json, массив `groq_api_keys`
- кнопка Report показывает найденные слова с таймкодами

## конфиг NwRemover.json

лежит рядом с плагином или в `D:\AEplugin`. поля:

- `models_dir`, каталог моделей whisper
- `model_file`, явный путь модели для выбора from config
- `groq_api_key`, один ключ
- `groq_api_keys`, массив ключей, ротация round-robin
- `replacement_sound`, wav для режима Custom sound
- `custom_words`, свои слова в словарь
- `whitelist`, глобальные исключения

импорт ключей из формата `GROQ_API_KEY_N=gsk_...`:

```
python import_groq_keys.py C:\path\groq.txt
```

## сборка

нужны gcc (msys2 ucrt64), cmake, ninja. third_party не в репо, склонируй отдельно:

- https://github.com/ggml-org/whisper.cpp в `third_party/whisper.cpp`
- https://github.com/Trentonom0r3/AETK в `third_party/AETK` (заголовки AE SDK)

whisper.cpp собирается без AVX2/BMI2/FMA/F16C под старые cpu:

```
cmake -S third_party/whisper.cpp -B build/whisper -G Ninja -DGGML_NATIVE=OFF -DGGML_AVX=ON -DGGML_AVX2=OFF -DGGML_BMI2=OFF -DGGML_FMA=OFF -DGGML_F16C=OFF -DWHISPER_BUILD_EXAMPLES=OFF -DWHISPER_BUILD_TESTS=OFF -DBUILD_SHARED_LIBS=OFF
cmake --build build/whisper --target whisper
cmake -S . -B build/plugin -G Ninja -DCMAKE_RC_COMPILER=<ucrt64>/windres.exe
cmake --build build/plugin
```

PiPL-ресурс генерирует `pipl_gen.py` в `NwRemover.rc`. плагин собирается gcc-MinGW без MSVC, интерфейс AE это C-ABI x64

## тесты

- `test_pipeline dict`, словарь и уровни строгости
- `test_host`, загрузка .aex, PiPL, GLOBAL_SETUP
- `test_pipeline <wav> <model> <lang> <out> <mode> [sound.wav]`, полный прогон, mode 1 бип 2 тишина 3 реверс 4 питч 5 свой звук

## ограничения

- groq-путь написан, вживую не проверен, нужны ключи
- анализ синхронный, без отмены посреди whisper
- точность зависит от модели, base ошибается на сложном мате, для тяжёлых случаев бери larger или groq
- AE 2026 не проверялся, тестировано на AE 2023