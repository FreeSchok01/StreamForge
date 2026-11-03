# FFmpeg D3D11-Frame-Übergabe an NVENC

## Ziel

Der D3D11-BGRA-zu-NV12-Konverter liefert bereits native NV12-Texturen. Der nächste GPU-Pfad soll diese Texturen ohne CPU-Readback an FFmpegs NVENC-Encoder übergeben. Das ist ein eigener Pfad neben dem bestehenden CPU-YUV420P-Eingang; der Software-Encoder und die aktuelle WindowsRecordingSession bleiben zunächst unverändert.

## Verifizierter FFmpeg-Vertrag

FFmpeg NVENC unterstützt D3D11-Hardwareframes, wenn der Encoder mit `AV_PIX_FMT_D3D11` und einem passenden `hw_frames_ctx` konfiguriert wird. Der Frames-Kontext muss `format = AV_PIX_FMT_D3D11`, `sw_format = AV_PIX_FMT_NV12` sowie die tatsächliche Breite und Höhe melden. Der NVENC-Adapter liest für D3D11 die Textur aus `AVFrame.data[0]` und den Subresource-Index aus `AVFrame.data[1]`; der Frame muss außerdem sein `hw_frames_ctx` referenzieren.

Quellen:
- FFmpeg `nvenc.c`: Registrierung von D3D11-Ressourcen als DirectX-Ressourcen und Verwendung von `data[1]` als Subresource-Index.
- FFmpeg `hwcontext_d3d11va.h`: `AVD3D11FrameDescriptor` und Anforderungen an benutzerdefinierte Frame-Pools.
- FFmpeg `hwcontext.h`: `AVHWFramesContext.pool` kann vor `av_hwframe_ctx_init()` bereitgestellt werden.

## Gerätekontext

1. Einen `AVHWDeviceContext` vom Typ `AV_HWDEVICE_TYPE_D3D11VA` anlegen.
2. Den `AVD3D11VADeviceContext` mit demselben `ID3D11Device`, Immediate Context, `ID3D11VideoDevice` und `ID3D11VideoContext` befüllen, die Capture und Video-Processor verwenden.
3. Vor Übergabe der COM-Zeiger an FFmpeg jeweils eine eigene COM-Referenz halten. FFmpeg gibt die eingetragenen Interfaces beim Abbau seines Gerätekontexts frei.
4. `lock` und `unlock` mit dem gemeinsamen `std::recursive_mutex` verbinden. FFmpeg dokumentiert ausdrücklich, dass dieser Lock rekursiv sein muss.
5. Den Gerätekontext über `av_buffer_ref()` an den Frames-Kontext und den Encoder weiterreichen.

Ein mit `av_hwdevice_ctx_create()` separat erzeugtes D3D11-Gerät darf nicht verwendet werden: Die vom Konverter erzeugten Texturen gehören zum Capture-Gerät und müssen mit dem Encoder-Gerätekontext kompatibel sein.

## Frames und Textur-Pool

Der geplante Frames-Kontext verwendet:
- `format = AV_PIX_FMT_D3D11`
- `sw_format = AV_PIX_FMT_NV12`
- Capture-Auflösung als `width` und `height`
- den zuvor erstellten D3D11-Device-Context

Der bevorzugte Ansatz ist ein eigener `AVBufferPool` über `AVHWFramesContext.pool`. Jeder Pool-Buffer muss als `AVD3D11FrameDescriptor` beginnen und die Textur sowie den Array-/Subresource-Index beschreiben. Der Pool soll die NV12-Texturen tatsächlich besitzen und wiederverwenden; der Video-Processor schreibt in eine vom Pool ausgeliehene Textur. Damit werden Texturerzeugung und Lebensdauer nicht unabhängig voneinander verwaltet.

Bei einzelnen Texturen statt einer Array-Textur ist der Subresource-Index 0. Ein späterer Array-Pool kann mehrere Textur-Slices mit entsprechendem Index bereitstellen. Der konkrete Pool muss gegen die eingesetzte FFmpeg-Version getestet werden; die Header-Dokumentation beschreibt die Descriptor-Anforderung, aber nicht jede versionsspezifische Pool-Initialisierungsnuance.

