#include "SvgIconRasterizer.h"

#include <d2d1_3.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
    struct LocalCOM
    {
        HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ~LocalCOM()
        {
            if(SUCCEEDED(result))
            {
                CoUninitialize();
            }
        }
    };

    void Check(HRESULT result)
    {
        if(FAILED(result))
        {
            throw result;
        }
    }
}

bool RasterizeEditorSvg(const std::filesystem::path& path, unsigned int maximumSize,
    ImageData& image, std::string& error)
{
    image = {};
    error.clear();

    try
    {
        if(maximumSize == 0 || maximumSize > 1024)
        {
            throw std::runtime_error("Invalid editor icon raster size.");
        }

        // Bound input before handing it to the SVG parser. Assets are trusted local artwork.
        std::ifstream file(path, std::ios::binary | std::ios::ate);

        if(!file || file.tellg() <= 0 || file.tellg() > 1024 * 1024)
        {
            throw std::runtime_error("Missing, empty, or oversized SVG.");
        }

        std::vector<char> xml(static_cast<size_t>(file.tellg()));
        file.seekg(0);

        if(!file.read(xml.data(), static_cast<std::streamsize>(xml.size())))
        {
            throw std::runtime_error("Cannot read SVG.");
        }

        LocalCOM com;

        if(FAILED(com.result) && com.result != RPC_E_CHANGED_MODE)
        {
            Check(com.result);
        }

        ComPtr<IWICImagingFactory> wic;
        ComPtr<ID2D1Factory> factory;
        ComPtr<IWICBitmap> bitmap;
        ComPtr<ID2D1RenderTarget> target;
        ComPtr<ID2D1DeviceContext5> context;
        Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
        Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()));
        Check(wic->CreateBitmap(maximumSize, maximumSize, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnLoad, &bitmap));
        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        Check(factory->CreateWicBitmapRenderTarget(bitmap.Get(), properties, &target));
        // Capability check also handles older runtimes without introducing a D3D11 device.
        Check(target.As(&context));
        ComPtr<IStream> stream;
        Check(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
        Check(stream->Write(xml.data(), static_cast<ULONG>(xml.size()), nullptr));
        LARGE_INTEGER start{};
        Check(stream->Seek(start, STREAM_SEEK_SET, nullptr));
        ComPtr<ID2D1SvgDocument> document;
        Check(context->CreateSvgDocument(stream.Get(), D2D1::SizeF(300, 150), &document));
        ComPtr<ID2D1SvgElement> root;
        document->GetRoot(&root);

        if(!root)
        {
            throw std::runtime_error("SVG has no root element.");
        }

        D2D1_SVG_VIEWBOX viewBox{};
        const bool hasViewBox = SUCCEEDED(root->GetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX,
            &viewBox, sizeof(viewBox))) &&
            viewBox.width > 0 && viewBox.height > 0;
        auto dimension = [&](const wchar_t* name)
        {
            D2D1_SVG_LENGTH length{};
            return SUCCEEDED(root->GetAttributeValue(name, &length)) &&
                length.units == D2D1_SVG_LENGTH_UNITS_NUMBER && length.value > 0 ? length.value : 0.0f;
        };
        float width = dimension(L"width");
        float height = dimension(L"height");

        if(width == 0 && height > 0 && hasViewBox)
        {
            width = height * viewBox.width / viewBox.height;
        }

        if(height == 0 && width > 0 && hasViewBox)
        {
            height = width * viewBox.height / viewBox.width;
        }

        if(width == 0)
        {
            width = hasViewBox ? viewBox.width : 300.0f;
        }

        if(height == 0)
        {
            height = hasViewBox ? viewBox.height : 150.0f;
        }

        if(!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0)
        {
            throw std::runtime_error("Invalid SVG dimensions.");
        }

        const float scale = static_cast<float>(maximumSize) / (std::max)(width, height);
        const auto pixelWidth = static_cast<UINT>((std::max)(1.0f, std::round(width * scale)));
        const auto pixelHeight = static_cast<UINT>((std::max)(1.0f, std::round(height * scale)));
        document->SetViewportSize(D2D1::SizeF(width, height));
        context->BeginDraw();
        context->Clear(D2D1::ColorF(0, 0.0f));
        context->SetTransform(D2D1::Matrix3x2F::Scale(pixelWidth / width, pixelHeight / height));
        context->DrawSvgDocument(document.Get());
        Check(context->EndDraw());

        ImageData result;
        result.width = pixelWidth;
        result.height = pixelHeight;
        result.channels = 4;
        result.pixels.resize(static_cast<size_t>(pixelWidth) * pixelHeight * 4);
        const WICRect rectangle{0, 0, static_cast<INT>(pixelWidth), static_cast<INT>(pixelHeight)};
        Check(bitmap->CopyPixels(&rectangle, pixelWidth * 4, static_cast<UINT>(result.pixels.size()), result.pixels.data()));
        // Artwork's alpha is coverage. White straight RGB permits any theme tint and avoids
        // dark fringes caused by multiplying premultiplied RGB with ImGui's straight-alpha blend.
        for(size_t i = 0; i < result.pixels.size(); i += 4)
        {
            result.pixels[i] = result.pixels[i + 1] = result.pixels[i + 2] = 255;
        }

        image = std::move(result);
        return true;
    }
    catch(HRESULT result)
    {
        error = "Direct2D SVG rasterization failed (requires SVG-capable WIC context), HRESULT " + std::to_string(result);
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
    }

    return false;
}
