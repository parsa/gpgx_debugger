# smd_dgx architecture

Цель: рабочий эмулятор (Genesis Plus GX) с debug-API, к которому подключается
клиент. Основной клиент — IDA (связка ida + gens из smd_ida_tools2), плюс
standalone Qt-приложение для разработки/проверки.

```
┌────────────────────────────────────────────────────────────────┐
│ Hosts                                                          │
│   smd_dgx_qt6  — standalone Qt app (это репо)                  │
│   IDA plugin   — будущий хост: статически линкует gx_core +    │
│                  debugger, эмулятор крутится в отдельном       │
│                  потоке ТОГО ЖЕ процесса                       │
├────────────────────────────────────────────────────────────────┤
│ Views (frontends/qt6/views/*)                                  │
│   Чистые QWidget'ы. Зависят ТОЛЬКО от debugger/DebugState.h    │
│   и debugger/IDebugBackend.h — ни одного инклюда ядра.         │
│   IDA — Qt-приложение, поэтому эти же виджеты можно            │
│   встраивать в IDA-плагин (create_empty_widget + layout).      │
├────────────────────────────────────────────────────────────────┤
│ Debug API                                                      │
│   IDebugBackend  — синхронный интерфейс состояния (регистры,   │
│                    память/регионы, VDP, звук, брейкпоинты,     │
│                    run-control). In-proc реализация:           │
│                    GpgxBackend.                                │
│   DebugApi.h     — тот же функционал как команды/события       │
│                    (DebugCommand/DebugEvent) для транспорта.   │
│                    Минимальная поверхность выведена из         │
│                    protobuf-протокола smd_ida_tools2 (см.      │
│                    спек: только реально используемые IDA RPC). │
├────────────────────────────────────────────────────────────────┤
│ Transports (debugger/)                                         │
│   InProcTransport — очереди команд/событий между потоками      │
│                     одного процесса (статическая линковка).    │
│   [план] SharedMemTransport — межпроцессный ring-buffer,       │
│                     когда эмулятор живёт отдельным процессом.  │
│   [опция] protobuf-адаптер поверх DebugApi для legacy          │
│                     Gensida — protobuf наружу не торчит.       │
├────────────────────────────────────────────────────────────────┤
│ Emulator core                                                  │
│   gx_core (core/*) + core/debug/cpuhook (HOOK_CPU)             │
│   GpgxBackend вешает cpu_hook, ведёт паузу/степы,              │
│   m68k_instr_callback -> HOOK_M68K_E на каждой инструкции.     │
└────────────────────────────────────────────────────────────────┘
```

## Контракты, которые нельзя ломать

- **Байт-порядок**: ядро на LSB_FIRST хранит ROM/RAM68K/VRAM word-swapped.
  `readMemory`/`readRegion` отдают ЛОГИЧЕСКИЙ порядок (backend делает `^1`).
  Сырые указатели `VdpState.vram/sat` — как в ядре: логический байт A =
  `ptr[A^1]`; cram/vsram читаются 16-битными LE-словами.
- **Регионы памяти** (id стабильны): 0 ROM, 1 RAM 68K (0xFF0000), 2 RAM Z80
  (0xA00000), 3 VRAM, 4 CRAM, 5 VSRAM, 6 Regs M68K, 7 Regs Z80, 8 Regs VDP.
  Паритет с hex-редактором Gens.
- **VDP-брейкпоинт-пространство** (наследие протокола IDA): VRAM +0x0000,
  CRAM +0x10000, VSRAM +0x20000; в IDA это псевдосегменты от 0xD00000.
- **Codemap** (`changed`: executed pc -> predecessor) шлётся с каждым
  pause/stop событием и атомарно очищается — IDA строит по нему код.

## IDA-плагин (фаза 2 — скелет готов, ждёт SDK)

Цель: **IDA 9.2+ (юзер ставит 9.3)** — первая IDA на стоковом Qt 6.8.2, том же,
что и наши вьюшки. Никакого спец-Qt как в ≤9.1.

Состояние:
1. ✅ `smd_dgx_emu` — статик-либа (gx_core + GpgxBackend + InProcTransport +
   EmuHost), без Qt. Собирается, слинкована в оба фронта.
2. ✅ `debugger/EmuHost` — headless-раннер: эмулятор на std::thread, sinks для
   кадров/событий, диспетчер DebugCommand, pause-pump (вложенный цикл Gens).
   Backend копит codemap (executed pc → predecessor) для `apply_codemap`.
3. ✅ `ida_plugin/smd_dgx_ida.cpp` — порт Gensida ida_debug.cpp: debugger_t +
   HT_IDD-диспетчер, gRPC-клиент заменён прямыми вызовами backend().
   Маркеры VERIFY-9.3 там, где сигнатуры надо сверить с 9.3 SDK.
   Память в IDA роутится через регионы (side-effect-free, IO-хендлеры не
   трогаются): ROM/RAM/Z80-окно/VDP-псевдосегменты 0xD00000.
4. ✅ `ida_plugin/CMakeLists.txt` — gated: без `-DIDA_SDK_DIR=...` таргет
   пропускается. `-DIDA_INSTALL_DIR=...` включает post-build copy в plugins/.
5. ✅ Все 3 TU плагина КОМПИЛИРУЮТСЯ против настоящих заголовков ida-sdk
   v9.3.0-sdk.3 (склонированы в scratchpad/sdk93repo). Сверено по реальным
   idd.hpp/loader.hpp/kernwin.hpp. Ключевые факты 9.2/9.3:
   - Плагин = PLUGIN_MULTI + plugmod_t : event_listener_t (on_event→HT_IDD),
     как родные dbg-модули SDK (не старый init/term/PLUGIN_KEEP).
   - `register_info_t.register_class` ПЕРЕИМЕНОВАН в `register_class_mask`
     (9.1→9.2); binary layout тот же (uchar), но код на 9.1 не скомпилится.
   - debug_event_t: set_modinfo(id)/set_eid(id)/set_exit_code(id,code), eid().
   - PH.id==PLFM_68K (7) — гейт на 68000-базу.
6. ✅ Фаза 3 (вьюшки в IDA) — код готов. КРИТИЧНО: IDA pro.h и Qt6
   qbytearrayalgorithms.h ОБА безусловно определяют qstrlen/qsnprintf/... inline
   → коллизия (на Qt5/IDA≤9.1 её не было). Решение: IDA и Qt НИКОГДА не в одном
   TU. ida_dock.cpp = IDA-glue (create_empty_widget/actions, без Qt),
   ida_views.cpp = Qt-виджеты (без IDA-заголовков), граница = void*(TWidget→
   QWidget) в ida_views_shared.h. Плюс: метод EmuHost::emit переименован в
   emitEvent (Qt-макрос `emit`).
7. ⏳ Осталось при установленном SDK: реально слинковать smd_dgx_ida.dll
   (ida.lib), проверить рантайм в IDA; VERIFY-9.3-маркеры — точки, где API
   подтверждён по заголовкам, но не проверен в живой IDA.

Сборка плагина (когда SDK установлен):
```
cmake --preset x64-debug -DIDA_SDK_DIR=<sdk> -DIDA_INSTALL_DIR=<ida> 
ninja smd_dgx_ida
```
