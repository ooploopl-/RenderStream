// A simple RenderStream application that sends back a 3D scene using a DX11 texture
//
// Usage: Compile, copy the executable into your RenderStream Projects folder and launch via d3

#define WIN32_LEAN_AND_MEAN

#include <iostream>
#include <vector>
#include <windows.h>
#include <shlwapi.h>
#include <tchar.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl.h>
#include <unordered_map>

// auto-generated from hlsl
#include "Generated_Code/VertexShader.h"
#include "Generated_Code/PixelShader.h"

#include "../../include/d3renderstream.h"

#if defined(UNICODE) || defined(_UNICODE)
#define tcout std::wcout
#define tcerr std::wcerr
#else
#define tcout std::cout
#define tcerr std::cerr
#endif

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

// Load renderstream DLL from disguise software's install path
HMODULE loadRenderStream()
{
    HKEY hKey;
    if (FAILED(RegOpenKeyEx(HKEY_CURRENT_USER, TEXT("Software\\d3 Technologies\\d3 Production Suite"), 0, KEY_READ, &hKey)))
    {
        tcerr << "Failed to open 'Software\\d3 Technologies\\d3 Production Suite' registry key" << std::endl;
        return nullptr;
    }

    TCHAR buffer[512];
    DWORD bufferSize = sizeof(buffer);
    if (FAILED(RegQueryValueEx(hKey, TEXT("exe path"), 0, nullptr, reinterpret_cast<LPBYTE>(buffer), &bufferSize)))
    {
        tcerr << "Failed to query value of 'exe path'" << std::endl;
        return nullptr;
    }

    if (!PathRemoveFileSpec(buffer))
    {
        tcerr << "Failed to remove file spec from path: " << buffer << std::endl;
        return nullptr;
    }

    if (_tcscat_s(buffer, bufferSize, TEXT("\\d3renderstream.dll")) != 0)
    {
        tcerr << "Failed to append filename to path: " << buffer << std::endl;
        return nullptr;
    }

    HMODULE hLib = ::LoadLibraryEx(buffer, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!hLib)
    {
        tcerr << "Failed to load dll: " << buffer << std::endl;
        return nullptr;
    }
    return hLib;
}

// Get streams into (descMem) buffer and return a pointer into it
const StreamDescriptions* getStreams(decltype(rs_getStreams)* rs_getStreams, std::vector<uint8_t>& descMem)
{
    uint32_t nBytes = 0;
    rs_getStreams(nullptr, &nBytes);

    const static int MAX_TRIES = 3;
    int iterations = 0;

    RS_ERROR res = RS_ERROR_BUFFER_OVERFLOW;
    do
    {
        descMem.resize(nBytes);
        res = rs_getStreams(reinterpret_cast<StreamDescriptions*>(descMem.data()), &nBytes);

        if (res == RS_ERROR_SUCCESS)
            break;

        ++iterations;
    } while (res == RS_ERROR_BUFFER_OVERFLOW && iterations < MAX_TRIES);

    if (res != RS_ERROR_SUCCESS)
        throw std::runtime_error("Failed to get streams");

    if (nBytes < sizeof(StreamDescriptions))
        throw std::runtime_error("Invalid stream descriptions");

    return reinterpret_cast<const StreamDescriptions*>(descMem.data());
}

static constexpr DirectX::XMFLOAT3 cubeVertices[] =
{
    DirectX::XMFLOAT3(-0.5f,-0.5f,-0.5f),
    DirectX::XMFLOAT3( 0.5f,-0.5f,-0.5f),
    DirectX::XMFLOAT3( 0.5f,-0.5f, 0.5f),
    DirectX::XMFLOAT3(-0.5f,-0.5f, 0.5f),

    DirectX::XMFLOAT3(-0.5f, 0.5f,-0.5f),
    DirectX::XMFLOAT3( 0.5f, 0.5f,-0.5f),
    DirectX::XMFLOAT3( 0.5f, 0.5f, 0.5f),
    DirectX::XMFLOAT3(-0.5f, 0.5f, 0.5f),
};