## Frame-Lebensdauer

Beim Senden eines Frames an libavcodec darf die D3D11-Textur nicht freigegeben oder wiederverwendet werden, solange FFmpeg/NVENC noch darauf zugreift.

- Der `AVFrame` erhält einen `AVBufferRef`, dessen Daten den `AVD3D11FrameDescriptor` enthalten.
- Der Buffer hält die Pool-Lease fest.
- `AVFrame.hw_frames_ctx` hält den Frames-Kontext am Leben.
- Erst wenn FFmpeg alle Frame-Referenzen freigibt, kehrt der Buffer in den Pool zurück und die Textur kann erneut vom Video-Processor beschrieben werden.
- Der Pool wird erst nach dem Encoder-Flush und Freigeben aller Frames zerstört.

Ein bloßes Setzen von `frame->data[0]` und `frame->data[1]` ohne referenzzählenden `frame->buf[]`-Besitzer ist nicht zulässig.

## Synchronisation

- Video-Processor-Aufrufe und FFmpeg-D3D11VA-Aufrufe verwenden denselben rekursiven Context-Lock.
- Die Textur darf erst nach dem Video-Processor-Blt an NVENC übergeben werden.
- Die Reihenfolge der Befehle auf demselben Immediate Context plus der gemeinsame Lock serialisiert die CPU-seitige Befehlsübermittlung. Ob zusätzliche GPU-Fences für die konkrete Treiber-/Encoder-Kombination erforderlich sind, muss auf echter Hardware validiert werden.
- Die Pool-Lease verhindert Wiederverwendung, solange der Encoder den Frame referenziert; sie ersetzt keine erforderliche GPU-Synchronisation.

## Encoder-Konfiguration

Der NVENC-Zweig wird auf `AV_PIX_FMT_D3D11` umgestellt und erhält `codec_ctx->hw_frames_ctx`. Der bisherige `AV_PIX_FMT_YUV420P`-Pfad bleibt für Software-Encoding bestehen. Die Hardware-Frame-Konfiguration wird nur aktiviert, wenn der ausgewählte Encoder in der verlinkten FFmpeg-Build D3D11-Frames unterstützt. Die reine Build-Capability-Abfrage beweist weiterhin nicht, dass GPU, Treiber und Codec zur Laufzeit funktionieren.

## Umsetzungsschritte

1. D3D11VA-Gerätewrapper mit COM-Referenzierung und rekursiven Lock-Callbacks.
2. Wiederverwendbarer NV12-Texturpool, der vom Video-Processor und FFmpeg-Frame-Buffer gemeinsam verwendet wird.
3. `AVFrame`-Wrapper mit `AVD3D11FrameDescriptor`, `buf[0]` und `hw_frames_ctx`.
4. Separater NVENC-Hardwareframe-Eingabepfad; CPU-Pfad nicht regressieren.
5. Windows-CI-Build und Unit-Tests für Ownership/Pool-Recycling.
6. Hardwaretests auf NVIDIA-Systemen: H.264, HEVC und AV1 soweit verfügbar, 1080p60, Flush/Stop, längere Laufzeit und GPU-Auslastung.

## Noch nicht umgesetzt / nicht behauptet

Diese Dokumentation beschreibt den Integrationsvertrag und die geplante Implementierung. Der D3D11-Konverter besitzt inzwischen einen eigenen Lease-basierten Free-List-Pool und kann freigegebene NV12-Texturen wiederverwenden. Dieser Pool ist noch nicht an `AVHWFramesContext.pool` gekoppelt; die gemeinsame FFmpeg-Pool-Integration, der FFmpeg-D3D11VA-Gerätewrapper und der direkte NVENC-Hardwareframe-Pfad sind noch nicht implementiert. Es gibt noch keine bestätigte NVIDIA-Hardwarevalidierung.
