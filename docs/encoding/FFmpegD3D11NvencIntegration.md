# FFmpeg D3D11VA-Gerätekontext

## Implementierter Wrapper

`FFmpegD3D11DeviceContext` kann einen bereits existierenden Capture-Gerätekontext
als `AV_HWDEVICE_TYPE_D3D11VA` initialisieren. Er erzeugt ausdrücklich kein
zweites D3D11-Gerät. Vor dem Initialisieren prüft er:

- Device und Immediate Context gehören zum selben D3D11-Gerät.
- Der übergebene Context ist der Immediate Context.
- `ID3D11VideoDevice` und `ID3D11VideoContext` gehören zu genau diesem Gerät
  beziehungsweise Context.
- Ein gemeinsamer rekursiver Mutex ist vorhanden.

Die COM-Interfaces erhalten jeweils eine zusätzliche Referenz, bevor sie in
`AVD3D11VADeviceContext` eingetragen werden. FFmpeg gibt diese Referenzen beim
Abbau des Gerätekontexts frei. Ein `AVHWDeviceContext.free`-Callback hält den
Mutex-Eigentümer bis zum Ende der FFmpeg-Gerätelebensdauer am Leben. FFmpeg
verwendet dieselben rekursiven Lock-Callbacks wie Capture und Video-Processor.

`reference()` liefert eine geliehene `AVBufferRef*-Referenz`; jeder weitere
Eigentümer muss mit `av_buffer_ref()` eine eigene Referenz erstellen. Der
Wrapper darf erst heruntergefahren werden, wenn seine eigenen Nutzer ihre
Referenzen übernommen haben.

## Noch nicht umgesetzt

Dieser Wrapper stellt nur den gemeinsamen D3D11VA-Gerätekontext bereit. Er
erstellt noch keinen `AVHWFramesContext`, keinen benutzerdefinierten
`AVBufferPool`, keine D3D11-Frame-Buffer und übergibt noch keine Textur an
NVENC. Die vorhandene NV12-Textur-Lease wird dadurch noch nicht mit FFmpegs
Frame-Referenzzählung verbunden.

Für einen benutzerdefinierten `AVHWFramesContext.pool` verlangt FFmpeg
`AVBufferRef`-Daten, die mit einem `AVD3D11FrameDescriptor` beginnen. Der
Datenpuffer muss die Texturreferenz bis zur Freigabe durch FFmpeg halten.
FFmpeg legt unabhängig davon interne Pool-Metadaten an; deshalb müssen
`initial_pool_size`, der benutzerdefinierte Pool und dessen Freigabe in der
konkreten verwendeten FFmpeg-Version gemeinsam getestet werden.

Quellen:
- FFmpeg `hwcontext_d3d11va.h`: Device-Interface-Ownership, rekursiver Lock
  und Descriptor-Vertrag für benutzerdefinierte Pools.
- FFmpeg `hwcontext_d3d11va.c`: Device-Uninitialisierung, Frames-Initialisierung,
  interner Pool und `data[0]`/`data[1]`-Zuordnung.
- FFmpeg `hwcontext.h`: Lebensdauer und Pool-Vertrag von
  `AVHWDeviceContext` und `AVHWFramesContext`.

Der Wrapper ist noch nicht als vollständiger GPU-Encoding-Pfad zu betrachten.
NVIDIA-Hardwaretests bleiben erforderlich.
