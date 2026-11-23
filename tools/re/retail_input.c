// Small Win32 input bridge for a Kingdoms window running under Proton.
// Compile with: x86_64-w64-mingw32-gcc -O2 -o /tmp/retail_input.exe tools/re/retail_input.c
// Run with the same Proton prefix as the game: proton runinprefix /tmp/retail_input.exe ...
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int number(const char *s, int *out) {
    char *end = NULL;
    long value = strtol(s, &end, 0);
    if (!s[0] || !end || *end || value < -32768 || value > 32767) return 0;
    *out = (int)value;
    return 1;
}

static int process_id(const char *s, DWORD *out) {
    char *end = NULL;
    unsigned long value = strtoul(s, &end, 0);
    if (!s[0] || !end || *end || value == 0 || value > 0xfffffffful) return 0;
    *out = (DWORD)value;
    return 1;
}

static SHORT key_for_character(char character) {
    return VkKeyScanA(character);
}

static int printable_text(const char *text) {
    if (!text[0]) return 0;
    for (const unsigned char *it = (const unsigned char *)text; *it; ++it) {
        if (*it < 0x20 || *it > 0x7e || key_for_character((char)*it) == -1)
            return 0;
    }
    return 1;
}

static void type_character(char character) {
    const SHORT translated = key_for_character(character);
    const BYTE key = LOBYTE(translated);
    const BYTE modifiers = HIBYTE(translated);

    if (modifiers & 1) keybd_event(VK_SHIFT, 0, 0, 0);
    if (modifiers & 2) keybd_event(VK_CONTROL, 0, 0, 0);
    if (modifiers & 4) keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(key, 0, 0, 0);
    Sleep(15);
    keybd_event(key, 0, KEYEVENTF_KEYUP, 0);
    if (modifiers & 4) keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    if (modifiers & 2) keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
    if (modifiers & 1) keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
    Sleep(15);
}

typedef struct {
    DWORD wanted_pid;
    HWND found;
    unsigned matches;
} WindowSearch;

static BOOL CALLBACK list_game_windows(HWND window, LPARAM opaque) {
    unsigned *matches = (unsigned *)opaque;
    char title[128] = {0};
    DWORD pid = 0;
    RECT client;
    POINT origin = {0, 0};
    if (!IsWindowVisible(window) || GetWindowTextA(window, title, sizeof(title)) <= 0 ||
        strcmp(title, "Kingdoms") != 0)
        return TRUE;
    GetWindowThreadProcessId(window, &pid);
    GetClientRect(window, &client);
    ClientToScreen(window, &origin);
    printf("pid=%lu hwnd=%p client=%ldx%ld origin=%ld,%ld foreground=%s\n",
           (unsigned long)pid, (void *)window, client.right, client.bottom,
           origin.x, origin.y, GetForegroundWindow() == window ? "yes" : "no");
    ++*matches;
    return TRUE;
}

static BOOL CALLBACK find_game_window(HWND window, LPARAM opaque) {
    WindowSearch *search = (WindowSearch *)opaque;
    char title[128] = {0};
    DWORD pid = 0;
    if (!IsWindowVisible(window) || GetWindowTextA(window, title, sizeof(title)) <= 0 ||
        strcmp(title, "Kingdoms") != 0)
        return TRUE;
    GetWindowThreadProcessId(window, &pid);
    if (search->wanted_pid && search->wanted_pid != pid) return TRUE;
    search->found = window;
    ++search->matches;
    return TRUE;
}

static void usage(void) {
    fprintf(stderr,
        "usage: retail_input [--pid PID] windows | info | close | move x y [hold_ms] | click x y [left|right] | doubleclick x y [left|right] | drag x1 y1 x2 y2 [hold_ms] | postclick x y | key vk [hold_ms] | hotkey ctrl|shift|alt vk | text ASCII | sequence\n"
        "sequence reads wait, move, click, doubleclick, key, hotkey and text commands from stdin\n");
}

