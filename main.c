#include <windows.h>
#include <wininet.h>
#include <stdio.h>
#include <string.h>

#define ID_BUTTON_SEND    101
#define ID_EDIT_INPUT     102
#define ID_EDIT_OUTPUT    103
#define ID_LINK_GETKEY    104

HWND hInputBox, hOutputBox, hSendButton, hLinkLabel;

HBRUSH hMainBgBrush;
HBRUSH hEditBgBrush;
HFONT hCustomFont, hLinkFont;
HICON hAppIcon;

// Global state variables
char savedApiKey[128] = {0};
BOOL isKeyValidated = FALSE;
char configFilePath[MAX_PATH] = {0};

// Custom layout definitions to override default behaviors cleanly
WNDPROC OldEditProc;

// Intercept clicks on the link to change cursor to a hand
LRESULT CALLBACK LinkStaticProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SETCURSOR) {
        SetCursor(LoadCursor(NULL, IDC_HAND));
        return TRUE;
    }
    return CallWindowProc(OldEditProc, hwnd, msg, wp, lp);
}

void ResolveConfigPath() {
    GetModuleFileNameA(NULL, configFilePath, MAX_PATH);
    char* lastBackslash = strrchr(configFilePath, '\\');
    if (lastBackslash != NULL) {
        *(lastBackslash + 1) = '\0';
        strcat(configFilePath, ".purebox_key");
    }
}

void LoadCachedKey() {
    FILE* file = fopen(configFilePath, "r");
    if (file) {
        if (fgets(savedApiKey, sizeof(savedApiKey), file)) {
            // Strip newline configurations
            savedApiKey[strcspn(savedApiKey, "\r\n")] = 0;
            if (strlen(savedApiKey) > 5) {
                isKeyValidated = TRUE; 
            }
        }
        fclose(file);
    }
}

void SaveValidKey(const char* key) {
    FILE* file = fopen(configFilePath, "w");
    if (file) {
        fputs(key, file);
        fclose(file);
    }
}

void ExtractPureBoxText(const char* json, char* outText, DWORD maxLen) {
    if (strstr(json, "\"error\"") != NULL) {
        if (strstr(json, "API_KEY_INVALID") != NULL || strstr(json, "400") != NULL) {
            snprintf(outText, maxLen, "LOGIN_FAILED");
            return;
        }
        if (strstr(json, "429") != NULL || strstr(json, "RESOURCE_EXHAUSTED") != NULL) {
            snprintf(outText, maxLen, "PureBox Error: Rate limit exceeded (20 requests per minute max). Please wait a moment and try again.");
            return;
        }
        snprintf(outText, maxLen, "PureBox Error: The server returned an error. Please check your account quota status.");
        return;
    }

    const char* target = "\"text\": \"";
    const char* start = strstr(json, target);
    if (!start) {
        snprintf(outText, maxLen, "PureBox Error: Received an invalid or empty response from the server.");
        return;
    }
    start += strlen(target);
    
    DWORD i = 0;
    while (*start && *start != '"' && i < maxLen - 1) {
        if (*start == '\\' && *(start + 1) == 'n') {
            outText[i++] = '\r';
            outText[i++] = '\n';
            start += 2;
        } else {
            outText[i++] = *start;
            start++;
        }
    }
    outText[i] = '\0';
}

void AskPureBox(const char* prompt, const char* apiKey, char* outputBuffer, DWORD bufferSize) {
    HINTERNET hInternet = InternetOpenA("PureBox_Client", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInternet) {
        snprintf(outputBuffer, bufferSize, "Error: Failed to open internet handles.");
        return;
    }

    HINTERNET hConnect = InternetConnectA(hInternet, "generativelanguage.googleapis.com", INTERNET_DEFAULT_HTTPS_PORT, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConnect) {
        snprintf(outputBuffer, bufferSize, "Error: Connection to Google servers failed.");
        InternetCloseHandle(hInternet);
        return;
    }

    char urlPath[512];
    snprintf(urlPath, sizeof(urlPath), "/v1beta/models/gemini-2.5-flash:generateContent?key=%s", apiKey);

    DWORD flags = INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD;
    HINTERNET hRequest = HttpOpenRequestA(hConnect, "POST", urlPath, NULL, NULL, NULL, flags, 0);
    if (!hRequest) {
        snprintf(outputBuffer, bufferSize, "Error: Failed to format HTTP request.");
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInternet);
        return;
    }

    const char* headers = "Content-Type: application/json\r\n";

    char cleanPrompt[512] = {0};
    int j = 0;
    for (int i = 0; prompt[i] != '\0' && j < 500; i++) {
        if (prompt[i] == '"') {
            cleanPrompt[j++] = '\\';
        }
        cleanPrompt[j++] = prompt[i];
    }

    char jsonPayload[4096];
    snprintf(jsonPayload, sizeof(jsonPayload),
             "{"
             "\"systemInstruction\": {\"parts\": [{\"text\": \"Your name is PureBox, an AI assistant made by Azure. Answer user queries directly and concisely without any introductory filler or greetings unless they explicitly ask who you are or who made you.\"}]},"
             "\"contents\": [{\"parts\": [{\"text\": \"%s\"}]}]"
             "}",
             cleanPrompt);

    if (HttpSendRequestA(hRequest, headers, strlen(headers), jsonPayload, strlen(jsonPayload))) {
        static char tempBuffer[16384]; 
        memset(tempBuffer, 0, sizeof(tempBuffer));
        DWORD bytesRead = 0;
        InternetReadFile(hRequest, tempBuffer, sizeof(tempBuffer) - 1, &bytesRead);
        
        ExtractPureBoxText(tempBuffer, outputBuffer, bufferSize);
    } else {
        snprintf(outputBuffer, bufferSize, "Error: Failed to receive data from PureBox backend.");
    }

    InternetCloseHandle(hRequest);
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);
}