static constexpr uint16_t cubeIndices[] =
{
    0, 1, 2, 3, 0, 4, 5, 6, 7, 4, 
    1, 5, 
    2, 6, 
    3, 7
};

static constexpr UINT cubeDrawCalls[] =
{
    10,
    2,
    2,
    2
};



struct ConstantBufferStruct 
{
    DirectX::XMMATRIX worldViewProjection;
};

int main()
{
    HMODULE hLib = loadRenderStream();
    if (!hLib)
    {
        tcerr << "Failed to load RenderStream DLL" << std::endl;
        return 1;
    }

#define LOAD_FN(FUNC_NAME) \
    decltype(FUNC_NAME)* FUNC_NAME = reinterpret_cast<decltype(FUNC_NAME)>(GetProcAddress(hLib, #FUNC_NAME)); \
    if (!FUNC_NAME) { \
        tcerr << "Failed to get function " #FUNC_NAME " from DLL" << std::endl; \
        return 2; \
    }

    LOAD_FN(rs_initialise);
    LOAD_FN(rs_getStreams);
    LOAD_FN(rs_awaitFrameData);
    LOAD_FN(rs_getFrameCamera);
    LOAD_FN(rs_sendFrame);
    LOAD_FN(rs_shutdown);

    if (rs_initialise(RENDER_STREAM_VERSION_MAJOR, RENDER_STREAM_VERSION_MINOR) != RS_ERROR_SUCCESS)
    {
        tcerr << "Failed to initialise RenderStream" << std::endl;
        return 3;
    }

#ifdef _DEBUG
    const uint32_t deviceFlags = D3D11_CREATE_DEVICE_DEBUG;
#else
    const uint32_t deviceFlags = 0;
#endif
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags, nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf())))
    {
        tcerr << "Failed to initialise DirectX 11" << std::endl;
        rs_shutdown();
        return 4;
    }

    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
    {
        CD3D11_BUFFER_DESC vertexDesc(sizeof(cubeVertices), D3D11_BIND_VERTEX_BUFFER);
        D3D11_SUBRESOURCE_DATA vertexData;
        ZeroMemory(&vertexData, sizeof(D3D11_SUBRESOURCE_DATA));
        vertexData.pSysMem = cubeVertices;
        vertexData.SysMemPitch = 0;
        vertexData.SysMemSlicePitch = 0;
        if (FAILED(device->CreateBuffer(&vertexDesc, &vertexData, vertexBuffer.GetAddressOf())))
        {
            tcerr << "Failed to initialise DirectX 11: vertex buffer" << std::endl;
            rs_shutdown();
            return 41;
        }
    }
    Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
    {
        CD3D11_BUFFER_DESC indexDesc(sizeof(cubeIndices), D3D11_BIND_INDEX_BUFFER);
        D3D11_SUBRESOURCE_DATA indexData;
        ZeroMemory(&indexData, sizeof(D3D11_SUBRESOURCE_DATA));
        indexData.pSysMem = cubeIndices;
        indexData.SysMemPitch = 0;
        indexData.SysMemSlicePitch = 0;
        if (FAILED(device->CreateBuffer(&indexDesc, &indexData, indexBuffer.GetAddressOf())))
        {
            tcerr << "Failed to initialise DirectX 11: index buffer" << std::endl;
            rs_shutdown();
            return 42;
        }
    }

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
    {
        if (FAILED(device->CreateVertexShader(VertexShaderBlob, std::size(VertexShaderBlob), nullptr, vertexShader.GetAddressOf())))
        {
            tcerr << "Failed to initialise DirectX 11: vertex shader" << std::endl;
            rs_shutdown();
            return 43;
        }
    }
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
    {
        D3D11_INPUT_ELEMENT_DESC inputElementDesc[] = { { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 } };

        if (FAILED(device->CreateInputLayout(inputElementDesc, ARRAYSIZE(inputElementDesc), VertexShaderBlob, std::size(VertexShaderBlob), inputLayout.GetAddressOf())))
        {
            tcerr << "Failed to initialise DirectX 11: index buffer" << std::endl;
            rs_shutdown();
            return 44;
        }
    }
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
    {
        if (FAILED(device->CreatePixelShader(PixelShaderBlob, std::size(PixelShaderBlob), nullptr, pixelShader.GetAddressOf())))
        {
            tcerr << "Failed to initialise DirectX 11: pixel shader" << std::endl;
            rs_shutdown();
            return 45;
        }
    }
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer;
    {
        CD3D11_BUFFER_DESC constantBufferDesc(sizeof(ConstantBufferStruct), D3D11_BIND_CONSTANT_BUFFER);
        if (FAILED(device->CreateBuffer(&constantBufferDesc, nullptr, constantBuffer.GetAddressOf())))
        {
            tcerr << "Failed to initialise DirectX 11: constant buffer" << std::endl;
            rs_shutdown();
            return 46;
        }
    }

    std::vector<uint8_t> descMem;
    const StreamDescriptions* header = nullptr;
    struct RenderTarget
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view;
    };
    std::unordered_map<StreamHandle, RenderTarget> renderTargets;
    FrameData frameData;
    while (true)
    {
        // Wait for a frame request
        RS_ERROR err = rs_awaitFrameData(5000, &frameData);
        if (err == RS_ERROR_STREAMS_CHANGED)
        {
            try
            {
                header = getStreams(rs_getStreams, descMem);
                // Create render targets for all streams
                const size_t numStreams = header ? header->nStreams : 0;
                for (size_t i = 0; i < numStreams; ++i)
                {
                    const StreamDescription& description = header->streams[i];
                    RenderTarget& target = renderTargets[description.handle];

                    D3D11_TEXTURE2D_DESC rtDesc;
                    ZeroMemory(&rtDesc, sizeof(D3D11_TEXTURE2D_DESC));
                    rtDesc.Width = description.width;
                    rtDesc.Height = description.height;
                    rtDesc.MipLevels = 1;
                    rtDesc.ArraySize = 1;
                    rtDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    rtDesc.SampleDesc.Count = 1;
                    rtDesc.Usage = D3D11_USAGE_DEFAULT;
                    rtDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
                    rtDesc.CPUAccessFlags = 0;
                    rtDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
                    if (FAILED(device->CreateTexture2D(&rtDesc, nullptr, target.texture.GetAddressOf())))
                        throw std::runtime_error("Failed to create render target texture for stream");

                    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
                    ZeroMemory(&rtvDesc, sizeof(D3D11_RENDER_TARGET_VIEW_DESC));
                    rtvDesc.Format = rtDesc.Format;
                    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                    if (FAILED(device->CreateRenderTargetView(target.texture.Get(), &rtvDesc, target.view.GetAddressOf())))
                        throw std::runtime_error("Failed to create render target view for stream");
                }
            }
            catch (const std::exception& e)
            {
                tcerr << e.what() << std::endl;
                rs_shutdown();
                return 6;
            }
            tcout << "Found " << (header ? header->nStreams : 0) << " streams" << std::endl;
            continue;
        }
        else if (err == RS_ERROR_TIMEOUT)
        {
            continue;
        }
        else if (err != RS_ERROR_SUCCESS)
        {
            tcerr << "rs_awaitFrameData returned " << err << std::endl;
            break;
        }

        // Respond to frame request
        const size_t numStreams = header ? header->nStreams : 0;
        for (size_t i = 0; i < numStreams; ++i)
        {
            const StreamDescription& description = header->streams[i];

            CameraResponseData response;
            response.tTracked = frameData.tTracked;
            if (rs_getFrameCamera(description.handle, &response.camera) == RS_ERROR_SUCCESS)
            {
                const RenderTarget& target = renderTargets.at(description.handle);
                context->OMSetRenderTargets(1, target.view.GetAddressOf(), nullptr);

                const float clearColour[4] = { 0.f, 0.f, 0.f, 0.f };
                context->ClearRenderTargetView(target.view.Get(), clearColour);

                D3D11_VIEWPORT viewport;
                ZeroMemory(&viewport, sizeof(D3D11_VIEWPORT));
                viewport.Width = static_cast<float>(description.width);
                viewport.Height = static_cast<float>(description.height);
                viewport.MinDepth = 0;
                viewport.MaxDepth = 1;
                context->RSSetViewports(1, &viewport);

                ConstantBufferStruct constantBufferData;
                const DirectX::XMMATRIX world = DirectX::XMMatrixIdentity();

                const float pitch = -DirectX::XMConvertToRadians(response.camera.rx);
                const float yaw = DirectX::XMConvertToRadians(response.camera.ry);
                const float roll = -DirectX::XMConvertToRadians(response.camera.rz);

                const DirectX::XMMATRIX cameraTranslation = DirectX::XMMatrixTranslation(response.camera.x, response.camera.y, response.camera.z);
                const DirectX::XMMATRIX cameraRotation = DirectX::XMMatrixRotationRollPitchYaw(pitch, yaw, roll);
                const DirectX::XMMATRIX view = DirectX::XMMatrixInverse(nullptr, cameraTranslation) * DirectX::XMMatrixTranspose(cameraRotation);

                const float throwRatioH = response.camera.focalLength / response.camera.sensorX;
                const float throwRatioV = response.camera.focalLength / response.camera.sensorY;
                const float fovH = 2.0f * atan(0.5f / throwRatioH);
                const float fovV = 2.0f * atan(0.5f / throwRatioV);

                const bool orthographic = response.camera.orthoWidth > 0.0f;
                const float cameraAspect = response.camera.sensorX / response.camera.sensorY;
                float imageHeight, imageWidth;
                if (orthographic)
                {
                    imageHeight = response.camera.orthoWidth / cameraAspect;
                    imageWidth = cameraAspect * imageHeight;
                }
                else
                {
                    imageWidth = 2.0f * tan(0.5f * fovH);
                    imageHeight = 2.0f * tan(0.5f * fovV);
                }

                const DirectX::XMMATRIX overscan = DirectX::XMMatrixTranslation(response.camera.cx, response.camera.cy, 0.f);

                const float nearZ = response.camera.nearZ;
                const float farZ = response.camera.farZ;

                const float l = (-0.5f + description.clipping.left) * imageWidth;
                const float r = (-0.5f + description.clipping.right) * imageWidth;
                const float t = (-0.5f + 1.f - description.clipping.top) * imageHeight;
                const float b = (-0.5f + 1.f - description.clipping.bottom) * imageHeight;

                const DirectX::XMMATRIX projection = orthographic ? DirectX::XMMatrixOrthographicOffCenterLH(l, r, b, t, nearZ, farZ) : DirectX::XMMatrixPerspectiveOffCenterLH(l * nearZ, r * nearZ, b * nearZ, t * nearZ, nearZ, farZ);

                constantBufferData.worldViewProjection = DirectX::XMMatrixTranspose(world * view * projection * overscan);
                context->UpdateSubresource(constantBuffer.Get(), 0, nullptr, &constantBufferData, 0, 0);

                // Draw cube
                UINT stride = sizeof(DirectX::XMFLOAT3);
                UINT offset = 0;
                context->IASetVertexBuffers(0, 1, vertexBuffer.GetAddressOf(), &stride, &offset);
                context->IASetIndexBuffer(indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
                context->IASetInputLayout(inputLayout.Get());
                context->VSSetShader(vertexShader.Get(), nullptr, 0);
                context->VSSetConstantBuffers(0, 1, constantBuffer.GetAddressOf());
                context->PSSetShader(pixelShader.Get(), nullptr, 0);
                UINT startIndex = 0;
                for (UINT indexCount : cubeDrawCalls)
                {
                    context->DrawIndexed(indexCount, startIndex, 0);
                    startIndex += indexCount;
                }

                SenderFrameTypeData data;
                data.dx11.resource = target.texture.Get();
                if (rs_sendFrame(description.handle, RS_FRAMETYPE_DX11_TEXTURE, data, &response) != RS_ERROR_SUCCESS)
                {
                    tcerr << "Failed to send frame" << std::endl;
                    rs_shutdown();
                    return 7;
                }
            }
        }
    }

    if (rs_shutdown() != RS_ERROR_SUCCESS)
    {
        tcerr << "Failed to shutdown RenderStream" << std::endl;
        return 99;
    }

    return 0;
}
