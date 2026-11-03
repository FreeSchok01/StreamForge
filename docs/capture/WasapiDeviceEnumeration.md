# WASAPI audio device enumeration

`WasapiDeviceEnumerator` lists active Windows audio endpoints for both
`AudioDeviceFlow::Capture` (microphones/line inputs) and
`AudioDeviceFlow::Render` (speakers/headphones, used for loopback capture).

Each result contains the endpoint's stable Windows device ID, friendly name,
flow and whether it is the current default endpoint.

Pass the selected ID to `AudioCaptureConfig::deviceId`. Leave it empty to use
the Windows default endpoint. For desktop/system audio, select a Render
endpoint and set `AudioCaptureConfig::loopback = true`.

The enumerator currently returns active endpoints only. Device-change
notifications and automatic recovery after unplug/reconnect are not implemented.