static int run_sequence(HWND window, const RECT *client) {
    char line[2048];
    unsigned long line_number = 0;
    SetForegroundWindow(window);
    Sleep(100);
    while (fgets(line, sizeof(line), stdin)) {
        ++line_number;
        size_t length = strlen(line);
        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = '\0';

        char *cursor = line;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (!*cursor || *cursor == '#') continue;

        char *command = cursor;
        while (*cursor && *cursor != ' ' && *cursor != '\t') ++cursor;
        if (*cursor) *cursor++ = '\0';
        while (*cursor == ' ' || *cursor == '\t') ++cursor;

        if (strcmp(command, "wait") == 0) {
            int milliseconds;
            if (!number(cursor, &milliseconds) || milliseconds < 0 || milliseconds > 30000) {
                fprintf(stderr, "sequence line %lu: wait expects milliseconds from 0 to 30000\n", line_number);
                return 2;
            }
            Sleep((DWORD)milliseconds);
            printf("sequence line %lu: waited %d ms\n", line_number, milliseconds);
            continue;
        }

        if (strcmp(command, "text") == 0) {
            if (!printable_text(cursor)) {
                fprintf(stderr, "sequence line %lu: text expects printable ASCII\n", line_number);
                return 2;
            }
            for (const char *it = cursor; *it; ++it) type_character(*it);
            printf("sequence line %lu: typed %lu printable characters\n",
                   line_number, (unsigned long)strlen(cursor));
            continue;
        }

        char *arguments[5] = {0};
        unsigned count = 0;
        while (*cursor) {
            if (count == 5) {
                fprintf(stderr, "sequence line %lu: too many arguments\n", line_number);
                return 2;
            }
            arguments[count++] = cursor;
            while (*cursor && *cursor != ' ' && *cursor != '\t') ++cursor;
            if (*cursor) *cursor++ = '\0';
            while (*cursor == ' ' || *cursor == '\t') ++cursor;
        }

        if (strcmp(command, "move") == 0 || strcmp(command, "click") == 0 ||
            strcmp(command, "doubleclick") == 0) {
            int x, y, hold = 0;
            const int is_move = strcmp(command, "move") == 0;
            const int is_doubleclick = strcmp(command, "doubleclick") == 0;
            if (count < 2 || count > 3 ||
                !number(arguments[0], &x) || !number(arguments[1], &y) ||
                x < 0 || y < 0 || x >= client->right || y >= client->bottom ||
                (is_move && count == 3 &&
                 (!number(arguments[2], &hold) || hold < 0 || hold > 30000)) ||
                (!is_move && count == 3 && strcmp(arguments[2], "left") != 0 &&
                 strcmp(arguments[2], "right") != 0)) {
                fprintf(stderr, "sequence line %lu: invalid %s arguments\n", line_number, command);
                return 2;
            }
            POINT target = {x, y};
            if (!ClientToScreen(window, &target) || !SetCursorPos(target.x, target.y)) {
                fprintf(stderr, "sequence line %lu: failed to position cursor\n", line_number);
                return 1;
            }
            if (is_move) {
                if (hold) Sleep((DWORD)hold);
                printf("sequence line %lu: moved cursor to %d,%d\n", line_number, x, y);
                continue;
            }
            Sleep(100);
            const int right = count == 3 && strcmp(arguments[2], "right") == 0;
            const DWORD down = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
            const DWORD up = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
            const int clicks = is_doubleclick ? 2 : 1;
            UINT down_sent = 0, up_sent = 0;
            INPUT input = {0};
            input.type = INPUT_MOUSE;
            for (int i = 0; i < clicks; ++i) {
                input.mi.dwFlags = down;
                down_sent += SendInput(1, &input, sizeof(input));
                Sleep(35);
                input.mi.dwFlags = up;
                up_sent += SendInput(1, &input, sizeof(input));
                if (i + 1 < clicks) Sleep(80);
            }
            int used_fallback = 0;
            if (down_sent != (UINT)clicks || up_sent != (UINT)clicks) {
                used_fallback = 1;
                for (int i = 0; i < clicks; ++i) {
                    mouse_event(down, 0, 0, 0, 0);
                    Sleep(35);
                    mouse_event(up, 0, 0, 0, 0);
                    if (i + 1 < clicks) Sleep(80);
                }
            }
            printf("sequence line %lu: %s button=%s sent=%u/%u fallback=%s\n", line_number,
                   is_doubleclick ? "doubleclick" : "click", right ? "right" : "left",
                   down_sent, up_sent, used_fallback ? "mouse_event" : "none");
            continue;
        }

        if (strcmp(command, "key") == 0) {
            int key, hold = 100;
            if ((count != 1 && count != 2) || !number(arguments[0], &key) ||
                key < 0 || key > 255 ||
                (count == 2 && (!number(arguments[1], &hold) || hold < 0 || hold > 30000))) {
                fprintf(stderr, "sequence line %lu: key expects vk [hold_ms]\n", line_number);
                return 2;
            }
            keybd_event((BYTE)key, 0, 0, 0);
            Sleep((DWORD)hold);
            keybd_event((BYTE)key, 0, KEYEVENTF_KEYUP, 0);
            printf("sequence line %lu: sent virtual key %d for %d ms\n",
                   line_number, key, hold);
            continue;
        }

        if (strcmp(command, "hotkey") == 0) {
            int key;
            BYTE modifier;
            if (count != 2 || !number(arguments[1], &key) || key < 0 || key > 255) {
                fprintf(stderr, "sequence line %lu: hotkey expects ctrl|shift|alt vk\n", line_number);
                return 2;
            }
            if (strcmp(arguments[0], "ctrl") == 0) modifier = VK_CONTROL;
            else if (strcmp(arguments[0], "shift") == 0) modifier = VK_SHIFT;
            else if (strcmp(arguments[0], "alt") == 0) modifier = VK_MENU;
            else {
                fprintf(stderr, "sequence line %lu: unknown hotkey modifier\n", line_number);
                return 2;
            }
            keybd_event(modifier, 0, 0, 0);
            Sleep(35);
            keybd_event((BYTE)key, 0, 0, 0);
            Sleep(35);
            keybd_event((BYTE)key, 0, KEYEVENTF_KEYUP, 0);
            Sleep(35);
            keybd_event(modifier, 0, KEYEVENTF_KEYUP, 0);
            printf("sequence line %lu: sent hotkey modifier=%s virtual_key=%d\n",
                   line_number, arguments[0], key);
            continue;
        }

        fprintf(stderr, "sequence line %lu: unknown command '%s'\n", line_number, command);
        return 2;
    }
    if (ferror(stdin)) {
        fprintf(stderr, "failed while reading input sequence\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    SetProcessDPIAware();
    DWORD wanted_pid = 0, window_pid = 0;
    int argbase = 1;
    if (argc >= 3 && strcmp(argv[1], "--pid") == 0) {
        if (!process_id(argv[2], &wanted_pid)) { usage(); return 2; }
        argbase = 3;
    }
    if (argc <= argbase) { usage(); return 2; }

    if (argc == argbase + 1 && strcmp(argv[argbase], "windows") == 0) {
        unsigned matches = 0;
        EnumWindows(list_game_windows, (LPARAM)&matches);
        if (!matches) {
            fprintf(stderr, "Kingdoms window not found\n");
            return 1;
        }
        return 0;
    }

    WindowSearch search = {wanted_pid, NULL, 0};
    EnumWindows(find_game_window, (LPARAM)&search);
    if (search.matches == 0) {
        fprintf(stderr, "Kingdoms window not found\n");
        return 1;
    }
    if (search.matches != 1) {
        fprintf(stderr, "found %u Kingdoms windows; choose one with --pid PID\n", search.matches);
        return 1;
    }
    HWND window = search.found;
    GetWindowThreadProcessId(window, &window_pid);
    const int nargs = argc - argbase;
    const char *command = argv[argbase];

    RECT client, bounds;
    POINT origin = {0, 0}, cursor = {0, 0};
    GetClientRect(window, &client);
    GetWindowRect(window, &bounds);
    ClientToScreen(window, &origin);
    GetCursorPos(&cursor);

    if (nargs == 1 && strcmp(command, "info") == 0) {
        HWND foreground = GetForegroundWindow();
        printf("pid=%lu hwnd=%p client=%ldx%ld window=%ld,%ld..%ld,%ld origin=%ld,%ld cursor=%ld,%ld dpi=%u foreground=%s\n",
               (unsigned long)window_pid, (void *)window,
               client.right, client.bottom, bounds.left, bounds.top, bounds.right, bounds.bottom,
               origin.x, origin.y, cursor.x, cursor.y, GetDpiForWindow(window),
               foreground == window ? "Kingdoms" : "other");
        return 0;
    }
    if (nargs == 1 && strcmp(command, "close") == 0) {
        return PostMessageA(window, WM_CLOSE, 0, 0) ? 0 : 1;
    }
    if (nargs == 1 && strcmp(command, "sequence") == 0)
        return run_sequence(window, &client);

    if (nargs == 2 && strcmp(command, "text") == 0) {
        const char *text = argv[argbase + 1];
        if (!printable_text(text)) {
            fprintf(stderr, "text expects printable ASCII supported by the active keyboard layout\n");
            return 2;
        }
        SetForegroundWindow(window);
        Sleep(100);
        for (const char *it = text; *it; ++it) type_character(*it);
        printf("typed %lu printable characters\n", (unsigned long)strlen(text));
        return 0;
    }

    if (nargs >= 3 && (strcmp(command, "move") == 0 ||
                       strcmp(command, "click") == 0 ||
                       strcmp(command, "doubleclick") == 0 ||
                       strcmp(command, "postclick") == 0)) {
        int x, y, hold = 0;
        const int is_move = strcmp(command, "move") == 0;
        const int is_click = strcmp(command, "click") == 0;
        const int is_doubleclick = strcmp(command, "doubleclick") == 0;
        const int is_postclick = strcmp(command, "postclick") == 0;
        if (!number(argv[argbase + 1], &x) || !number(argv[argbase + 2], &y) ||
            x < 0 || y < 0 || x >= client.right || y >= client.bottom ||
            (is_move && nargs > 4) ||
            (is_move && nargs == 4 &&
             (!number(argv[argbase + 3], &hold) || hold < 0 || hold > 30000)) ||
            ((is_click || is_doubleclick) && nargs > 4) ||
            ((is_click || is_doubleclick) && nargs == 4 && strcmp(argv[argbase + 3], "left") != 0 &&
             strcmp(argv[argbase + 3], "right") != 0) ||
            (is_postclick && nargs != 3)) {
            usage(); return 2;
        }
        if (is_postclick) {
            LPARAM point = MAKELPARAM(x, y);
            PostMessageA(window, WM_MOUSEMOVE, 0, point);
            Sleep(30);
            PostMessageA(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
            Sleep(60);
            PostMessageA(window, WM_LBUTTONUP, 0, point);
            printf("posted client click %d,%d\n", x, y);
            return 0;
        }

        SetForegroundWindow(window);
        POINT target = {x, y};
        if (!ClientToScreen(window, &target) || !SetCursorPos(target.x, target.y)) {
            fprintf(stderr, "could not move cursor to client %d,%d\n", x, y);
            return 1;
        }
        Sleep(100);
        GetCursorPos(&cursor);
        POINT actual = cursor;
        ScreenToClient(window, &actual);
        printf("cursor client requested=%d,%d actual=%ld,%ld screen=%ld,%ld\n",
               x, y, actual.x, actual.y, cursor.x, cursor.y);
        if (is_move) {
            if (hold) Sleep((DWORD)hold);
            return 0;
        }

        const int right = nargs == 4 && strcmp(argv[argbase + 3], "right") == 0;
        const DWORD down = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
        const DWORD up = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
        INPUT input = {0};
        input.type = INPUT_MOUSE;
        UINT down_sent = 0, up_sent = 0;
        const int click_count = is_doubleclick ? 2 : 1;
        for (int i = 0; i < click_count; ++i) {
            input.mi.dwFlags = down;
            down_sent += SendInput(1, &input, sizeof(input));
            Sleep(35);
            input.mi.dwFlags = up;
            up_sent += SendInput(1, &input, sizeof(input));
            if (i + 1 < click_count) Sleep(80);
        }
        if (down_sent != (UINT)click_count || up_sent != (UINT)click_count) {
            for (int i = 0; i < click_count; ++i) {
                mouse_event(down, 0, 0, 0, 0);
                Sleep(35);
                mouse_event(up, 0, 0, 0, 0);
                if (i + 1 < click_count) Sleep(80);
            }
        }
        printf("%s button=%s sent=%u/%u\n", is_doubleclick ? "doubleclick" : "click",
               right ? "right" : "left", down_sent, up_sent);
        return 0;
    }

    if ((nargs == 5 || nargs == 6) && strcmp(command, "drag") == 0) {
        int x1, y1, x2, y2, hold = 100;
        if (!number(argv[argbase + 1], &x1) || !number(argv[argbase + 2], &y1) ||
            !number(argv[argbase + 3], &x2) || !number(argv[argbase + 4], &y2) ||
            (nargs == 6 && !number(argv[argbase + 5], &hold)) ||
            x1 < 0 || y1 < 0 || x2 < 0 || y2 < 0 ||
            x1 >= client.right || x2 >= client.right ||
            y1 >= client.bottom || y2 >= client.bottom || hold < 0 || hold > 30000) {
            usage(); return 2;
        }
        SetForegroundWindow(window);
        POINT start = {x1, y1}, finish = {x2, y2};
        if (!ClientToScreen(window, &start) || !ClientToScreen(window, &finish) ||
            !SetCursorPos(start.x, start.y)) {
            fprintf(stderr, "could not move cursor to drag start %d,%d\n", x1, y1);
            return 1;
        }
        Sleep(100);
        INPUT input = {0};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        if (SendInput(1, &input, sizeof(input)) != 1) {
            fprintf(stderr, "could not press mouse button for drag\n");
            return 1;
        }
        for (int step = 1; step <= 20; ++step) {
            const int x = start.x + (finish.x - start.x) * step / 20;
            const int y = start.y + (finish.y - start.y) * step / 20;
            if (!SetCursorPos(x, y)) {
                mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                fprintf(stderr, "could not move cursor during drag\n");
                return 1;
            }
            Sleep(10);
        }
        if (hold) Sleep((DWORD)hold);
        input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        if (SendInput(1, &input, sizeof(input)) != 1) {
            mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            fprintf(stderr, "could not release mouse button after drag\n");
            return 1;
        }
        printf("dragged client %d,%d to %d,%d\n", x1, y1, x2, y2);
        return 0;
    }

    if (nargs >= 2 && strcmp(command, "key") == 0) {
        int key, hold = 100;
        if (!number(argv[argbase + 1], &key) || key < 0 || key > 255 || nargs > 3 ||
            (nargs == 3 && !number(argv[argbase + 2], &hold)) || hold < 0 || hold > 30000) {
            usage(); return 2;
        }
        SetForegroundWindow(window);
        keybd_event((BYTE)key, 0, 0, 0);
        Sleep((DWORD)hold);
        keybd_event((BYTE)key, 0, KEYEVENTF_KEYUP, 0);
        printf("sent virtual key %d for %d ms\n", key, hold);
        return 0;
    }

    if (nargs == 3 && strcmp(command, "hotkey") == 0) {
        int key;
        BYTE modifier;
        if (strcmp(argv[argbase + 1], "ctrl") == 0) modifier = VK_CONTROL;
        else if (strcmp(argv[argbase + 1], "shift") == 0) modifier = VK_SHIFT;
        else if (strcmp(argv[argbase + 1], "alt") == 0) modifier = VK_MENU;
        else { usage(); return 2; }
        if (!number(argv[argbase + 2], &key) || key < 0 || key > 255) {
            usage(); return 2;
        }
        SetForegroundWindow(window);
        keybd_event(modifier, 0, 0, 0);
        Sleep(35);
        keybd_event((BYTE)key, 0, 0, 0);
        Sleep(35);
        keybd_event((BYTE)key, 0, KEYEVENTF_KEYUP, 0);
        Sleep(35);
        keybd_event(modifier, 0, KEYEVENTF_KEYUP, 0);
        printf("sent hotkey modifier=%s virtual_key=%d\n", argv[argbase + 1], key);
        return 0;
    }

    usage();
    return 2;
}
