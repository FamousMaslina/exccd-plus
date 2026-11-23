// CHDK USB commands through the Windows Portable Devices MTP driver.
#include <windows.h>
#include <initguid.h>
#include <portabledeviceapi.h>
#include <portabledevice.h>
#include <wpdmtpextensions.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

#include "../core/ptp.h"

template<typename T> struct ReleaseCom {
    void operator()(T *value) const { value->Release(); }
};

template<typename T> using ComPtr = std::unique_ptr<T, ReleaseCom<T>>;

struct FreeComMemory {
    void operator()(void *value) const { CoTaskMemFree(value); }
};

static void check(HRESULT status, const char *operation)
{
    if (FAILED(status)) {
        std::ostringstream error;
        error << operation << " failed (0x" << std::hex << static_cast<unsigned long>(status) << ")";
        throw std::runtime_error(error.str());
    }
}

class ComApartment {
public:
    ComApartment() { check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize COM"); }
    ~ComApartment() { CoUninitialize(); }
};

template<typename T> static ComPtr<T> make_com(REFCLSID clsid, REFIID iid)
{
    T *value = nullptr;
    check(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, iid,
                           reinterpret_cast<void **>(&value)), "Create WPD object");
    return ComPtr<T>(value);
}

struct Reply {
    std::vector<DWORD> params;
    std::vector<BYTE> data;
};

class Camera {
public:
    Camera()
    {
        auto manager = make_com<IPortableDeviceManager>(CLSID_PortableDeviceManager, IID_IPortableDeviceManager);
        DWORD count = 0;
        check(manager->GetDevices(nullptr, &count), "Count portable devices");
        std::vector<PWSTR> ids(count);
        if (count != 0)
            check(manager->GetDevices(ids.data(), &count), "List portable devices");
        std::wstring camera_id;
        unsigned matches = 0;
        for (PWSTR id : ids) {
            std::unique_ptr<wchar_t, FreeComMemory> owned_id(id);
            if (!id)
                continue;
            std::wstring candidate(id);
            std::transform(candidate.begin(), candidate.end(), candidate.begin(), std::towupper);
            if (candidate.find(L"VID_04A9&PID_30FC") != std::wstring::npos) {
                camera_id = id;
                ++matches;
            }
        }
        if (matches != 1)
            throw std::runtime_error("Expected one Canon PowerShot A620 (USB 04A9:30FC) on the Windows MTP driver");
        device = make_com<IPortableDevice>(CLSID_PortableDeviceFTM, IID_IPortableDevice);
        auto client = values();
        check(client->SetStringValue(WPD_CLIENT_NAME, L"EXCCD+ CHDK USB"), "Set WPD client name");
        check(device->Open(camera_id.c_str(), client.get()), "Open A620");
    }

    ~Camera() { device->Close(); }

    Reply command(std::initializer_list<DWORD> params)
    {
        auto input = operation(params);
        return response(send(WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE, input.get()).get());
    }

    Reply read(std::initializer_list<DWORD> params)
    {
        auto input = operation(params);
        auto begin = send(WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_READ, input.get());
        const auto context = transfer_context(begin.get());
        ULONGLONG total = 0;
        check(begin->GetUnsignedLargeIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE, &total), "Get transfer size");
        if (total == 0xffffffff)
            throw std::runtime_error("The A620 reported an unknown transfer size");

        std::vector<BYTE> data;
        while (data.size() < total) {
            const auto size = static_cast<DWORD>(std::min<ULONGLONG>(total - data.size(), chunk_size));
            auto chunk = values();
            check(chunk->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, context.get()), "Set read context");
            check(chunk->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_READ, size), "Set read size");
            std::vector<BYTE> buffer(size);
            check(chunk->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, buffer.data(), size), "Set read buffer");
            auto result = send(WPD_COMMAND_MTP_EXT_READ_DATA, chunk.get());
            BYTE *raw = nullptr;
            DWORD length = 0;
            check(result->GetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, &raw, &length), "Get transfer data");
            std::unique_ptr<BYTE, FreeComMemory> owned_data(raw);
            if (length == 0 || length > size)
                throw std::runtime_error("Invalid read size reported by the WPD driver");
            data.insert(data.end(), raw, raw + length);
        }
        auto result = end_transfer(context.get());
        result.data = std::move(data);
        return result;
    }

    Reply write(std::initializer_list<DWORD> params, std::vector<BYTE> data)
    {
        auto input = operation(params);
        check(input->SetUnsignedLargeIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE, data.size()), "Set transfer size");
        auto begin = send(WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_WRITE, input.get());
        const auto context = transfer_context(begin.get());
        for (size_t offset = 0; offset < data.size();) {
            const auto size = static_cast<DWORD>(std::min<size_t>(data.size() - offset, chunk_size));
            auto chunk = values();
            check(chunk->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, context.get()), "Set write context");
            check(chunk->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_WRITE, size), "Set write size");
            check(chunk->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, data.data() + offset, size), "Set write buffer");
            auto result = send(WPD_COMMAND_MTP_EXT_WRITE_DATA, chunk.get());
            DWORD written = 0;
            check(result->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_WRITTEN, &written), "Get write size");
            if (written == 0 || written > size)
                throw std::runtime_error("Invalid write size reported by the WPD driver");
            offset += written;
        }
        return end_transfer(context.get());
    }

