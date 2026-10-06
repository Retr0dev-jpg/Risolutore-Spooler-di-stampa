#include <windows.h>
#include <winspool.h>
#include <conio.h>
#include <fcntl.h>
#include <io.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr wchar_t kSpooler[] = L"Spooler";
constexpr wchar_t kPrintersKey[] = L"SYSTEM\\CurrentControlSet\\Control\\Print\\Printers";
constexpr int kTotalSteps = 3;
constexpr auto kPurgeTimeout = 10s;
constexpr auto kMaxServiceWait = 60s;

// ─── Handles ────────────────────────────────────────────────────────────────

struct ServiceHandleCloser { void operator()(SC_HANDLE h) const { CloseServiceHandle(h); } };
struct KernelHandleCloser  { void operator()(HANDLE h) const { CloseHandle(h); } };
struct FindHandleCloser    { void operator()(HANDLE h) const { FindClose(h); } };
struct PrinterHandleCloser { void operator()(HANDLE h) const { ClosePrinter(h); } };
struct RegKeyCloser        { void operator()(HKEY h) const { RegCloseKey(h); } };

using ServiceHandle = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ServiceHandleCloser>;
using KernelHandle  = std::unique_ptr<void, KernelHandleCloser>;
using FindHandle    = std::unique_ptr<void, FindHandleCloser>;
using PrinterHandle = std::unique_ptr<void, PrinterHandleCloser>;
using RegKey        = std::unique_ptr<std::remove_pointer_t<HKEY>, RegKeyCloser>;

// ─── Console ────────────────────────────────────────────────────────────────

enum : WORD {
    kGray   = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
    kDim    = FOREGROUND_INTENSITY,
    kCyan   = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
    kGreen  = FOREGROUND_GREEN | FOREGROUND_INTENSITY,
    kYellow = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
    kRed    = FOREGROUND_RED | FOREGROUND_INTENSITY,
};

constexpr size_t kWidth = 64;
HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
WORD baseColor = kGray;

void initConsole() {
    SetConsoleTitleW(L"Risolutore Spooler di stampa");
    _setmode(_fileno(stdout), _O_U16TEXT);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(console, &info)) baseColor = info.wAttributes;
}

void write(const std::wstring& text, WORD color = 0) {
    if (color) SetConsoleTextAttribute(console, color);
    std::wcout << text << std::flush;
    if (color) SetConsoleTextAttribute(console, baseColor);
}

void rule(wchar_t ch, WORD color) { write(std::wstring(kWidth, ch) + L"\n", color); }

void tagged(const wchar_t* tag, WORD color, const std::wstring& text) {
    write(L"  ");
    write(tag, color);
    write(L" " + text + L"\n");
}

void step(const std::wstring& text)   { tagged(L"[..]", kDim, text); }
void ok(const std::wstring& text)     { tagged(L"[OK]", kGreen, text); }
void warn(const std::wstring& text)   { tagged(L"[!!]", kYellow, text); }
void fail(const std::wstring& text)   { tagged(L"[XX]", kRed, text); }
void detail(const std::wstring& text) { write(L"       " + text + L"\n", kDim); }

void header() {
    const std::wstring title = L"  Risolutore Spooler di stampa";
    const std::wstring version = L"v2.0  ";
    rule(L'═', kCyan);
    write(title + std::wstring(kWidth - title.size() - version.size(), L' ') + version + L"\n", kCyan);
    rule(L'═', kCyan);
}

void section(int index, const std::wstring& title) {
    write(L"\n [" + std::to_wstring(index) + L"/" + std::to_wstring(kTotalSteps) + L"] " + title + L"\n", kCyan);
    rule(L'─', kDim);
}

bool askYesNo(const std::wstring& question) {
    write(L"  ");
    write(L"[??]", kYellow);
    write(L" " + question + L" (s/n) ");
    for (;;) {
        const wint_t c = _getwch();
        if (c == L's' || c == L'S') { write(L"s\n"); return true; }
        if (c == L'n' || c == L'N') { write(L"n\n"); return false; }
    }
}

int finish(bool success, const std::wstring& message = L"") {
    write(L"\n");
    rule(L'═', success ? kGreen : kRed);
    write(L"  " + (message.empty()
        ? (success ? L"Coda di stampa ripristinata." : L"Ripristino non riuscito: vedi i messaggi sopra.")
        : message) + L"\n", success ? kGreen : kRed);
    rule(L'═', success ? kGreen : kRed);
    write(L"\n  Premi un tasto per uscire...", kDim);
    _getwch();
    write(L"\n");
    return success ? 0 : 1;
}

