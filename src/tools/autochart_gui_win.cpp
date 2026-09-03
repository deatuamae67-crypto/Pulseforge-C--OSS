#if !defined(_WIN32)
#error pulseforge-autochart-gui is Windows-only
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t window_class_name[] = L"PulseForgeAutoChartWindow";
constexpr UINT message_log = WM_APP + 1U;
constexpr UINT message_complete = WM_APP + 2U;
constexpr UINT message_review = WM_APP + 3U;

constexpr int id_media = 1001;
constexpr int id_browse_media = 1002;
constexpr int id_mods_root = 1003;
constexpr int id_browse_mods = 1004;
constexpr int id_mode = 1005;
constexpr int id_ml = 1006;
constexpr int id_video = 1007;
constexpr int id_keys = 1008;
constexpr int id_easy = 1010;
constexpr int id_normal = 1011;
constexpr int id_hard = 1012;
constexpr int id_expert = 1013;
constexpr int id_insane = 1014;
constexpr int id_variable_tempo = 1015;
constexpr int id_overwrite = 1016;
constexpr int id_generate = 1017;
constexpr int id_health = 1018;
constexpr int id_open_review = 1019;
constexpr int id_progress = 1020;
constexpr int id_log = 1021;

struct AppState final {
    HWND window{};
    HWND media{};
    HWND mods_root{};
    HWND mode{};
    HWND ml{};
    HWND video{};
    HWND keys{};
    HWND easy{};
    HWND normal{};
    HWND hard{};
    HWND expert{};
    HWND insane{};
    HWND variable_tempo{};
    HWND overwrite{};
    HWND generate{};
    HWND health{};
    HWND open_review{};
    HWND progress{};
    HWND log{};
    HFONT font{};
    std::atomic_bool running{false};
    std::wstring review_path;
};

[[nodiscard]] std::wstring window_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }
    std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}

void set_font(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

[[nodiscard]] std::wstring quote_argument(const std::wstring& value) {
    if (value.find_first_of(L" \t\"") == std::wstring::npos) {
        return value;
    }
    std::wstring output{L"\""};
    std::size_t slashes = 0U;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        if (ch == L'\"') {
            output.append(slashes * 2U + 1U, L'\\');
            output.push_back(L'\"');
            slashes = 0U;
            continue;
        }
        output.append(slashes, L'\\');
        slashes = 0U;
        output.push_back(ch);
    }
    output.append(slashes * 2U, L'\\');
    output.push_back(L'\"');
    return output;
}

[[nodiscard]] std::filesystem::path executable_directory() {
    std::wstring buffer(32'768U, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0U || length >= buffer.size()) {
        return std::filesystem::current_path();
    }
    buffer.resize(static_cast<std::size_t>(length));
    return std::filesystem::path(buffer).parent_path();
}

void append_log(AppState& state, const std::wstring_view text) {
    const int length = GetWindowTextLengthW(state.log);
    SendMessageW(state.log, EM_SETSEL, static_cast<WPARAM>(length), static_cast<LPARAM>(length));
    std::wstring line{text};
    if (!line.empty() && line.back() != L'\n') {
        line += L"\r\n";
    }
    SendMessageW(state.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(state.log, EM_SCROLLCARET, 0U, 0);
}

[[nodiscard]] std::wstring browse_media(HWND owner) {
    std::wstring buffer(32'768U, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrFilter =
        L"Audio and video\0*.mp3;*.ogg;*.wav;*.flac;*.aac;*.m4a;*.mp4;*.mkv;*.mov;*.avi;*.wmv;*.webm\0"
        L"Audio\0*.mp3;*.ogg;*.wav;*.flac;*.aac;*.m4a\0"
        L"Video\0*.mp4;*.mkv;*.mov;*.avi;*.wmv;*.webm\0"
        L"All files\0*.*\0\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&dialog)) {
        return {};
    }
    return std::wstring(buffer.c_str());
}

[[nodiscard]] std::wstring browse_folder(HWND owner) {
    BROWSEINFOW browse{};
    browse.hwndOwner = owner;
    browse.lpszTitle = L"Choose the PulseForge mods folder";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE list = SHBrowseForFolderW(&browse);
    if (list == nullptr) {
        return {};
    }
    std::wstring buffer(32'768U, L'\0');
    const BOOL ok = SHGetPathFromIDListW(list, buffer.data());
    CoTaskMemFree(list);
    return ok ? std::wstring(buffer.c_str()) : std::wstring{};
}

[[nodiscard]] std::wstring combo_value(HWND combo) {
    const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0U, 0);
    if (selection == CB_ERR) {
        return {};
    }
    const LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(selection), 0);
    if (length <= 0) {
        return {};
    }
    std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(selection), reinterpret_cast<LPARAM>(value.data()));
    value.resize(static_cast<std::size_t>(length));
    return value;
}

