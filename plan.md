# План: контент-конвейер (content_clipper)

## Цель
Оконное приложение на Qt6/C++ для автоматического создания виральных вертикальных нарезок (шортсов/рилсов) из видеофайлов (MKV, MP4) с прожжёнными субтитрами слово-за-словом, на русском и английском языках.

## Полный пайплайн
1. Выбор файла/папки в UI → очередь.
2. ffprobe → JSON метаданные.
3. Парсинг: главы, аудиодорожки (RU/EN индексы), субтитры (RU/EN, полные/forced/SDH), длительность, название.
4. Извлечение нужных субтитров (RU/EN/оба — по настройке) через `ffmpeg -map 0:s:X`. Если сабов нет → whisper-транскрибация (`--vad -mc 0`).
5. Мультиагентный LLM-анализ: Аналитик → Скаут → Продюсер → строгий JSON с 3–7 хайлайтами.
6. Для каждого хайлайта, для каждого языка (RU + EN): извлечь аудио-сегмент → whisper word-level сабы (по одному слову) → один проход ffmpeg (нарезка + 9:16 + прожиг сабов + нужная аудиодорожка) → `clip_N.mp4`.

### Режим 2: «Пересказ 2.0» (методология video-use)
LLM читает не сырые сабы, а packed-транскрипт (фразы по паузам + опц. метки спикеров pyannote) и возвращает EDL с драматургией: склейки-биты (HOOK/CONFLICT/CLIMAX/...) с quote/reason + caption. Источник транскрипта: whisper word-level по всему фильму (кэш) или сабы из файла. Рендер: склейка с afade 30мс (или кроссфейдами) + word-сабы по склеенному аудио + 9:16 + прожиг последними. Вывод: `story_NN.mp4` + `stories.json` + `story_NN_caption.txt` + `movie_packed.md`.

### Доработка качества (актуально)
- **Снаппинг границ** (`SrtUtils`): границы клипов/склеек прилипают к границам реплик SRT (+паддинги `padStartSec`/`padEndSec`), близкие склейки сливаются (`mergeGapSec`) — нет обрывов на полуслове.
- **DTW** (`whisperDtw`): точное выравнивание word-сабов по звуку (`-dtw <пресет> -nfa`, пресет из имени модели).
- **Кроссфейды** (`storyCrossfadeSec`): плавные переходы между склейками в пересказе (одинаковый фейд в видео и в wav для whisper).
- **Кадры в LLM** (`llmSendFrames`, `frameIntervalSec`): визуальный контекст для пересказа.

## Модули
- **AppSettings** — настройки (settings.json рядом с exe + вкладка в UI): пути к ffmpeg/ffprobe/whisper-cli/модели, LLM endpoint/key/model, `subsForLlm` (RU/EN/оба), `renderLanguages` (RU/EN), лимиты клипов.
- **MetadataParser** — разбор JSON ffprobe в `MediaInfo` (главы, дорожки, классификация RU/EN, выбор полных сабов, не forced/SDH). Байты ffprobe трактовать строго как UTF-8.
- **MediaExtractor** — ffprobe + извлечение субтитров (`-map 0:s:N`) + извлечение аудио в wav 16k mono (полное или сегмент).
- **LlmClient** — OpenAI-совместимый API (`POST {baseUrl}/chat/completions`). Аналитик (синопсис) → Скаут (кандидаты, текст бьётся на чанки по ~60к символов) → Продюсер (строгий JSON: title, start_time, end_time, duration_sec, media_info, description, virality_reason). Чистка markdown, валидация таймкодов, 1 ретрай.
- **WhisperRunner** — fallback-транскрибация (`--vad -mc 0`) и word-level сабы для клипов (`-ml 1 -sow`, `-l ru/en`).
- **ClipRenderer** — двуязычный рендер: RU-версия (русская аудио + русские word-сабы) и EN-версия (англ. аудио + англ. word-сабы). Один проход ffmpeg: `-ss/-t` + 9:16 blur-background + `subtitles=...:force_style=...` + `-map 0:a:N` → mp4 (libx264+aac). Вывод: `<basename>_clips/ru/clip_01.mp4`, `<basename>_clips/en/clip_01.mp4` + `clips.json`. Если дорожки нет — версия пропускается с предупреждением. ffmpeg запускается с рабочей папкой клипов (обход проблем с путями Windows в фильтре subtitles).
- **PipelineManager** — конечный автомат: `Probe → Parse → ExtractSubs (→WhisperFallback) → LLM → [клипы: Audio→Whisper→Render для RU и EN] → Done → следующий файл`.
- **UI** — выбор файла и папки, вкладка настроек, лог прогресса, таблица найденных клипов.

## Порядок реализации
1. AppSettings + настройки в UI, Qt Network в CMake.
2. Полный MetadataParser + сводка дорожек в лог.
3. Извлечение сабов (+ whisper-fallback).
4. LlmClient: 3 агента, парсинг JSON, ретрай.
5. ClipRenderer: нарезка → whisper word-subs → прожиг 9:16 (RU+EN).
6. Склейка в PipelineManager, пакетная обработка.
7. Обработка ошибок, очистка temp-файлов, полировка.