std::wstring errorText(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message = length ? std::wstring(buffer, length) : L"Errore sconosciuto";
    LocalFree(buffer);
    while (!message.empty() && wcschr(L"\r\n .", message.back())) message.pop_back();
    return message + L" (" + std::to_wstring(code) + L")";
}

// ─── Spool directories ──────────────────────────────────────────────────────

struct SpoolDir {
    std::wstring path;
    bool spoolFilesOnly;  // per-printer folders may be shared with unrelated files
};

std::wstring regString(HKEY key, const wchar_t* subKey, const wchar_t* value) {
    DWORD size = 0;
    if (RegGetValueW(key, subKey, value, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS) return {};
    for (int attempt = 0; attempt < 3; attempt++) {
        std::wstring text(size / sizeof(wchar_t) + 1, L'\0');
        size = static_cast<DWORD>(text.size() * sizeof(wchar_t));
        const LSTATUS rc = RegGetValueW(key, subKey, value, RRF_RT_REG_SZ, nullptr, text.data(), &size);
        if (rc == ERROR_SUCCESS) {
            text.resize(wcsnlen(text.c_str(), text.size()));
            return text;
        }
        if (rc != ERROR_MORE_DATA) break;
    }
    return {};
}

std::vector<SpoolDir> spoolDirectories() {
    std::vector<SpoolDir> dirs;
    auto add = [&dirs](std::wstring path, bool spoolFilesOnly) {
        while (!path.empty() && wcschr(L"\\/ ", path.back())) path.pop_back();
        if (path.empty()) return;
        for (const auto& dir : dirs)
            if (_wcsicmp(dir.path.c_str(), path.c_str()) == 0) return;
        dirs.push_back({ std::move(path), spoolFilesOnly });
    };

    std::wstring defaultDir = regString(HKEY_LOCAL_MACHINE, kPrintersKey, L"DefaultSpoolDirectory");
    if (defaultDir.empty()) {
        wchar_t system[MAX_PATH];
        const UINT length = GetSystemDirectoryW(system, MAX_PATH);
        if (length && length < MAX_PATH) defaultDir = std::wstring(system, length) + L"\\spool\\PRINTERS";
    }
    add(defaultDir, false);

    HKEY raw = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kPrintersKey, 0, KEY_READ, &raw) == ERROR_SUCCESS) {
        RegKey printers(raw);
        wchar_t name[256];
        for (DWORD i = 0;; i++) {
            DWORD length = ARRAYSIZE(name);
            const LSTATUS rc = RegEnumKeyExW(raw, i, name, &length, nullptr, nullptr, nullptr, nullptr);
            if (rc == ERROR_NO_MORE_ITEMS) break;
            if (rc == ERROR_SUCCESS) add(regString(raw, name, L"SpoolDirectory"), true);
        }
    }
    return dirs;
}

bool isSpoolFile(const std::wstring& name) {
    if (name.size() < 4) return false;
    const wchar_t* extension = name.c_str() + name.size() - 4;
    return _wcsicmp(extension, L".SPL") == 0 || _wcsicmp(extension, L".SHD") == 0;
}

DWORD listFiles(const SpoolDir& dir, std::vector<std::wstring>& names) {
    WIN32_FIND_DATAW data;
    HANDLE raw = FindFirstFileW((dir.path + L"\\*").c_str(), &data);
    if (raw == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) ? ERROR_SUCCESS : error;
    }
    FindHandle find(raw);
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (dir.spoolFilesOnly && !isSpoolFile(data.cFileName)) continue;
        names.push_back(data.cFileName);
    } while (FindNextFileW(raw, &data));
    return ERROR_SUCCESS;
}

size_t countSpoolFiles(const std::vector<SpoolDir>& dirs) {
    size_t count = 0;
    for (const auto& dir : dirs) {
        std::vector<std::wstring> names;
        listFiles({ dir.path, true }, names);
        count += names.size();
    }
    return count;
}

struct ClearResult {
    DWORD listError = ERROR_SUCCESS;
    int deleted = 0;
    std::vector<std::pair<std::wstring, DWORD>> failed;
};