// Function to reset the view positions of controls depending on validation state
void UpdateControlLayout(HWND hwnd) {
    RECT rect;
    GetClientRect(hwnd, &rect);
    int width = rect.right;
    int height = rect.bottom;

    int padding = 20;

    if (!isKeyValidated) {
        // Show components optimized for Login Mode
        ShowWindow(hLinkLabel, SW_SHOW);
        SetWindowTextA(hSendButton, "Login");
        SetWindowTextA(hOutputBox, "PureBox Secure Access Protection\r\n\r\nAn API key is required to connect to the backend models.\r\n\r\nPlease paste your Gemini API Key into the small baseline text field below and click 'Login' to unlock the console environment.");

        int outputWidth = width - (padding * 2);
        int outputHeight = height - (padding * 4) - 32 - 20; // Extra room for the link

        MoveWindow(hOutputBox, padding, padding, outputWidth, outputHeight, TRUE);
        MoveWindow(hLinkLabel, padding, padding + outputHeight + 8, outputWidth, 20, TRUE);
        MoveWindow(hInputBox, padding, padding * 2 + outputHeight + 20, width - (padding * 3) - 100, 32, TRUE);
        MoveWindow(hSendButton, width - padding - 100, padding * 2 + outputHeight + 20, 100, 32, TRUE);
    } else {
        // Hide link and stretch for Chat Mode
        ShowWindow(hLinkLabel, SW_HIDE);
        SetWindowTextA(hSendButton, "Send");
        
        int outputWidth = width - (padding * 2);
        int outputHeight = height - (padding * 3) - 32;

        MoveWindow(hOutputBox, padding, padding, outputWidth, outputHeight, TRUE);
        MoveWindow(hInputBox, padding, padding * 2 + outputHeight, width - (padding * 3) - 100, 32, TRUE);
        MoveWindow(hSendButton, width - padding - 100, padding * 2 + outputHeight, 100, 32, TRUE);
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            hCustomFont = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, 
                                      ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

            hLinkFont = CreateFontA(15, 0, 0, 0, FW_BOLD, FALSE, TRUE, FALSE, 
                                    ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

            hOutputBox = CreateWindowExA(0, "EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                0, 0, 0, 0, hwnd, (HMENU)ID_EDIT_OUTPUT, NULL, NULL);
            SendMessage(hOutputBox, WM_SETFONT, (WPARAM)hCustomFont, TRUE);

            hInputBox = CreateWindowExA(0, "EDIT", "",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                0, 0, 0, 0, hwnd, (HMENU)ID_EDIT_INPUT, NULL, NULL);
            SendMessage(hInputBox, WM_SETFONT, (WPARAM)hCustomFont, TRUE);

            hSendButton = CreateWindowA("BUTTON", "Send",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                0, 0, 0, 0, hwnd, (HMENU)ID_BUTTON_SEND, NULL, NULL);
            SendMessage(hSendButton, WM_SETFONT, (WPARAM)hCustomFont, TRUE);

            // Clickable URL block
            hLinkLabel = CreateWindowExA(0, "STATIC", "Get your Gemini API Key from Google AI Studio",
                WS_CHILD | WS_VISIBLE | SS_NOTIFY,
                0, 0, 0, 0, hwnd, (HMENU)ID_LINK_GETKEY, NULL, NULL);
            SendMessage(hLinkLabel, WM_SETFONT, (WPARAM)hLinkFont, TRUE);
            OldEditProc = (WNDPROC)SetWindowLongPtrA(hLinkLabel, GWLP_WNDPROC, (LONG_PTR)LinkStaticProc);

            // Run structural positions layout pass
            UpdateControlLayout(hwnd);

            if (isKeyValidated) {
                SetWindowTextA(hOutputBox, "Welcome back to PureBox. Connection secured via cached key profile.\r\nType a prompt below...");
            }
            return 0;
        }

        case WM_SIZE: {
            UpdateControlLayout(hwnd);
            return 0;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            if ((HWND)lParam == hLinkLabel) {
                SetTextColor(hdc, RGB(0, 162, 232)); // Neon Cyan/Blue text styling for link accent
                SetBkColor(hdc, RGB(28, 28, 28));    // Matches main panel window background color
                return (INT_PTR)hMainBgBrush;
            }
            SetTextColor(hdc, RGB(240, 240, 240));
            SetBkColor(hdc, RGB(35, 35, 38));
            return (INT_PTR)hEditBgBrush;
        }

        case WM_COMMAND: {
            // Monitor link clicks
            if (LOWORD(wParam) == ID_LINK_GETKEY && HIWORD(wParam) == STN_CLICKED) {
                ShellExecuteA(NULL, "open", "https://aistudio.google.com/api-keys", NULL, NULL, SW_SHOWNORMAL);
                return 0;
            }

            if (LOWORD(wParam) == ID_BUTTON_SEND) {
                char userBuffer[256];
                static char aiResponse[8192];

                GetWindowTextA(hInputBox, userBuffer, sizeof(userBuffer));

                if (strlen(userBuffer) > 0) {
                    if (!isKeyValidated) {
                        // Execution of the authentication handshake process
                        SetWindowTextA(hOutputBox, "Authenticating API Key with Google Cloud...");
                        UpdateWindow(hOutputBox);

                        AskPureBox("Ping test", userBuffer, aiResponse, sizeof(aiResponse));

                        if (strcmp(aiResponse, "LOGIN_FAILED") == 0) {
                            SetWindowTextA(hOutputBox, "Authentication Failed!\r\n\r\nThe key provided was rejected by the server module. Please double-check your string syntax data layout and try again.");
                            SetWindowTextA(hInputBox, "");
                        } else {
                            // Key verified! Save token and transition window states
                            strncpy(savedApiKey, userBuffer, sizeof(savedApiKey) - 1);
                            isKeyValidated = TRUE;
                            SaveValidKey(savedApiKey);
                            
                            SetWindowTextA(hInputBox, "");
                            UpdateControlLayout(hwnd);
                            SetWindowTextA(hOutputBox, "Authentication Successful! Connection pipeline established.\r\n\r\nWelcome to PureBox. Type a prompt below...");
                        }
                    } else {
                        // Standard chat mode operation pass using saved verification token
                        SetWindowTextA(hOutputBox, "PureBox is thinking...");
                        UpdateWindow(hOutputBox); 
                        
                        AskPureBox(userBuffer, savedApiKey, aiResponse, sizeof(aiResponse));
                        
                        SetWindowTextA(hOutputBox, aiResponse);
                        SetWindowTextA(hInputBox, ""); 
                    }
                }
            }
            return 0;
        }

        case WM_DESTROY:
            DeleteObject(hMainBgBrush);
            DeleteObject(hEditBgBrush);
            DeleteObject(hCustomFont);
            DeleteObject(hLinkFont);
            if (hAppIcon) DestroyIcon(hAppIcon); 
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    const char CLASS_NAME[] = "PureBox_WindowClass";
    
    hMainBgBrush = CreateSolidBrush(RGB(28, 28, 28));  
    hEditBgBrush = CreateSolidBrush(RGB(35, 35, 38));  

    // Locate cache parameters before initializing graphics structures
    ResolveConfigPath();
    LoadCachedKey();

    char iconFullPath[MAX_PATH];
    GetModuleFileNameA(NULL, iconFullPath, MAX_PATH);
    
    char* lastBackslash = strrchr(iconFullPath, '\\');
    if (lastBackslash != NULL) {
        *(lastBackslash + 1) = '\0';
        strcat(iconFullPath, "Logo.ico");
    }

    hAppIcon = (HICON)LoadImageA(NULL, iconFullPath, IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = hMainBgBrush; 
    
    if (hAppIcon) {
        wc.hIcon = hAppIcon;
    }

    RegisterClassA(&wc);
    
    HWND hwnd = CreateWindowExA(0, CLASS_NAME, "PureBox", 
        WS_OVERLAPPEDWINDOW, 
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 380, NULL, NULL, hInstance, NULL);
        
    if (hwnd == NULL) return 0;
    
    if (hAppIcon) {
        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hAppIcon);
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hAppIcon);
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    
    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}