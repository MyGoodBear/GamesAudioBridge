# Сборка Game Audio Bridge в Windows 10/11

## Требования
- Visual Studio 2022 (или Build Tools) с **Desktop development with C++**, MSVC x86/x64.
- CMake 3.21+.
- .NET Framework 4.x compiler (`csc.exe`).
- Для установщика — Inno Setup 6.
- Для загрузки ASIO SDK — Git и доступ к GitHub.

## Сборка
Откройте **Developer PowerShell for VS 2022** в корне проекта и выполните:

```powershell
.\build.ps1
```

Результаты будут в `dist\`, включая `dist\x86` и `dist\x64`. Для установщика:

```powershell
.\build.ps1 -BuildInstaller
```

Чтобы получить официальный исходный код Steinberg ASIO SDK под GPLv3:

```powershell
.\tools\Fetch-ASIO-SDK.ps1
```

Скрипт клонирует `https://github.com/audiosdk/asio` и проверяет наличие лицензии и `common\asio.h`. SDK подключается CMake к x64 ASIO DLL.

## Статус функций
- OpenAL Creative: экспериментальный путь, требует проверки на целевой Windows-машине.
- WASAPI Exclusive: **не реализован**.
- ASIO Steinberg: реализован экспериментальный x64 host и межпроцессный мост от перехваченных WASAPI render buffers к ASIO callback через shared-memory ring; нужен Windows build и тестирование на реальном драйвере.

Успешная компиляция не доказывает, что игровой звук реально перехватывается или выводится выбранным способом. Для этого нужны тесты на Windows 10, играх x86/x64 и реальном устройстве.

## Экспериментальный ASIO x64 backend

Для получения SDK и сборки используйте в Windows PowerShell:

```powershell
.\build.ps1 -FetchAsioSdk
```

При наличии SDK в `third_party\asio-sdk` x64-конфигурация собирает `GameAudioBridgeAsio.dll`. Движок хранит и маршрутизирует сигнал во внутреннем формате IEEE-754 float64, перечисляет ASIO-драйверы, открывает выбранный драйвер, получает имена аппаратных выходов, вызывает штатную панель управления драйвера и принимает матрицу маршрутизации source-channel → hardware-output.

**Ограничения ASIO 1.2-dev:** мост принимает поддерживаемые WASAPI render buffers в 48 кГц и преобразует их в float64; частоты, отличные от 48 кГц, не ресемплируются и остаются на исходном пути. В текущей версии не реализовано надёжное смешивание нескольких независимых игровых аудиопотоков в общий таймлайн. WASAPI Exclusive не реализован. Windows-сборка и тестирование с реальной картой в этой среде не выполнялись.


## ASIO sample-rate handling (development implementation)

- ASIO rate selector: Auto, 44.1, 48, 88.2 and 96 kHz. Auto tries supported rates in order 48, 44.1, 96, 88.2 kHz; it does not claim every driver supports every rate.
- Incoming WASAPI streams report source sample rate and bit depth. If the source rate differs from the active ASIO rate, the GUI shows a red warning and suggests a supported rate; 22.05 kHz prioritizes 44.1 kHz and then 88.2 kHz.
- Optional rate conversion: disabled means mismatched-rate streams are not redirected. Enabled means source samples are converted to the active ASIO rate in float64 before routing, using a 32-tap Blackman-windowed sinc kernel. Input bit depth is decoded to float64 and then encoded in the ASIO driver native output format; equal input/output bit depth is not required.
- Limitations: the current sinc converter works per WASAPI buffer and clamps its edge taps, so buffer-boundary artifacts are possible; it is not yet a fully stateful streaming SRC. Auto selects a supported startup rate before the game source format is known; after a mismatch, the UI suggests a better rate but does not silently restart the ASIO driver. Treat this as experimental until built and tested on Windows with the target Creative driver.