ClearResult clearDirectory(const SpoolDir& dir) {
    ClearResult result;
    // spoolsv.exe can keep file handles open for a moment after the service reports STOPPED.
    for (int attempt = 0; attempt < 5; attempt++) {
        if (attempt > 0) std::this_thread::sleep_for(500ms);
        std::vector<std::wstring> names;
        result.listError = listFiles(dir, names);
        if (result.listError != ERROR_SUCCESS) return result;

        result.failed.clear();
        for (const auto& name : names) {
            if (DeleteFileW((dir.path + L"\\" + name).c_str())) result.deleted++;
            else result.failed.push_back({ name, GetLastError() });
        }
        if (result.failed.empty()) break;
    }
    return result;
}

bool clearSpoolDirectories(const std::vector<SpoolDir>& dirs) {
    bool allCleared = true;
    for (const auto& dir : dirs) {
        const ClearResult result = clearDirectory(dir);
        if (result.listError != ERROR_SUCCESS) {
            fail(L"Cartella non accessibile: " + dir.path);
            detail(errorText(result.listError));
            allCleared = false;
            continue;
        }
        if (result.failed.empty()) {
            ok(L"Eliminati " + std::to_wstring(result.deleted) + L" file da " + dir.path);
            continue;
        }
        fail(std::to_wstring(result.failed.size()) + L" file non eliminabili in " + dir.path
            + L" (eliminati " + std::to_wstring(result.deleted) + L")");
        for (size_t i = 0; i < result.failed.size() && i < 5; i++)
            detail(result.failed[i].first + L": " + errorText(result.failed[i].second));
        allCleared = false;
    }
    return allCleared;
}

// ─── Queue purge (no service restart) ───────────────────────────────────────

struct PurgeReport {
    DWORD error = ERROR_SUCCESS;
    int purged = 0;
    std::vector<std::pair<std::wstring, DWORD>> failures;
};

PurgeReport purgeLocalQueues() {
    PurgeReport report;
    DWORD needed = 0, count = 0;
    if (!EnumPrintersW(PRINTER_ENUM_LOCAL, nullptr, 4, nullptr, 0, &needed, &count)
        && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        report.error = GetLastError();
        return report;
    }
    std::vector<BYTE> buffer(needed);
    if (needed > 0 && !EnumPrintersW(PRINTER_ENUM_LOCAL, nullptr, 4, buffer.data(), needed, &needed, &count)) {
        report.error = GetLastError();
        return report;
    }

    const auto* printers = reinterpret_cast<const PRINTER_INFO_4W*>(buffer.data());
    for (DWORD i = 0; i < count; i++) {
        PRINTER_DEFAULTSW access{ nullptr, nullptr, PRINTER_ACCESS_ADMINISTER };
        HANDLE raw = nullptr;
        if (!OpenPrinterW(printers[i].pPrinterName, &raw, &access)) {
            report.failures.push_back({ printers[i].pPrinterName, GetLastError() });
            continue;
        }
        PrinterHandle printer(raw);
        if (SetPrinterW(printer.get(), 0, nullptr, PRINTER_CONTROL_PURGE)) report.purged++;
        else report.failures.push_back({ printers[i].pPrinterName, GetLastError() });
    }
    return report;
}

bool purgeQueues(const std::vector<SpoolDir>& dirs) {
    step(L"Svuotamento delle code di tutte le stampanti locali...");

    // A hung spooler blocks print API calls indefinitely; the detached thread is released once spoolsv.exe stops.
    auto task = std::make_shared<std::packaged_task<PurgeReport()>>(purgeLocalQueues);
    auto pending = task->get_future();
    std::thread([task] { (*task)(); }).detach();
    if (pending.wait_for(kPurgeTimeout) != std::future_status::ready) {
        warn(L"Lo Spooler non risponde: serve il reset completo.");
        return false;
    }

    const PurgeReport report = pending.get();
    if (report.error != ERROR_SUCCESS) {
        warn(L"Spooler non raggiungibile: " + errorText(report.error));
        return false;
    }
    for (const auto& [name, error] : report.failures)
        warn(L"Coda non svuotata per \"" + name + L"\": " + errorText(error));
    if (!report.failures.empty()) return false;
    ok(L"Code svuotate: " + std::to_wstring(report.purged) + L" stampanti.");

    for (int i = 0; i < 10 && countSpoolFiles(dirs) > 0; i++) std::this_thread::sleep_for(300ms);
    const size_t leftover = countSpoolFiles(dirs);
    if (leftover > 0) {
        warn(std::to_wstring(leftover) + L" file di spool ancora presenti: serve il reset completo.");
        return false;
    }
    ok(L"Nessun file di spool residuo.");
    return true;
}

// ─── Services ───────────────────────────────────────────────────────────────

