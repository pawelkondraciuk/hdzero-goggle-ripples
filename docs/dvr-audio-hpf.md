# DVR High-Pass (P0), BoxPro

Gałąź `feat/dvr-audio-dsp` pochodzi z `main` (4fbb865), zgodnie z decyzją
użytkownika: zawiera poprawki DVR dodane po `ripples-v1.2.2`. Nie nadpisuje tagu
ani oficjalnego firmware. P1 / Voice Clarity nie jest częścią tej implementacji.

## Użycie

W Audio wybierz `DVR High-Pass`: Off → 80 Hz → 120 Hz → 160 Hz → 200 Hz.
Kliknięcie przechodzi do następnej wartości. Domyślnie Off. Zacznij odsłuchowe
porównanie od 120 Hz. Wybór zapisuje się automatycznie w istniejącym pliku
ustawień jako `[record] audio_hpf_hz`. Starszy plik lub nieprawidłowa wartość
oznacza Off. Zmiana podczas nagrania dotyczy następnego klipu: DVR przekazuje
wartość z zaakceptowanego snapshotu do prywatnego `record.conf`.

## Tor audio i decyzje

W bazowym kodzie `ai2aenc_prepare` wiąże kanał AI z AENC przez
`AW_MPI_SYS_Bind`. AAC trafia z osobnego wątku odbiorczego do istniejącego muxera.
Live audio oraz odtwarzanie plików mają osobne ścieżki. Menu Run Test na BoxPro
używa `arecord`; nie testuje tego filtra ani ścieżki AAC/DVR.

Off zachowuje binding AI → AENC. Włączenie HPF zastępuje wyłącznie to połączenie
wątkiem GetFrame → kopia PCM → DSP → SendFrame → ReleaseFrame. DSP to biquad
Butterwortha drugiego rzędu, Q=1/√2, współczynniki wyliczane raz przy konfiguracji.
Próbki są S16 interleaved, osobny stan dla każdego kanału, saturacja do int16.
Stan jest zerowany dla nowego nagrania. Brak AGC, bramki i odszumiania.

Przeanalizowano nagłówki SDK i kod maszynowy bibliotek dołączonych do repo:

- `libmedia_mpp.so:AudioEncEmptyThisBuffer` (0x22558) przekazuje `mpAddr` i
  `mLen` bez mnożenia przez liczbę kanałów do `WriteAudioStreamBuffer`.
- `libcedarx_aencoder.so:RequestWriteBuf` (0x3688) sprawdza miejsce i kopiuje
  dane przez memcpy do własnego bufora kołowego przed powrotem. Pozwala to
  ponownie użyć scratch oraz zwolnić ramkę AI po SendFrame. Dotyczy dostarczonych
  bibliotek; po zmianie SDK wymaga ponownej weryfikacji.
- `AIChannel_EmptyThisBuffer` również kopiuje dokładnie `mLen` bajtów;
  komentarz nagłówka „per channel” nie opisuje zachowania tych binariów.
- Enkoder inicjalizuje czas z `tmp_pts`, nie tylko `mTimeStamp`. Cała struktura
  ramki jest kopiowana, z zachowaniem obu pól, sekwencji i identyfikatora.
- `AW_MPI_AI_GetFrame` ma ścieżkę bez bindingu i ograniczony czas oczekiwania.
  Podobne API jest używane w `src/player/wav_test.c`, lecz nie stanowi to
  walidacji BoxPro.
- Wbudowany interfejs VQE wymaga odróżnienia od działającego algorytmu;
  poniżej wynik weryfikacji dołączonej biblioteki.

Bufor 32 KiB należy do kontekstu audio, bez alokacji na ramkę. Pump sprawdza
rozmiar, bit depth i mono/stereo. Nieznany układ przechodzi bez filtracji,
zeruje historię DSP i zwiększa licznik passthrough. SendFrame jest ponawiany
maksymalnie trzy razy (przerwy 5 ms); po wyczerpaniu prób ramka jest zwalniana,
a strata raportowana. SDK zwraca również ogólne FAILURE dla pełnego bufora.
Oczekiwanie GetFrame wynosi 200 ms; sygnał końca jest atomowy, wątki są łączone
przed zniszczeniem kanałów. Błąd utworzenia wątku sprząta rozpoczęty tor audio.
Błąd ReleaseFrame kończy pump i jest logowany; wymaga sprawdzenia na urządzeniu.

