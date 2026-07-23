#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>

#include "../../Common/Include/vdd_control_ioctl.h"

#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")

namespace {

struct Handle {
  HANDLE value {INVALID_HANDLE_VALUE};
  ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct DeviceInfoSet {
  HDEVINFO value {INVALID_HANDLE_VALUE};
  ~DeviceInfoSet() { if (value != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(value); }
};

bool open_control_device(Handle& output, std::wstring& path, DWORD& native_error) {
  const GUID interface_guid = ZAKO_VDD_CONTROL_GUID_INIT;
  DeviceInfoSet devices;
  devices.value = SetupDiGetClassDevsW(&interface_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (devices.value == INVALID_HANDLE_VALUE) {
    native_error = GetLastError();
    return false;
  }

  for (DWORD index = 0;; ++index) {
    SP_DEVICE_INTERFACE_DATA interface_data {};
    interface_data.cbSize = sizeof(interface_data);
    if (!SetupDiEnumDeviceInterfaces(devices.value, nullptr, &interface_guid, index, &interface_data)) {
      native_error = GetLastError();
      return false;
    }
    DWORD required = 0;
    SetupDiGetDeviceInterfaceDetailW(devices.value, &interface_data, nullptr, 0, &required, nullptr);
    if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
    std::vector<BYTE> storage(required);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
    detail->cbSize = sizeof(*detail);
    if (!SetupDiGetDeviceInterfaceDetailW(devices.value, &interface_data, detail, required, nullptr, nullptr)) {
      native_error = GetLastError();
      continue;
    }
    Handle candidate;
    candidate.value = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (candidate.value == INVALID_HANDLE_VALUE) {
      native_error = GetLastError();
      continue;
    }
    path = detail->DevicePath;
    output.value = candidate.value;
    candidate.value = INVALID_HANDLE_VALUE;
    return true;
  }
}

bool target_hdr_enabled(const DISPLAYCONFIG_PATH_INFO& path) {
  struct AdvancedColorInfo2 {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    union {
      struct {
        UINT32 advancedColorSupported : 1;
        UINT32 advancedColorActive : 1;
        UINT32 reserved1 : 1;
        UINT32 advancedColorLimitedByPolicy : 1;
        UINT32 highDynamicRangeSupported : 1;
        UINT32 highDynamicRangeUserEnabled : 1;
        UINT32 wideColorSupported : 1;
        UINT32 wideColorUserEnabled : 1;
        UINT32 reserved : 24;
      } bits;
      UINT32 value;
    } state;
    DISPLAYCONFIG_COLOR_ENCODING colorEncoding;
    UINT32 bitsPerColorChannel;
    UINT32 activeColorMode;
  } info2 {};
  info2.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15);
  info2.header.size = sizeof(info2);
  info2.header.adapterId = path.targetInfo.adapterId;
  info2.header.id = path.targetInfo.id;
  if (DisplayConfigGetDeviceInfo(&info2.header) == ERROR_SUCCESS) return info2.state.bits.highDynamicRangeUserEnabled != 0;

  DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info1 {};
  info1.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
  info1.header.size = sizeof(info1);
  info1.header.adapterId = path.targetInfo.adapterId;
  info1.header.id = path.targetInfo.id;
  return DisplayConfigGetDeviceInfo(&info1.header) == ERROR_SUCCESS && info1.advancedColorEnabled != 0;
}

bool set_display_hdr(const std::wstring& gdi_name, bool enabled, LONG& native_error) {
  UINT32 path_count = 0;
  UINT32 mode_count = 0;
  native_error = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
  if (native_error != ERROR_SUCCESS) return false;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  native_error = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
  if (native_error != ERROR_SUCCESS) return false;

  for (UINT32 index = 0; index < path_count; ++index) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = paths[index].sourceInfo.adapterId;
    source.header.id = paths[index].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
        _wcsicmp(source.viewGdiDeviceName, gdi_name.c_str()) != 0) continue;

    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE state {};
    state.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
    state.header.size = sizeof(state);
    state.header.adapterId = paths[index].targetInfo.adapterId;
    state.header.id = paths[index].targetInfo.id;
    state.enableAdvancedColor = enabled ? 1 : 0;
    native_error = DisplayConfigSetDeviceInfo(&state.header);
    if (native_error == ERROR_SUCCESS) return true;
    // Some Windows builds apply the per-user advanced-color transition but
    // return access denied while attempting to persist an adapter-level part
    // of the request. Treat the operation as successful only after querying
    // the effective target state back from DisplayConfig.
    if (target_hdr_enabled(paths[index]) == enabled) {
      native_error = ERROR_SUCCESS;
      return true;
    }
    return false;
  }
  native_error = ERROR_NOT_FOUND;
  return false;
}

bool list_all_display_paths(LONG& native_error) {
  UINT32 path_count = 0;
  UINT32 mode_count = 0;
  constexpr UINT32 query_flags = QDC_ALL_PATHS | QDC_VIRTUAL_MODE_AWARE;
  native_error = GetDisplayConfigBufferSizes(query_flags, &path_count, &mode_count);
  if (native_error != ERROR_SUCCESS) return false;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  native_error = QueryDisplayConfig(query_flags, &path_count, paths.data(),
                                    &mode_count, modes.data(), nullptr);
  if (native_error != ERROR_SUCCESS) return false;

  std::wcout << L"path_count=" << path_count << L'\n';
  for (UINT32 index = 0; index < path_count; ++index) {
    const auto& path = paths[index];
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = path.sourceInfo.adapterId;
    source.header.id = path.sourceInfo.id;
    const LONG source_status = DisplayConfigGetDeviceInfo(&source.header);

    DISPLAYCONFIG_TARGET_DEVICE_NAME target {};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = path.targetInfo.adapterId;
    target.header.id = path.targetInfo.id;
    const LONG target_status = DisplayConfigGetDeviceInfo(&target.header);

    std::wcout << L"path[" << index << L"].active="
               << ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) ? 1 : 0)
               << L" adapter=" << path.sourceInfo.adapterId.HighPart << L":"
               << std::hex << path.sourceInfo.adapterId.LowPart << std::dec
               << L" source_id=" << path.sourceInfo.id
               << L" target_id=" << path.targetInfo.id
               << L" source=" << (source_status == ERROR_SUCCESS ? source.viewGdiDeviceName : L"<inactive>")
               << L" monitor=" << (target_status == ERROR_SUCCESS ? target.monitorFriendlyDeviceName : L"<unknown>")
               << L" device_path=" << (target_status == ERROR_SUCCESS ? target.monitorDevicePath : L"<unknown>")
               << L'\n';
  }
  return true;
}