private:
    static constexpr DWORD chunk_size = 64 * 1024;
    ComPtr<IPortableDevice> device;

    static ComPtr<IPortableDeviceValues> values()
    {
        return make_com<IPortableDeviceValues>(CLSID_PortableDeviceValues, IID_IPortableDeviceValues);
    }

    static ComPtr<IPortableDeviceValues> operation(std::initializer_list<DWORD> params)
    {
        auto input = values();
        auto args = make_com<IPortableDevicePropVariantCollection>(
            CLSID_PortableDevicePropVariantCollection, IID_IPortableDevicePropVariantCollection);
        for (DWORD param : params) {
            PROPVARIANT arg{};
            arg.vt = VT_UI4;
            arg.ulVal = param;
            check(args->Add(&arg), "Add CHDK command argument");
        }
        check(input->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE, PTP_OC_CHDK), "Set CHDK operation code");
        check(input->SetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS, args.get()), "Set CHDK arguments");
        return input;
    }

    ComPtr<IPortableDeviceValues> send(const PROPERTYKEY &command, IPortableDeviceValues *input)
    {
        check(input->SetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY, command.fmtid), "Set WPD command category");
        check(input->SetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID, command.pid), "Set WPD command ID");
        IPortableDeviceValues *raw = nullptr;
        check(device->SendCommand(0, input, &raw), "Send WPD command");
        ComPtr<IPortableDeviceValues> result(raw);
        HRESULT status = E_FAIL;
        check(result->GetErrorValue(WPD_PROPERTY_COMMON_HRESULT, &status), "Get WPD command status");
        check(status, "WPD command");
        return result;
    }

    static std::unique_ptr<wchar_t, FreeComMemory> transfer_context(IPortableDeviceValues *result)
    {
        PWSTR context = nullptr;
        check(result->GetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, &context), "Get WPD transfer context");
        return std::unique_ptr<wchar_t, FreeComMemory>(context);
    }

    static Reply response(IPortableDeviceValues *result)
    {
        DWORD code = 0;
        check(result->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE, &code), "Get PTP response");
        if (code != 0x2001) {
            std::ostringstream error;
            error << "Camera returned PTP response 0x" << std::hex << code;
            throw std::runtime_error(error.str());
        }
        IPortableDevicePropVariantCollection *raw = nullptr;
        Reply reply;
        const HRESULT status = result->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_RESPONSE_PARAMS, &raw);
        if (status == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
            return reply;
        check(status, "Get PTP response parameters");
        ComPtr<IPortableDevicePropVariantCollection> params(raw);
        DWORD count = 0;
        check(params->GetCount(&count), "Count PTP response parameters");
        for (DWORD i = 0; i < count; ++i) {
            PROPVARIANT param{};
            check(params->GetAt(i, &param), "Read PTP response parameter");
            if (param.vt != VT_UI4) {
                PropVariantClear(&param);
                throw std::runtime_error("Unexpected PTP response parameter type");
            }
            reply.params.push_back(param.ulVal);
        }
        return reply;
    }

    Reply end_transfer(const wchar_t *context)
    {
        auto input = values();
        check(input->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, context), "Set end transfer context");
        return response(send(WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER, input.get()).get());
    }
};

static std::string utf8(const std::wstring &value)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (!length)
        throw std::runtime_error("Could not encode command as UTF-8");
    std::string result(length, '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), length, nullptr, nullptr))
        throw std::runtime_error("Could not encode command as UTF-8");
    result.pop_back();
    return result;
}

static std::vector<DWORD> start_lua(Camera &camera, const std::string &script)
{
    auto status = camera.write({PTP_CHDK_ExecuteScript, PTP_CHDK_SL_LUA |
        PTP_CHDK_SCRIPT_FL_NOKILL | PTP_CHDK_SCRIPT_FL_FLUSH_CAM_MSGS | PTP_CHDK_SCRIPT_FL_FLUSH_HOST_MSGS},
        {script.begin(), script.end()});
    if (status.params.at(1) == PTP_CHDK_S_ERR_SCRIPTRUNNING)
        throw std::runtime_error("A camera script is already running");
    return std::move(status.params);
}