void set_running(AppState& state, const bool running) {
    state.running.store(running);
    EnableWindow(state.generate, running ? FALSE : TRUE);
    EnableWindow(state.health, running ? FALSE : TRUE);
    SendMessageW(state.progress, PBM_SETMARQUEE, running ? TRUE : FALSE, 35U);
    ShowWindow(state.progress, running ? SW_SHOW : SW_HIDE);
}

[[nodiscard]] bool checked(HWND control) {
    return SendMessageW(control, BM_GETCHECK, 0U, 0) == BST_CHECKED;
}

[[nodiscard]] std::wstring difficulties(const AppState& state) {
    std::wstring value;
    const auto add = [&](HWND control, const wchar_t* name) {
        if (!checked(control)) {
            return;
        }
        if (!value.empty()) {
            value.push_back(L',');
        }
        value += name;
    };
    add(state.easy, L"easy");
    add(state.normal, L"normal");
    add(state.hard, L"hard");
    add(state.expert, L"expert");
    add(state.insane, L"insane");
    return value;
}

void post_log(HWND window, std::wstring text) {
    auto* heap = new std::wstring(std::move(text));
    if (!PostMessageW(window, message_log, 0U, reinterpret_cast<LPARAM>(heap))) {
        delete heap;
    }
}

[[nodiscard]] std::wstring utf8_to_wide(const std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0
    );
    if (required <= 0) {
        return L"[non-UTF8 output]";
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), required
    );
    return result;
}

void run_child(AppState* state, std::wstring command_line, const bool health_only) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0U)) {
        post_log(state->window, L"Could not create process output pipe.");
        PostMessageW(state->window, message_complete, 1U, 0);
        return;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0U);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    const BOOL created = CreateProcessW(
        nullptr,
        mutable_command.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        executable_directory().c_str(),
        &startup,
        &process
    );
    CloseHandle(write_pipe);
    if (!created) {
        CloseHandle(read_pipe);
        post_log(state->window, L"Could not start pulseforge-autochart.exe (Win32 error "
            + std::to_wstring(GetLastError()) + L").");
        PostMessageW(state->window, message_complete, 1U, 0);
        return;
    }

    std::string pending;
    std::vector<char> buffer(8'192U);
    for (;;) {
        DWORD received = 0U;
        const BOOL ok = ReadFile(
            read_pipe,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &received,
            nullptr
        );
        if (!ok || received == 0U) {
            break;
        }
        pending.append(buffer.data(), static_cast<std::size_t>(received));
        std::size_t newline = 0U;
        while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0U, newline + 1U);
            pending.erase(0U, newline + 1U);
            if (!health_only) {
                constexpr std::string_view report_prefix = "  Review: ";
                const auto position = line.find(report_prefix);
                if (position != std::string::npos) {
                    auto path = line.substr(position + report_prefix.size());
                    while (!path.empty() && (path.back() == '\r' || path.back() == '\n')) {
                        path.pop_back();
                    }
                    auto* review = new std::wstring(utf8_to_wide(path));
                    if (!PostMessageW(
                            state->window,
                            message_review,
                            0U,
                            reinterpret_cast<LPARAM>(review))) {
                        delete review;
                    }
                }
            }
            post_log(state->window, utf8_to_wide(line));
        }
    }
    if (!pending.empty()) {
        post_log(state->window, utf8_to_wide(pending));
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1U;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(read_pipe);
    PostMessageW(state->window, message_complete, static_cast<WPARAM>(exit_code), health_only ? 1 : 0);
}