## Weryfikacja wbudowanego HPF / VQE

Sprawdzono konkretną `lib/softwinner/lib/libmedia_mpp.so`, SHA-256:
`410e3aa3197a3127a65e66d2142beddcf8c7447a4522f476bfed835f08025936`.
Analiza statyczna ARM (Capstone + tablica symboli ELF) pokazuje:

- `AW_MPI_AI_SetVqeAttr` przekazuje indeks `0x7f002081` do
  `AIChannel_SetConfig`. Jego obsługa (0x1eb98–0x1ebc8, 0x1eca0) kopiuje
  0x140 bajtów konfiguracji do kontekstu AI pod offsetem 0x1f8.
- `AW_MPI_AI_EnableVqe` przekazuje `0x7f002082`. Obsługa
  0x1edd8–0x1ede0 tylko ustawia flagę pod offsetem 0x1f4 na 1.
  Disable analogicznie zeruje tę flagę.
- `AIChannel_GetConfig` odczytuje konfigurację do zwrócenia przez API.
  W `AIChannel_EmptyThisBuffer`, `AIChannel_ComponentThread` oraz
  `audioHw_AI_CapThread` nie ma użycia tej konfiguracji/flagi do filtrowania
  próbek. Ścieżka AI kopiuje PCM i przekazuje go dalej.

To znacznie mocniejsza przesłanka niż sam brak nazw symboli HPF: wskazuje, że
VQE wejścia w tej bibliotece jest interfejsem zapisującym ustawienia, bez
aktywnego przetwarzania. Sam SUCCESS z Set/Enable ani poprawny Get nie dowodzi
filtracji. Jest to wynik analizy binarnej, nie pomiar na BoxPro.

Nagłówek `mm_comm_aio.h` opisuje dwa różne interfejsy: samodzielny HPF
80/120/150 Hz (`AUDIO_HPF_FREQ_E`) oraz HPF w konfiguracji AGC
80/120/150/300/500 Hz (`s8UseHighPassFilt`). Wcześniejsze określenie „tylko
80/120/150 Hz” dotyczyło pierwszego z nich. Żaden nie deklaruje 160/200 Hz ani
charakterystyki Butterwortha drugiego rzędu. Komentarz `s32WorkSampleRate`
w konfiguracji VQE wymienia 8/16 kHz; nie potwierdza obsługi DVR 48 kHz.
Nie uruchamiamy AGC, aby uzyskać HPF.

Własny DSP pozostaje uzasadniony dla dostarczonych bibliotek: ma określoną
charakterystykę i testy przy 48 kHz. Jeżeli zostanie dostarczona inna biblioteka
VQE, trzeba ponowić analizę i pomiar Off/On na prawdziwym DVR. Działający HPF
w istniejącym bindingu byłby wart rozważenia, ponieważ pozwoliłby uniknąć pump.

## Walidacja bez urządzenia

```
make -C test/audio_dsp run asan pump
make -C test/record run run_record_control
```

43 asercje DSP: Off bit-identical, wszystkie częstotliwości odcięcia,
40/120/1000 Hz, impuls, cisza, saturacja bez przepełnień, niezależne stereo,
reset, błędne parametry i godzina symulowanego PCM. Dla HPF 120 Hz / 48 kHz:
40 Hz −19,14 dB, 120 Hz −3,01 dB, 1 kHz −0,001 dB. ASan/UBSan sprawdzają
pamięć i niezdefiniowane operacje. Godzina PCM nie jest godziną testu sprzętu.

Test pump używa rzeczywistych deklaracji SDK i mocków MPI: prywatna kopia,
mono/stereo, zachowanie timestampów, retry po FAILURE, timeout, odrzucenie po
wyczerpaniu prób i zabezpieczenia formatu. Nie dowodzi działania binariów MPP.
Regresje DVR obejmują przekazanie HPF ze snapshotu oraz Off.

