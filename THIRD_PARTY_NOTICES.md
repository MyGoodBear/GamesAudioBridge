# Third-party components and licensing

## Steinberg ASIO SDK

Official source repository: https://github.com/audiosdk/asio
Copyright: Steinberg Media Technologies GmbH and contributors. The SDK repository states that the SDK may be used under either the Steinberg ASIO License or GNU GPL version 3. This project selects GPLv3. Keep the SDK's own LICENSE.txt and per-file notices intact if fetched or redistributed.

The current Game Audio Bridge source does **not yet compile or call an ASIO host**. The fetch script prepares SDK source for future integration; fetching it alone does not add ASIO playback.

## Windows APIs
WASAPI/Core Audio is used through the Windows SDK; no third-party WASAPI library is required.

## OpenAL
The prototype dynamically loads the system OpenAL32.dll and does not bundle an OpenAL SDK/runtime. Creative OpenAL must be available on the target PC for the relevant process bitness.
