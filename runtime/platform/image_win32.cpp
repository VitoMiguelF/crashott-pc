#include "image.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace plat
{
    bool DecodePng(const uint8_t* data, size_t size, int& width, int& height, std::vector<uint8_t>& rgba)
    {
        HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool ok = false;
        {
            ComPtr<IWICImagingFactory> factory;
            ComPtr<IWICStream> stream;
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> converter;
            UINT w = 0, h = 0;
            ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
                 SUCCEEDED(factory->CreateStream(&stream)) &&
                 SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(data), DWORD(size))) &&
                 SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
                 SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
                 SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                                 WICBitmapPaletteTypeCustom)) &&
                 SUCCEEDED(converter->GetSize(&w, &h)) && w && h;
            if (ok)
            {
                width = int(w), height = int(h);
                rgba.resize(size_t(w) * h * 4);
                ok = SUCCEEDED(converter->CopyPixels(nullptr, w * 4, UINT(rgba.size()), rgba.data()));
            }
        }
        if (com == S_OK || com == S_FALSE)
            CoUninitialize();
        return ok;
    }
}
