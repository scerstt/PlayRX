// What the USB check (Settings -> Receivers) reports about this PC besides the streams themselves:
//   usb_controller(vid, pid, port)  the USB host controller an adapter is plugged into ("AMD USB 3.10 eXtensible
//                                   Host Controller - 1.10 (Microsoft) [PCI 1022:15B7]"); "" if unknown
//   power_info()                    mains or battery, and the Windows power plan and power mode
// Windows only (SetupAPI / CfgMgr32 / PowrProf); on macOS and Linux both say "" (not shown).
// Header-only so the Mac and Linux builds need no new source file.
#pragma once
#include <cstdint>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h>  // before devpkey.h: the DEVPKEY_* values are defined here (selectany)
#include <devpkey.h>
#include <powrprof.h>
#include <setupapi.h>

#include <cstdio>
#include <cwchar>
#include <vector>
#endif

namespace arx {

struct PowerInfo {
  std::string source;  // "mains", "battery", "" (unknown; desktops without a battery say "mains")
  std::string plan;    // the power plan's name ("Balanced")
  std::string mode;    // Windows 10/11 power mode: "best performance", "balanced", "best power efficiency", ""
};

#ifdef _WIN32
namespace usb_info_detail {

inline std::string narrow(const wchar_t *w) {
  if (!w || !*w) return "";
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

// a device node's property as text (the first string of a list)
inline std::wstring prop(DEVINST dn, const DEVPROPKEY &key) {
  DEVPROPTYPE type = 0;
  ULONG size = 0;
  if (CM_Get_DevNode_PropertyW(dn, &key, &type, nullptr, &size, 0) != CR_BUFFER_SMALL || size < 2) return L"";
  std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
  if (CM_Get_DevNode_PropertyW(dn, &key, &type, reinterpret_cast<PBYTE>(buf.data()), &size, 0) != CR_SUCCESS) return L"";
  if (type != DEVPROP_TYPE_STRING && type != DEVPROP_TYPE_STRING_LIST) return L"";
  return buf.data();
}

inline std::wstring instance_id(DEVINST dn) {
  wchar_t id[MAX_DEVICE_ID_LEN + 1] = {};
  return CM_Get_Device_IDW(dn, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS ? id : L"";
}

// "PCIROOT(0)#PCI(0801)#PCI(0003)#USBROOT(0)#USB(3)#USB(1)" -> "3.1" (libusb's port path)
inline std::string port_path(const std::wstring &loc) {
  std::string out;
  size_t p = loc.find(L"USBROOT(");
  if (p == std::wstring::npos) return "";
  while ((p = loc.find(L"#USB(", p)) != std::wstring::npos) {
    p += 5;
    const size_t e = loc.find(L')', p);
    if (e == std::wstring::npos) break;
    out += (out.empty() ? "" : ".") + narrow(loc.substr(p, e - p).c_str());
    p = e;
  }
  return out;
}

}  // namespace usb_info_detail

inline std::string usb_controller(uint16_t vid, uint16_t pid, const std::string &port) {
  using namespace usb_info_detail;
  HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
  if (set == INVALID_HANDLE_VALUE) return "";
  wchar_t want[32];
  std::swprintf(want, 32, L"VID_%04X&PID_%04X", vid, pid);
  std::string found;
  SP_DEVINFO_DATA d{};
  d.cbSize = sizeof(d);
  for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); i++) {
    std::wstring id = instance_id(d.DevInst);
    for (auto &c : id) c = static_cast<wchar_t>(towupper(c));
    if (id.find(want) == std::wstring::npos || id.find(L"&MI_") != std::wstring::npos) continue;  // the device, not an interface
    if (!port.empty() && port_path(prop(d.DevInst, DEVPKEY_Device_LocationPaths)) != port) continue;
    // up the tree to the host controller: the first parent on the PCI bus
    DEVINST dn = d.DevInst, parent = 0;
    std::string name;
    for (int up = 0; up < 12 && CM_Get_Parent(&parent, dn, 0) == CR_SUCCESS; up++) {
      dn = parent;
      std::wstring pid_s = instance_id(dn);
      if (pid_s.compare(0, 4, L"PCI\\") != 0) continue;
      std::wstring desc = prop(dn, DEVPKEY_Device_FriendlyName);
      if (desc.empty()) desc = prop(dn, DEVPKEY_Device_DeviceDesc);
      name = narrow(desc.c_str());
      // PCI\VEN_1022&DEV_15B7&... -> [PCI 1022:15B7]
      const size_t v = pid_s.find(L"VEN_"), dv = pid_s.find(L"DEV_");
      if (v != std::wstring::npos && dv != std::wstring::npos)
        name += " [PCI " + narrow(pid_s.substr(v + 4, 4).c_str()) + ":" + narrow(pid_s.substr(dv + 4, 4).c_str()) + "]";
      break;
    }
    if (!name.empty() && found.find(name) == std::string::npos) found += (found.empty() ? "" : " / ") + name;
  }
  SetupDiDestroyDeviceInfoList(set);
  return found;
}

inline PowerInfo power_info() {
  PowerInfo p;
  SYSTEM_POWER_STATUS ps{};
  if (GetSystemPowerStatus(&ps)) {
    if (ps.BatteryFlag == 128) p.source = "mains";  // no battery: a desktop
    else if (ps.ACLineStatus == 1) p.source = "mains";
    else if (ps.ACLineStatus == 0) p.source = "battery";
  }
  GUID *scheme = nullptr;
  if (PowerGetActiveScheme(nullptr, &scheme) == ERROR_SUCCESS && scheme) {
    DWORD size = 0;
    if (PowerReadFriendlyName(nullptr, scheme, nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS && size > 2) {
      std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
      if (PowerReadFriendlyName(nullptr, scheme, nullptr, nullptr, reinterpret_cast<UCHAR *>(buf.data()), &size) == ERROR_SUCCESS)
        p.plan = usb_info_detail::narrow(buf.data());
    }
    LocalFree(scheme);
  }
  // the power mode slider (Windows 10 1809+): powrprof's PowerGetEffectiveOverlayScheme, looked up at run time
  using OverlayFn = DWORD(WINAPI *)(GUID *);
  if (HMODULE pp = GetModuleHandleW(L"powrprof.dll")) {
    if (auto fn = reinterpret_cast<OverlayFn>(reinterpret_cast<void *>(GetProcAddress(pp, "PowerGetEffectiveOverlayScheme")))) {
      GUID g{};
      if (fn(&g) == ERROR_SUCCESS) {
        static const GUID kEff = {0x961cc777, 0x2547, 0x4f9d, {0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}};
        static const GUID kPerf = {0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}};
        static const GUID kNone = {};
        if (IsEqualGUID(g, kEff)) p.mode = "best power efficiency";
        else if (IsEqualGUID(g, kPerf)) p.mode = "best performance";
        else if (IsEqualGUID(g, kNone)) p.mode = "balanced";
      }
    }
  }
  return p;
}

inline std::string cpu_name() {  // "AMD Ryzen 7 5800X 8-Core Processor"
  wchar_t buf[128] = {};
  DWORD size = sizeof(buf);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                   RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
    return "";
  std::string s = usb_info_detail::narrow(buf);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}
#else
inline std::string usb_controller(uint16_t, uint16_t, const std::string &) { return ""; }
inline PowerInfo power_info() { return {}; }
inline std::string cpu_name() { return ""; }
#endif

}  // namespace arx