bool queryStatus(SC_HANDLE service, SERVICE_STATUS_PROCESS& status) {
    DWORD needed = 0;
    return QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed) != FALSE;
}

bool waitForState(SC_HANDLE service, DWORD target, SERVICE_STATUS_PROCESS& status) {
    using Clock = std::chrono::steady_clock;
    if (!queryStatus(service, status)) return false;
    const auto start = Clock::now();
    auto lastProgress = start;
    DWORD checkpoint = status.dwCheckPoint;

    while (status.dwCurrentState != target) {
        if (target == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED) return false;
        // Services often report a wait hint of 0 or an unrealistic one.
        const auto hint = std::chrono::milliseconds(std::clamp<DWORD>(status.dwWaitHint, 5000, 30000));
        const auto now = Clock::now();
        if (now - lastProgress > hint || now - start > kMaxServiceWait) return false;

        std::this_thread::sleep_for(std::clamp<std::chrono::milliseconds>(hint / 10, 250ms, 1000ms));
        if (!queryStatus(service, status)) return false;
        if (status.dwCheckPoint != checkpoint) {
            checkpoint = status.dwCheckPoint;
            lastProgress = Clock::now();
        }
    }
    return true;
}

bool checkSpooler(SC_HANDLE scm, DWORD& state) {
    ServiceHandle service(OpenServiceW(scm, kSpooler, SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS));
    if (!service) {
        fail(L"Servizio Spooler non accessibile: " + errorText(GetLastError()));
        return false;
    }

    DWORD needed = 0;
    QueryServiceConfigW(service.get(), nullptr, 0, &needed);
    std::vector<BYTE> buffer(needed);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!QueryServiceConfigW(service.get(), config, needed, &needed)) {
        fail(L"Configurazione dello Spooler non leggibile: " + errorText(GetLastError()));
        return false;
    }
    if (config->dwStartType == SERVICE_DISABLED) {
        warn(L"Il servizio Spooler è disabilitato: la stampa non può funzionare.");
        if (!askYesNo(L"Impostarlo su avvio automatico?")) return false;
        if (!ChangeServiceConfigW(service.get(), SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
            fail(L"Modifica non riuscita: " + errorText(GetLastError()));
            return false;
        }
        ok(L"Spooler impostato su avvio automatico.");
    }

    SERVICE_STATUS_PROCESS status{};
    if (!queryStatus(service.get(), status)) {
        fail(L"Stato dello Spooler non leggibile: " + errorText(GetLastError()));
        return false;
    }
    state = status.dwCurrentState;
    switch (state) {
    case SERVICE_RUNNING: ok(L"Spooler in esecuzione."); break;
    case SERVICE_STOPPED: warn(L"Spooler arrestato."); break;
    default:              warn(L"Spooler bloccato in una fase di avvio o arresto."); break;
    }
    return true;
}

// Without this, killing spoolsv.exe lets the SCM restart the spooler before the queue is cleared.
class RecoveryActionsPause {
public:
    explicit RecoveryActionsPause(SC_HANDLE scm)
        : service_(OpenServiceW(scm, kSpooler, SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG | SERVICE_START)) {
        if (!service_) return;
        DWORD needed = 0;
        QueryServiceConfig2W(service_.get(), SERVICE_CONFIG_FAILURE_ACTIONS, nullptr, 0, &needed);
        saved_.resize(needed);
        if (needed == 0 || !QueryServiceConfig2W(service_.get(), SERVICE_CONFIG_FAILURE_ACTIONS, saved_.data(), needed, &needed)) {
            warn(L"Azioni di ripristino dello Spooler non leggibili: " + errorText(GetLastError()));
            return;
        }
        if (savedActions()->cActions == 0) return;

        SC_ACTION placeholder{};
        SERVICE_FAILURE_ACTIONSW none{};
        none.lpsaActions = &placeholder;  // non-null with cActions = 0 deletes the actions
        if (ChangeServiceConfig2W(service_.get(), SERVICE_CONFIG_FAILURE_ACTIONS, &none)) paused_ = true;
        else warn(L"Azioni di ripristino non sospese: " + errorText(GetLastError()));
    }

    ~RecoveryActionsPause() {
        if (paused_ && !ChangeServiceConfig2W(service_.get(), SERVICE_CONFIG_FAILURE_ACTIONS, savedActions()))
            warn(L"Azioni di ripristino dello Spooler non ripristinate: " + errorText(GetLastError()));
    }