bool activate_zako_display(LONG& native_error) {
  UINT32 path_count = 0;
  UINT32 mode_count = 0;
  native_error = GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &path_count, &mode_count);
  if (native_error != ERROR_SUCCESS) return false;
  std::vector<DISPLAYCONFIG_PATH_INFO> all_paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  native_error = QueryDisplayConfig(QDC_ALL_PATHS, &path_count, all_paths.data(),
                                    &mode_count, modes.data(), nullptr);
  if (native_error != ERROR_SUCCESS) return false;

  std::vector<DISPLAYCONFIG_PATH_INFO> selected;
  DISPLAYCONFIG_PATH_INFO zako_path {};
  bool found_zako = false;
  for (UINT32 index = 0; index < path_count; ++index) {
    auto path = all_paths[index];
    if (path.flags & DISPLAYCONFIG_PATH_ACTIVE) selected.push_back(path);

    DISPLAYCONFIG_TARGET_DEVICE_NAME target {};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = path.targetInfo.adapterId;
    target.header.id = path.targetInfo.id;
    if (!found_zako && DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS &&
        (wcsstr(target.monitorDevicePath, L"ZAK2333") != nullptr ||
         wcsstr(target.monitorFriendlyDeviceName, L"Zako") != nullptr)) {
      zako_path = path;
      found_zako = true;
    }
  }
  if (!found_zako) {
    native_error = ERROR_NOT_FOUND;
    return false;
  }
  if (!(zako_path.flags & DISPLAYCONFIG_PATH_ACTIVE)) selected.push_back(zako_path);

  for (auto& path : selected) {
    path.flags |= DISPLAYCONFIG_PATH_ACTIVE;
    path.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    path.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
  }
  const UINT32 base_flags = SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES;
  native_error = SetDisplayConfig(static_cast<UINT32>(selected.size()), selected.data(),
                                  0, nullptr, base_flags | SDC_VALIDATE);
  if (native_error != ERROR_SUCCESS) return false;
  native_error = SetDisplayConfig(static_cast<UINT32>(selected.size()), selected.data(),
                                  0, nullptr, base_flags | SDC_APPLY | SDC_SAVE_TO_DATABASE);
  return native_error == ERROR_SUCCESS;
}