static void run_lua(Camera &camera, const std::string &script)
{
    const auto id = start_lua(camera, script).at(0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        const DWORD status = camera.command({PTP_CHDK_ScriptStatus}).params.at(0);
        if (status & PTP_CHDK_SCRIPT_STATUS_MSG) {
            const auto msg = camera.read({PTP_CHDK_ReadScriptMsg});
            if (msg.params.at(2) != id)
                continue;
            const auto length = msg.params.at(3);
            if (length > msg.data.size())
                throw std::runtime_error("Incomplete CHDK script message");
            const std::string value(msg.data.begin(), msg.data.begin() + length);
            if (msg.params.at(0) == PTP_CHDK_S_MSGTYPE_ERR)
                throw std::runtime_error(value);
            const auto type = msg.params.at(1);
            if (type == PTP_CHDK_TYPE_INTEGER && length == 4) {
                const auto n = static_cast<DWORD>(msg.data[0]) | (static_cast<DWORD>(msg.data[1]) << 8)
                    | (static_cast<DWORD>(msg.data[2]) << 16) | (static_cast<DWORD>(msg.data[3]) << 24);
                std::cout << static_cast<LONG>(n) << '\n';
            } else if (type == PTP_CHDK_TYPE_BOOLEAN && length != 0) {
                std::cout << (msg.data[0] ? "true" : "false") << '\n';
            } else if (type == PTP_CHDK_TYPE_NIL) {
                std::cout << "nil\n";
            } else {
                std::cout << value << '\n';
            }
        } else if (status & PTP_CHDK_SCRIPT_STATUS_RUN) {
            Sleep(100);
        } else {
            return;
        }
    }
    throw std::runtime_error("Timed out waiting for camera Lua");
}

static std::string read_local_file(const wchar_t *path)
{
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input)
        throw std::runtime_error("Could not open local input file");
    input.exceptions(std::ios::badbit);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

int wmain(int argc, wchar_t **argv) try
{
    const std::wstring command = argc > 1 ? argv[1] : L"";
    if (!(((command == L"version" || command == L"reboot") && argc == 2) ||
          ((command == L"lua" || command == L"lua-file") && argc == 3) ||
          (command == L"download" && argc == 4) ||
          (command == L"upload" && argc == 4))) {
        std::cerr << "Usage: chdk-wpd version | reboot | lua SCRIPT | lua-file LOCAL\n"
                     "                | download A/PATH LOCAL | upload LOCAL A/PATH\n";
        return 2;
    }

    ComApartment com;
    Camera camera;
    if (command == L"version") {
        const auto version = camera.command({PTP_CHDK_Version});
        std::cout << "CHDK PTP " << version.params.at(0) << '.' << version.params.at(1) << '\n';
    } else if (command == L"reboot") {
        const auto status = start_lua(camera, "sleep(1500); return reboot()");
        if (status.at(1) != PTP_CHDK_S_ERRTYPE_NONE)
            throw std::runtime_error("The camera rejected the reboot script");
        std::cout << "Reboot requested\n";
    } else if (command == L"lua") {
        run_lua(camera, utf8(argv[2]));
    } else if (command == L"lua-file") {
        auto script = read_local_file(argv[2]);
        if (script.rfind("\xef\xbb\xbf", 0) == 0)
            script.erase(0, 3);
        run_lua(camera, script);
    } else if (command == L"download") {
        const auto name = utf8(argv[2]);
        if (name.rfind("A/", 0) != 0)
            throw std::runtime_error("Camera path must start with A/");
        camera.write({PTP_CHDK_TempData, 0}, {name.begin(), name.end()});
        const auto file = camera.read({PTP_CHDK_DownloadFile});
        if (file.params.at(0) != file.data.size())
            throw std::runtime_error("Downloaded size differs from CHDK's file size");
        std::ofstream output(std::filesystem::path(argv[3]), std::ios::binary);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        if (!file.data.empty())
            output.write(reinterpret_cast<const char *>(file.data.data()), file.data.size());
        output.close();
        std::cout << file.data.size() << " bytes\n";
    } else {
        const auto name = utf8(argv[3]);
        if (name.rfind("A/", 0) != 0)
            throw std::runtime_error("Camera path must start with A/");
        const auto contents = read_local_file(argv[2]);
        const DWORD length = static_cast<DWORD>(name.size());
        std::vector<BYTE> payload{
            static_cast<BYTE>(length), static_cast<BYTE>(length >> 8),
            static_cast<BYTE>(length >> 16), static_cast<BYTE>(length >> 24)
        };
        payload.insert(payload.end(), name.begin(), name.end());
        payload.insert(payload.end(), contents.begin(), contents.end());
        const size_t file_size = payload.size() - sizeof(length) - length;
        camera.write({PTP_CHDK_UploadFile}, std::move(payload));
        std::cout << file_size << " bytes\n";
    }
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