## Build BoxPro

Workflow `.github/workflows/build_and_release_boxpro.yml` uruchamia testy hosta,
potem istniejące `bash setup.sh automated_build` i `make -C build_boxpro`.
Target CMake to `-DHDZ_BOXPRO=ON` (definicja kodu `HDZBOXPRO`). Nowy plik DSP
wchodzi przez istniejący GLOB `src/record/*.c`, libm jest już linkowana.
W zakładce Actions pobierz artefakt pakietu firmware z udanego builda swojej
gałęzi, nie pakiet dla innego modelu. Sam build nie potwierdza działania DSP
na goglach. Lokalny pełny build używa tych samych dwóch komend i pobiera
Bootlin ARM toolchain; setup usuwa wcześniejsze katalogi `build_*`.

## Testy wymagające BoxPro

1. Kopia ustawień i znany działający pakiet firmware dla BoxPro przed aktualizacją.
2. Off: nagrywanie MP4/TS, audio on/off, Mic/Line In/A/V In, odsłuch live i playback.
3. HPF dla każdego ustawienia i źródła: log pierwszej ramki (len, bitwidth,
   soundmode, timestamp), brak passthrough/dropped, poprawna długość klipu.
4. Nagranie tonu 40/120/1000 Hz w prawdziwym DVR, porównanie widma Off/120 Hz;
   Run Test nie wystarcza. Sprawdzić rzeczywisty format i liczbę kanałów.
5. Kilka start/stop, szybkie restarty, zmiana źródła między klipami, zmiana HPF
   w trakcie nagrania (dopiero kolejny klip), restart gogli i trwałość ustawień.
6. Minimum 30–60 min: klaps na początku i końcu do pomiaru A/V drift, brak przerw,
   obserwacja CPU, temperatury, pamięci i liczników strat. Porównać z Off.
7. Głośna mowa i drony: brak trzasków; ocenić naturalność otoczenia w hali i na
   zewnątrz z Saramonic SR-LMX1+, Line/AV Gain 3. HPF nie naprawi przesterowania
   analogowego ani tłumienia góry przez umiejscowienie mikrofonu za uchem.
8. Sprawdzić czytelność i nawigację menu 600 px, w tym wszystkie testy i Back.

Nie wykonano testów na fizycznym urządzeniu. Narzut CPU, stabilność MPP i brak
A/V drift pozostają do zmierzenia; nie są gwarantowane przez testy hosta.

## Cofnięcie

Wybierz Off i rozpocznij nowy klip, aby wrócić do bindingu. Dla pełnego cofnięcia
wgraj zachowany oryginalny pakiet BoxPro standardową procedurą aktualizacji
producenta. Nowy klucz jest ignorowany przez starszy firmware; kopia ustawień
pozwala odtworzyć konfigurację. Nie zmieniono bootloadera ani procedury update.

## Zmienione pliki

- `src/record/audio_dsp.c`, `.h`: czysty DSP bez zależności od MPP.
- `src/record/ai2aenc.c`, `.h`: pump PCM, bufor, reset, liczniki i lifecycle.
- `src/record/confparser.c`, `record.conf`: odczyt HPF z walidacją, domyślne Off.
- `src/record/main.c`: HPF w logu parametrów nagrania.
- `src/core/settings.c`, `.h`: trwałe ustawienie i walidacja starszych plików.
- `src/core/dvr.c`: przekazanie HPF ze snapshotu do konfiguracji rejestratora.
- `src/ui/page_audio.c`, `conf/ui.h`: wybór HPF i dodatkowy wiersz menu.
- `test/audio_dsp/*`, `.gitignore`: testy DSP i pump, Makefile, ignorowane pliki wykonywalne.
- `test/record/test_dvr_naming.c`: snapshot HPF i poprawka nagłówka testu Linux.
- `.github/workflows/build_and_release_boxpro.yml`: testy hosta przed buildem.
- `docs/dvr-audio-hpf.md`: architektura, użycie, build, testy sprzętowe i rollback.
