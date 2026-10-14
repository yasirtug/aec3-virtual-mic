#include "echo_model.h"

#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>

#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kDataDirectoryName[] = L"AEC3VirtualMic";
constexpr wchar_t kModelFileName[] = L"residual_echo_estimator.tflite";

// Chrome Stable, Beta, Dev and Canary user data roots below %LOCALAPPDATA%.
constexpr const wchar_t* kChromeUserDataDirectories[] = {
    L"\\Google\\Chrome\\User Data",
    L"\\Google\\Chrome Beta\\User Data",
    L"\\Google\\Chrome Dev\\User Data",
    L"\\Google\\Chrome SxS\\User Data",
};

// Optimization guide target 71: OPTIMIZATION_TARGET_WEBRTC_NEURAL_RESIDUAL_
// ECHO_ESTIMATOR. Layout: <store>\71\<hash>\<version>\model.tflite.
constexpr wchar_t kModelStoreDirectory[] =
    L"\\optimization_guide_model_store\\71";
constexpr wchar_t kChromeModelFileName[] = L"model.tflite";

std::wstring Win32ErrorText(const std::wstring& operation, DWORD code) {
  wchar_t* system_message = nullptr;
  const DWORD size = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<wchar_t*>(&system_message), 0,
      nullptr);

  std::wstring message(operation);
  message.append(L" failed (error ").append(std::to_wstring(code)).append(L")");
  if (size != 0 && system_message != nullptr) {
    std::wstring detail(system_message, size);
    while (!detail.empty() &&
           (detail.back() == L'\r' || detail.back() == L'\n')) {
      detail.pop_back();
    }
    message.append(L": ").append(detail);
  }
  if (system_message != nullptr) {
    LocalFree(system_message);
  }
  return message;
}

std::wstring LocalAppDataPath() {
  PWSTR path = nullptr;
  std::wstring result;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT,
                                     nullptr, &path)) &&
      path != nullptr) {
    result = path;
  }
  CoTaskMemFree(path);
  return result;
}

bool DirectoryExists(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::vector<std::wstring> Subdirectories(const std::wstring& directory) {
  std::vector<std::wstring> result;
  WIN32_FIND_DATAW data = {};
  const std::wstring pattern = directory + L"\\*";
  HANDLE find =
      FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                       FindExSearchLimitToDirectories, nullptr, 0);
  if (find == INVALID_HANDLE_VALUE) {
    return result;
  }
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        std::wcscmp(data.cFileName, L".") != 0 &&
        std::wcscmp(data.cFileName, L"..") != 0) {
      result.push_back(directory + L"\\" + data.cFileName);
    }
  } while (FindNextFileW(find, &data));
  FindClose(find);
  return result;
}

// Returns true for a non-empty regular file and reports its last write time.
bool NonEmptyFile(const std::wstring& path, FILETIME* last_write_time) {
  WIN32_FILE_ATTRIBUTE_DATA attributes = {};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard,
                            &attributes) ||
      (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
      (attributes.nFileSizeHigh == 0 && attributes.nFileSizeLow == 0)) {
    return false;
  }
  if (last_write_time != nullptr) {
    *last_write_time = attributes.ftLastWriteTime;
  }
  return true;
}

void SetError(std::wstring* error, std::wstring message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

}  // namespace

std::wstring EchoModelPath() {
  const std::wstring local_app_data = LocalAppDataPath();
  if (local_app_data.empty()) {
    return {};
  }
  return local_app_data + L"\\" + kDataDirectoryName + L"\\" + kModelFileName;
}

bool EchoModelInstalled() {
  const std::wstring path = EchoModelPath();
  return !path.empty() && NonEmptyFile(path, nullptr);
}

bool ImportEchoModelFromChrome(std::wstring* error) {
  if (error != nullptr) {
    error->clear();
  }
  const std::wstring local_app_data = LocalAppDataPath();
  if (local_app_data.empty()) {
    SetError(error,
             L"Windows did not report this user's local application data "
             L"folder, so Chrome's neural echo model cannot be located.");
    return false;
  }

  bool chrome_found = false;
  std::wstring newest_model;
  FILETIME newest_time = {};
  for (const wchar_t* user_data : kChromeUserDataDirectories) {
    const std::wstring user_data_path = local_app_data + user_data;
    if (!DirectoryExists(user_data_path)) {
      continue;
    }
    chrome_found = true;
    const std::wstring store = user_data_path + kModelStoreDirectory;
    for (const std::wstring& hash_directory : Subdirectories(store)) {
      for (const std::wstring& version_directory :
           Subdirectories(hash_directory)) {
        const std::wstring candidate =
            version_directory + L"\\" + kChromeModelFileName;
        FILETIME write_time = {};
        if (NonEmptyFile(candidate, &write_time) &&
            (newest_model.empty() ||
             CompareFileTime(&write_time, &newest_time) > 0)) {
          newest_model = candidate;
          newest_time = write_time;
        }
      }
    }
  }

  if (!chrome_found) {
    SetError(error,
             L"Google Chrome was not found for this Windows user. The neural "
             L"echo model is Google's residual echo estimator that Chrome "
             L"downloads for its own echo cancellation; this app can only "
             L"copy it from a local Chrome installation.");
    return false;
  }
  if (newest_model.empty()) {
    SetError(error,
             L"Google Chrome has not downloaded its neural echo model "
             L"(residual echo estimator) yet. Chrome fetches it in the "
             L"background on its own schedule; keep Chrome running for a "
             L"while and import again.");
    return false;
  }

  const std::wstring directory =
      local_app_data + L"\\" + kDataDirectoryName;
  if (!CreateDirectoryW(directory.c_str(), nullptr)) {
    const DWORD code = GetLastError();
    if (code != ERROR_ALREADY_EXISTS) {
      SetError(error,
               Win32ErrorText(L"Creating the folder " + directory, code));
      return false;
    }
  }

  const std::wstring destination = directory + L"\\" + kModelFileName;
  const std::wstring temporary = destination + L".tmp";
  if (!CopyFileW(newest_model.c_str(), temporary.c_str(), FALSE)) {
    const DWORD code = GetLastError();
    DeleteFileW(temporary.c_str());
    SetError(error,
             Win32ErrorText(L"Copying Chrome's neural echo model", code));
    return false;
  }
  if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD code = GetLastError();
    DeleteFileW(temporary.c_str());
    SetError(error,
             Win32ErrorText(L"Saving the neural echo model", code));
    return false;
  }
  return true;
}