    RecoveryActionsPause(const RecoveryActionsPause&) = delete;
    RecoveryActionsPause& operator=(const RecoveryActionsPause&) = delete;

private:
    SERVICE_FAILURE_ACTIONSW* savedActions() { return reinterpret_cast<SERVICE_FAILURE_ACTIONSW*>(saved_.data()); }

    ServiceHandle service_;
    std::vector<BYTE> saved_;
    bool paused_ = false;
};

struct ServiceName {
    std::wstring name;
    std::wstring display;
};

std::vector<ServiceName> stopDependents(SC_HANDLE scm) {
    std::vector<ServiceName> stopped;
    ServiceHandle spooler(OpenServiceW(scm, kSpooler, SERVICE_ENUMERATE_DEPENDENTS));
    if (!spooler) return stopped;

    DWORD needed = 0, count = 0;
    if (EnumDependentServicesW(spooler.get(), SERVICE_ACTIVE, nullptr, 0, &needed, &count)
        || GetLastError() != ERROR_MORE_DATA) return stopped;
    std::vector<BYTE> buffer(needed);
    auto* dependents = reinterpret_cast<ENUM_SERVICE_STATUSW*>(buffer.data());
    if (!EnumDependentServicesW(spooler.get(), SERVICE_ACTIVE, dependents, needed, &needed, &count)) return stopped;

    // Returned in reverse start order, which is the correct stop order.
    for (DWORD i = 0; i < count; i++) {
        const ServiceName dependent{ dependents[i].lpServiceName, dependents[i].lpDisplayName };
        ServiceHandle service(OpenServiceW(scm, dependent.name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS));
        SERVICE_STATUS ignored{};
        SERVICE_STATUS_PROCESS status{};
        if (service && ControlService(service.get(), SERVICE_CONTROL_STOP, &ignored)
            && waitForState(service.get(), SERVICE_STOPPED, status)) {
            ok(L"Servizio dipendente arrestato: " + dependent.display);
            stopped.push_back(dependent);
        } else {
            warn(L"Servizio dipendente non arrestato: " + dependent.display);
        }
    }
    return stopped;
}

void startDependents(SC_HANDLE scm, const std::vector<ServiceName>& dependents) {
    for (auto it = dependents.rbegin(); it != dependents.rend(); ++it) {
        ServiceHandle service(OpenServiceW(scm, it->name.c_str(), SERVICE_START | SERVICE_QUERY_STATUS));
        SERVICE_STATUS_PROCESS status{};
        const bool started = service
            && (StartServiceW(service.get(), 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING)
            && waitForState(service.get(), SERVICE_RUNNING, status);
        if (started) ok(L"Servizio dipendente riavviato: " + it->display);
        else warn(L"Servizio dipendente non riavviato: " + it->display);
    }
}

bool enablePrivilege(const wchar_t* name) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &raw)) return false;
    KernelHandle token(raw);
    TOKEN_PRIVILEGES privileges{ 1 };
    if (!LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid)) return false;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    return AdjustTokenPrivileges(token.get(), FALSE, &privileges, sizeof(privileges), nullptr, nullptr)
        && GetLastError() == ERROR_SUCCESS;
}

bool isSpoolerProcess(HANDLE process) {
    wchar_t image[MAX_PATH];
    DWORD length = MAX_PATH;
    if (!QueryFullProcessImageNameW(process, 0, image, &length)) return false;
    const std::wstring path(image, length);
    const size_t slash = path.find_last_of(L'\\');
    return _wcsicmp(path.c_str() + (slash == std::wstring::npos ? 0 : slash + 1), L"spoolsv.exe") == 0;
}

bool terminateSpooler(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{};
    if (!queryStatus(service, status)) {
        fail(L"Stato dello Spooler non leggibile: " + errorText(GetLastError()));
        return false;
    }
    if (status.dwCurrentState == SERVICE_STOPPED) return true;
    if (status.dwProcessId == 0 || (status.dwServiceFlags & SERVICE_RUNS_IN_SYSTEM_PROCESS)) {
        fail(L"Processo dello Spooler non identificabile.");
        return false;
    }

    enablePrivilege(SE_DEBUG_NAME);
    KernelHandle process(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, status.dwProcessId));
    if (!process) {
        fail(L"Impossibile aprire spoolsv.exe: " + errorText(GetLastError()));
        return false;
    }
    if (!isSpoolerProcess(process.get())) {
        fail(L"Il processo del servizio non è spoolsv.exe: terminazione annullata.");
        return false;
    }
    if (!TerminateProcess(process.get(), 1)) {
        fail(L"Terminazione di spoolsv.exe non riuscita: " + errorText(GetLastError()));
        return false;
    }
    WaitForSingleObject(process.get(), 10000);
    if (!waitForState(service, SERVICE_STOPPED, status)) {
        fail(L"Lo Spooler non risulta arrestato neanche dopo la terminazione forzata.");
        return false;
    }
    ok(L"spoolsv.exe terminato, Spooler arrestato.");
    return true;
}