[[nodiscard]] std::wstring base_cli_command() {
    const auto cli = executable_directory() / L"pulseforge-autochart.exe";
    return quote_argument(cli.wstring());
}

void start_generate(AppState& state) {
    const auto media = window_text(state.media);
    if (media.empty()) {
        MessageBoxW(state.window, L"Choose an audio or video file first.", L"PulseForge AutoChart", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const auto diffs = difficulties(state);
    if (diffs.empty()) {
        MessageBoxW(state.window, L"Choose at least one difficulty.", L"PulseForge AutoChart", MB_OK | MB_ICONINFORMATION);
        return;
    }
    state.review_path.clear();
    SetWindowTextW(state.log, L"");
    EnableWindow(state.open_review, FALSE);

    std::wstring command = base_cli_command() + L" " + quote_argument(media);
    command += L" --mode " + combo_value(state.mode);
    command += L" --ml " + combo_value(state.ml);
    command += L" --video-analysis " + combo_value(state.video);
    command += L" --keys " + window_text(state.keys);
    command += L" --difficulties " + diffs;
    const auto mods = window_text(state.mods_root);
    if (!mods.empty()) {
        command += L" --mods-root " + quote_argument(mods);
    }
    if (checked(state.variable_tempo)) {
        command += L" --variable-tempo";
    }
    if (checked(state.overwrite)) {
        command += L" --overwrite";
    }

    append_log(state, L"Starting high-accuracy AutoChart analysis...");
    set_running(state, true);
    std::thread(run_child, &state, std::move(command), false).detach();
}

void start_health(AppState& state) {
    if (state.running.load()) {
        return;
    }
    SetWindowTextW(state.log, L"");
    append_log(state, L"Running AutoChart ML preflight...");
    set_running(state, true);
    std::wstring command = base_cli_command() + L" --ml-health";
    std::thread(run_child, &state, std::move(command), true).detach();
}

void layout(AppState& state, const int width, const int height) {
    const int margin = 22;
    const int label_width = 92;
    const int row = 34;
    const int gap = 10;
    int y = 22;
    const int content_width = (std::max)(280, width - margin * 2);
    const int edit_x = margin + label_width;
    const int browse_width = 90;
    const int edit_width = content_width - label_width - browse_width - gap;
    const auto move = [](HWND control, int x, int top, int w, int h) {
        MoveWindow(control, x, top, (std::max)(20, w), (std::max)(20, h), TRUE);
    };

    HWND label = GetDlgItem(state.window, 2001);
    move(label, margin, y + 5, label_width, 24);
    move(state.media, edit_x, y, edit_width, 28);
    move(GetDlgItem(state.window, id_browse_media), edit_x + edit_width + gap, y, browse_width, 28);
    y += row + 3;
    label = GetDlgItem(state.window, 2002);
    move(label, margin, y + 5, label_width, 24);
    move(state.mods_root, edit_x, y, edit_width, 28);
    move(GetDlgItem(state.window, id_browse_mods), edit_x + edit_width + gap, y, browse_width, 28);
    y += row + 12;

    const int third = (content_width - gap * 2) / 3;
    move(GetDlgItem(state.window, 2003), margin, y, third, 20);
    move(GetDlgItem(state.window, 2004), margin + third + gap, y, third, 20);
    move(GetDlgItem(state.window, 2005), margin + (third + gap) * 2, y, third, 20);
    y += 21;
    move(state.mode, margin, y, third, 160);
    move(state.ml, margin + third + gap, y, third, 160);
    move(state.video, margin + (third + gap) * 2, y, third, 160);
    y += 38;

    move(GetDlgItem(state.window, 2006), margin, y + 4, 40, 22);
    move(state.keys, margin + 45, y, 55, 27);
    int x = margin + 120;
    for (HWND control : {state.easy, state.normal, state.hard, state.expert, state.insane}) {
        move(control, x, y + 2, 80, 24);
        x += 82;
    }
    y += 34;
    move(state.variable_tempo, margin, y, 150, 24);
    move(state.overwrite, margin + 160, y, 155, 24);
    y += 36;

    move(state.generate, margin, y, 190, 34);
    move(state.health, margin + 200, y, 150, 34);
    move(state.open_review, margin + 360, y, 170, 34);
    y += 45;
    move(state.progress, margin, y, content_width, 20);
    y += 28;
    move(state.log, margin, y, content_width, (std::max)(110, height - y - margin));
}

HWND make_control(
    HWND parent,
    const wchar_t* klass,
    const wchar_t* text,
    DWORD style,
    int id
) {
    return CreateWindowExW(
        lstrcmpW(klass, WC_EDITW) == 0 ? WS_EX_CLIENTEDGE : 0U,
        klass,
        text,
        WS_CHILD | WS_VISIBLE | style,
        0, 0, 100, 28,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr),
        nullptr
    );
}

void create_controls(AppState& state) {
    state.font = CreateFontW(
        -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"
    );
    make_control(state.window, WC_STATICW, L"Media", SS_LEFT, 2001);
    state.media = make_control(state.window, WC_EDITW, L"", ES_AUTOHSCROLL, id_media);
    make_control(state.window, WC_BUTTONW, L"Browse...", BS_PUSHBUTTON, id_browse_media);
    make_control(state.window, WC_STATICW, L"Mods folder", SS_LEFT, 2002);
    state.mods_root = make_control(state.window, WC_EDITW, L"mods", ES_AUTOHSCROLL, id_mods_root);
    make_control(state.window, WC_BUTTONW, L"Browse...", BS_PUSHBUTTON, id_browse_mods);

    make_control(state.window, WC_STATICW, L"Analysis quality", SS_LEFT, 2003);
    make_control(state.window, WC_STATICW, L"Neural analysis", SS_LEFT, 2004);
    make_control(state.window, WC_STATICW, L"Video assist", SS_LEFT, 2005);
    state.mode = make_control(state.window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, id_mode);
    state.ml = make_control(state.window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, id_ml);
    state.video = make_control(state.window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, id_video);
    for (const wchar_t* item : {L"fast", L"accurate", L"maximum"}) SendMessageW(state.mode, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(item));
    for (const wchar_t* item : {L"off", L"auto", L"on"}) SendMessageW(state.ml, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(item));
    for (const wchar_t* item : {L"off", L"auto", L"on"}) SendMessageW(state.video, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(item));
    SendMessageW(state.mode, CB_SETCURSEL, 1U, 0);
    SendMessageW(state.ml, CB_SETCURSEL, 1U, 0);
    SendMessageW(state.video, CB_SETCURSEL, 1U, 0);

    make_control(state.window, WC_STATICW, L"Keys", SS_LEFT, 2006);
    state.keys = make_control(state.window, WC_EDITW, L"4", ES_NUMBER | ES_AUTOHSCROLL, id_keys);
    state.easy = make_control(state.window, WC_BUTTONW, L"Easy", BS_AUTOCHECKBOX, id_easy);
    state.normal = make_control(state.window, WC_BUTTONW, L"Normal", BS_AUTOCHECKBOX, id_normal);
    state.hard = make_control(state.window, WC_BUTTONW, L"Hard", BS_AUTOCHECKBOX, id_hard);
    state.expert = make_control(state.window, WC_BUTTONW, L"Expert", BS_AUTOCHECKBOX, id_expert);
    state.insane = make_control(state.window, WC_BUTTONW, L"Insane", BS_AUTOCHECKBOX, id_insane);
    SendMessageW(state.hard, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(state.expert, BM_SETCHECK, BST_CHECKED, 0);
    state.variable_tempo = make_control(state.window, WC_BUTTONW, L"Variable tempo", BS_AUTOCHECKBOX, id_variable_tempo);
    state.overwrite = make_control(state.window, WC_BUTTONW, L"Overwrite output", BS_AUTOCHECKBOX, id_overwrite);

    state.generate = make_control(state.window, WC_BUTTONW, L"Generate AutoChart", BS_DEFPUSHBUTTON, id_generate);
    state.health = make_control(state.window, WC_BUTTONW, L"ML health check", BS_PUSHBUTTON, id_health);
    state.open_review = make_control(state.window, WC_BUTTONW, L"Open review", BS_PUSHBUTTON, id_open_review);
    EnableWindow(state.open_review, FALSE);
    state.progress = make_control(state.window, PROGRESS_CLASSW, L"", PBS_MARQUEE, id_progress);
    ShowWindow(state.progress, SW_HIDE);
    state.log = make_control(
        state.window,
        WC_EDITW,
        L"Choose a media file and click Generate AutoChart.\r\nThe review page will highlight only the notes that most need human attention.",
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        id_log
    );

    for (int id : {2001,2002,2003,2004,2005,2006,id_media,id_browse_media,id_mods_root,id_browse_mods,id_mode,id_ml,id_video,id_keys,id_easy,id_normal,id_hard,id_expert,id_insane,id_variable_tempo,id_overwrite,id_generate,id_health,id_open_review,id_progress,id_log}) {
        if (HWND control = GetDlgItem(state.window, id); control != nullptr) {
            set_font(control, state.font);
        }
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        state = static_cast<AppState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) {
        return DefWindowProcW(window, message, w_param, l_param);
    }

    switch (message) {
    case WM_CREATE:
        create_controls(*state);
        return 0;
    case WM_SIZE:
        layout(*state, LOWORD(l_param), HIWORD(l_param));
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(w_param);
        if (id == id_browse_media) {
            const auto path = browse_media(window);
            if (!path.empty()) SetWindowTextW(state->media, path.c_str());
        } else if (id == id_browse_mods) {
            const auto path = browse_folder(window);
            if (!path.empty()) SetWindowTextW(state->mods_root, path.c_str());
        } else if (id == id_generate) {
            start_generate(*state);
        } else if (id == id_health) {
            start_health(*state);
        } else if (id == id_open_review && !state->review_path.empty()) {
            ShellExecuteW(window, L"open", state->review_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }
    case message_log: {
        std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(l_param));
        if (line) append_log(*state, *line);
        return 0;
    }
    case message_review: {
        std::unique_ptr<std::wstring> review(reinterpret_cast<std::wstring*>(l_param));
        if (review) {
            state->review_path = std::move(*review);
        }
        return 0;
    }
    case message_complete: {
        const DWORD exit_code = static_cast<DWORD>(w_param);
        const bool health_only = l_param != 0;
        set_running(*state, false);
        if (!health_only && exit_code == 0U && !state->review_path.empty()) {
            EnableWindow(state->open_review, TRUE);
            append_log(*state, L"Finished. Open Review to inspect the uncertainty queue.");
        } else if (exit_code == 0U) {
            append_log(*state, L"Finished successfully.");
        } else {
            append_log(*state, L"Process finished with error code " + std::to_wstring(exit_code) + L".");
        }
        return 0;
    }
    case WM_CLOSE:
        if (state->running.load()) {
            MessageBoxW(
                window,
                L"AutoChart is still running. Wait for it to finish before closing this window.",
                L"PulseForge AutoChart",
                MB_OK | MB_ICONINFORMATION
            );
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (state->font != nullptr) DeleteObject(state->font);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, w_param, l_param);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX common{};
    common.dwSize = sizeof(common);
    common.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&common);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW klass{};
    klass.cbSize = sizeof(klass);
    klass.lpfnWndProc = window_proc;
    klass.hInstance = instance;
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    klass.lpszClassName = window_class_name;
    if (RegisterClassExW(&klass) == 0U) {
        CoUninitialize();
        return 1;
    }

    AppState state;
    HWND window = CreateWindowExW(
        0U,
        window_class_name,
        L"PulseForge AutoChart Studio",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        930,
        700,
        nullptr,
        nullptr,
        instance,
        &state
    );
    if (window == nullptr) {
        CoUninitialize();
        return 1;
    }
    ShowWindow(window, show);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0U, 0U) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