void usage() {
  std::cout << "Usage: vdd_command.exe <COMMAND> [arguments...]\n"
               "       vdd_command.exe HDR-ON \\\\.\\DISPLAYn\n"
               "       vdd_command.exe HDR-OFF \\\\.\\DISPLAYn\n"
               "       vdd_command.exe DISPLAY-LIST-ALL\n"
               "       vdd_command.exe DISPLAY-ACTIVATE-ZAKO\n"
               "Example: vdd_command.exe CREATEMONITOR\n";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  if ((std::wstring(argv[1]) == L"HDR-ON" || std::wstring(argv[1]) == L"HDR-OFF")) {
    if (argc != 3) {
      usage();
      return 2;
    }
    const bool enabled = std::wstring(argv[1]) == L"HDR-ON";
    LONG native_error = ERROR_SUCCESS;
    if (!set_display_hdr(argv[2], enabled, native_error)) {
      std::wcerr << L"display=" << argv[2] << L" hdr=" << enabled << L" status=FAILED native_error=" << native_error << L'\n';
      return 5;
    }
    std::wcout << L"display=" << argv[2] << L" hdr=" << enabled << L" status=SUCCESS\n";
    return 0;
  }
  if (std::wstring(argv[1]) == L"DISPLAY-LIST-ALL") {
    LONG native_error = ERROR_SUCCESS;
    if (!list_all_display_paths(native_error)) {
      std::wcerr << L"display_list_all=FAILED native_error=" << native_error << L'\n';
      return 6;
    }
    return 0;
  }
  if (std::wstring(argv[1]) == L"DISPLAY-ACTIVATE-ZAKO") {
    LONG native_error = ERROR_SUCCESS;
    if (!activate_zako_display(native_error)) {
      std::wcerr << L"display_activate_zako=FAILED native_error=" << native_error << L'\n';
      return 7;
    }
    std::wcout << L"display_activate_zako=SUCCESS\n";
    return 0;
  }
  std::wstring command = argv[1];
  for (int index = 2; index < argc; ++index) {
    command.push_back(L' ');
    command.append(argv[index]);
  }

  Handle device;
  std::wstring path;
  DWORD native_error = ERROR_SUCCESS;
  if (!open_control_device(device, path, native_error)) {
    std::wcerr << L"open_control_device=FAILED native_error=" << native_error << L'\n';
    return 3;
  }

  DWORD returned = 0;
  const DWORD bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
  const BOOL ok = DeviceIoControl(device.value, IOCTL_VDD_COMMAND,
                                  command.data(), bytes, nullptr, 0, &returned, nullptr);
  if (!ok) {
    std::wcerr << L"command=FAILED native_error=" << GetLastError() << L'\n';
    return 4;
  }
  std::wcout << L"device=" << path << L'\n' << L"command=" << command << L" status=SUCCESS\n";
  return 0;
}