bool stopSpooler(SC_HANDLE scm) {
    ServiceHandle service(OpenServiceW(scm, kSpooler, SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!service) {
        fail(L"Servizio Spooler non accessibile: " + errorText(GetLastError()));
        return false;
    }
    SERVICE_STATUS_PROCESS status{};
    if (!queryStatus(service.get(), status)) {
        fail(L"Stato dello Spooler non leggibile: " + errorText(GetLastError()));
        return false;
    }
    if (status.dwCurrentState == SERVICE_STOPPED) {
        ok(L"Spooler già arrestato.");
        return true;
    }

    step(L"Arresto dello Spooler...");
    SERVICE_STATUS ignored{};
    if (!ControlService(service.get(), SERVICE_CONTROL_STOP, &ignored)) {
        const DWORD error = GetLastError();
        if (error != ERROR_SERVICE_NOT_ACTIVE && error != ERROR_SERVICE_CANNOT_ACCEPT_CTRL
            && error != ERROR_SERVICE_REQUEST_TIMEOUT) {
            fail(L"Arresto rifiutato: " + errorText(error));
            return false;
        }
    }
    if (waitForState(service.get(), SERVICE_STOPPED, status)) {
        ok(L"Spooler arrestato.");
        return true;
    }

    warn(L"Lo Spooler non si arresta: terminazione forzata di spoolsv.exe.");
    return terminateSpooler(service.get());
}

bool startSpooler(SC_HANDLE scm) {
    ServiceHandle service(OpenServiceW(scm, kSpooler, SERVICE_START | SERVICE_QUERY_STATUS));
    if (!service) {
        fail(L"Servizio Spooler non accessibile: " + errorText(GetLastError()));
        return false;
    }

    step(L"Avvio dello Spooler...");
    if (!StartServiceW(service.get(), 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        fail(L"Avvio non riuscito: " + errorText(GetLastError()));
        return false;
    }
    SERVICE_STATUS_PROCESS status{};
    if (!waitForState(service.get(), SERVICE_RUNNING, status)) {
        fail(L"Lo Spooler non ha completato l'avvio.");
        if (status.dwWin32ExitCode != NO_ERROR) detail(errorText(status.dwWin32ExitCode));
        return false;
    }
    ok(L"Spooler avviato.");
    return true;
}

bool fullReset(SC_HANDLE scm, const std::vector<SpoolDir>& dirs) {
    const auto dependents = stopDependents(scm);

    bool stopped = false;
    bool cleared = false;
    {
        RecoveryActionsPause pause(scm);
        stopped = stopSpooler(scm);
        if (stopped) cleared = clearSpoolDirectories(dirs);
        else warn(L"Pulizia dei file saltata: lo Spooler è ancora attivo.");
    }

    // Always restart, even after a failed step, so printing is never left disabled.
    const bool started = startSpooler(scm);
    if (started) startDependents(scm, dependents);
    else for (const auto& dependent : dependents) warn(L"Servizio dipendente rimasto arrestato: " + dependent.display);

    return stopped && cleared && started;
}

}  // namespace

// ─── Main ───────────────────────────────────────────────────────────────────

int wmain() {
    initConsole();
    header();

    ServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) {
        fail(L"Gestione servizi non accessibile: " + errorText(GetLastError()));
        return finish(false);
    }

    section(1, L"Verifica del servizio Spooler");
    DWORD state = 0;
    if (!checkSpooler(scm.get(), state)) return finish(false);
    const auto dirs = spoolDirectories();
    for (const auto& dir : dirs) detail(L"Cartella di spool: " + dir.path);

    section(2, L"Svuotamento delle code senza riavvio");
    if (state != SERVICE_RUNNING) step(L"Spooler non in esecuzione: si passa al reset completo.");
    else if (purgeQueues(dirs)) return finish(true, L"Code svuotate senza riavviare lo Spooler.");

    section(3, L"Reset completo dello Spooler");
    return finish(fullReset(scm.get(), dirs));
}
