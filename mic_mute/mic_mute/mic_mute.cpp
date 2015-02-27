// mic_mute.cpp : Defines the entry point for the application.
//

#include "stdafx.h"
#include "mic_mute.h"
#include "./../key_hook/key_hook.h"

_SetShortCut	SetShortcut;
_SetEnabled		SetEnabled;
_GetShortCut	GetShortcut;
_SetMode		SetMode;
_GetVkCode		GetVkCode;

#define MAX_LOADSTRING 100

// Global Variables:
HINSTANCE hInst = NULL;								// current instance
TCHAR szTitle[MAX_LOADSTRING] = _T("");					// The title bar text
TCHAR szWindowClass[MAX_LOADSTRING] = _T("");			// the main window class name
DWORD SavedVolume = 0;
UINT TrayMsg = 0;
HMENU TrayMenu = NULL;
INT ShowNotifications = 0;
INT SoundSignal = 0;
HICON IconBlack = 0, IconRed = 0;

WNDPROC shortcut_edit_proc = NULL;
UINT prev_code = 0;

TCHAR szMicOnSound[MAX_PATH];
TCHAR szMicOffSound[MAX_PATH];
TCHAR szMediaPath[MAX_PATH], szMicOnDefault[1024], szMicOffDefault[1024];

HMENU DevicesMenu = NULL;
#define DEVICE_FIRST_ID 50000
#define DEVICE_LAST_ID 50999

// Forward declarations of functions included in this code module:
ATOM				MyRegisterClass(HINSTANCE hInstance);
BOOL				InitInstance(HINSTANCE, int);
LRESULT CALLBACK	WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK	About(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK	SetupShortcut(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK	SelectAudioFiles(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK	ShortcutEditProc(HWND, UINT, WPARAM, LPARAM);
VOID				MuteToggle(HWND hWnd);
VOID				StartMutedToggle(HWND hWnd);
VOID				TransmitterToggle(HWND hWnd);
VOID				ShowNotificationsToggle(HWND hWnd);
VOID				SoundSignalToggle(HWND hWnd);
VOID				AutorunToggle(HWND hWnd);
BOOL CALLBACK		EnumCallback(LPGUID guid, LPCTSTR descr, LPCTSTR modname, LPVOID ctx);
VOID				ReadIni(VOID);
VOID				WriteIni(VOID);
VOID				SetIcon(HWND, HICON);
VOID				SetIcon(HMENU, UINT, UINT, LPCTSTR module = NULL);
HBITMAP				Icon2Bitmap(HICON, UINT);

CMixer mixer_mic_in(MIXERLINE_COMPONENTTYPE_SRC_MICROPHONE, CMixer::Record);

typedef BOOL (CALLBACK *LPDSENUMCALLBACKW)(LPGUID, LPCWSTR, LPCWSTR, LPVOID);
typedef HRESULT (WINAPI * DirectSoundCaptureEnumerateW_t)(__in LPDSENUMCALLBACKW, __in_opt LPVOID);

HANDLE HookEvent = NULL;
HANDLE StopEvent = NULL;
HANDLE Thread = NULL;
HWND AppHWnd = NULL;
BOOL StartMuted = FALSE;
BOOL Autorun = FALSE;
UINT SelectedDevice = 0;
HANDLE SingleControl = NULL;
int MicMode = MIC_MODE_STANDART;
bool IsMuted = false;

bool restart_with_admin_rights = false;

BOOL CALLBACK EnumCallback(LPGUID guid, LPCTSTR descr, LPCTSTR modname, LPVOID ctx)
{
	HMENU menu = (HMENU)ctx;
	MENUITEMINFO mii;
	for (UINT _idx = 0; _idx < CMixer::DevCount(); _idx++)
	{
		MIXERCAPS _caps;
		if FAILED(CMixer::GetCaps(_idx, &_caps)) continue;
		if (_caps.cDestinations == 0) continue;
		if (StrStr(descr, _caps.szPname) == NULL) continue;
		if (mixer_mic_in.SelectDevice(_idx) == false) continue;
		mii.cbSize = sizeof(MENUITEMINFO);
		mii.fMask = MIIM_STRING | MIIM_ID | MIIM_FTYPE;
		mii.fType = MFT_RADIOCHECK | MFT_STRING;
		mii.wID = DEVICE_FIRST_ID + _idx;
		mii.hbmpChecked = NULL;
		StringCchLength(descr, STRSAFE_MAX_CCH, & mii.cch);
		mii.dwTypeData = new TCHAR[mii.cch + 1];
		StringCchCopy(mii.dwTypeData, mii.cch + 1, descr);
		InsertMenuItem(menu, _idx, TRUE, &mii);
		if (SelectedDevice == (UINT)-1)
		{
			CheckMenuItem(menu, DEVICE_FIRST_ID + _idx, MF_CHECKED);
			SelectedDevice = _idx;
		}
	}
	return TRUE;
}

DWORD WINAPI ThreadProc( LPVOID lpParam ) 
{
	while (1)
	{
		if (WaitForSingleObject(HookEvent, 0) == WAIT_OBJECT_0)
		{
			ResetEvent(HookEvent);
			MuteToggle(HWND(lpParam));
		}
		else
		if (IsMuted)
		{
			if (mixer_mic_in.GetVolume() > 0)
			{
				mixer_mic_in.SetVolume(0);
			}
			if (!mixer_mic_in.GetMute())
			{
				mixer_mic_in.SetMute(TRUE);
			}
		}
		if (WaitForSingleObject(StopEvent, 0) == WAIT_OBJECT_0)
		{
			break;
		}
		Sleep(100);
	}
	return 0;
}

int APIENTRY _tWinMain(HINSTANCE hInstance,
                     HINSTANCE hPrevInstance,
                     LPTSTR    lpCmdLine,
                     int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);

 	hInst = hInstance;

	SingleControl = CreateMutex(NULL, FALSE, _T("SingleControl!"));
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		CloseHandle(SingleControl);
		return 0;
	}

//	InitCommonControls();

	HMODULE dsound = LoadLibrary(_T("dsound.dll"));
	DirectSoundCaptureEnumerateW_t DirectSoundCaptureEnumerate = (DirectSoundCaptureEnumerateW_t)GetProcAddress(dsound, "DirectSoundCaptureEnumerateW");

	MSG msg;
	HACCEL hAccelTable;
	HOOKPROC hLLKeybProc; 
	static HINSTANCE hinstDLL; 
	static HHOOK hhook; 

	hinstDLL = LoadLibrary(_T("key_hook.dll")); 

	if (!hinstDLL)
	{
		TCHAR warn[1024];
		LoadString(hInstance, IDS_NOHOOK, warn, sizeof(warn) / sizeof(warn[0]));
		TCHAR caption[1024];
		LoadString(hInstance, IDS_ERRORCAPTION, caption, sizeof(caption) / sizeof(caption[0]));
		MessageBox(NULL, warn, caption, MB_OK | MB_ICONERROR);
		return 0;
	}

	hLLKeybProc = (HOOKPROC)GetProcAddress(hinstDLL, "_LowLevelKeyboardProc@12"); 
	SetShortcut = (_SetShortCut)GetProcAddress(hinstDLL, "_SetShortcut@12");
	GetShortcut = (_GetShortCut)GetProcAddress(hinstDLL, "_GetShortcut@12");
	SetEnabled = (_SetEnabled)GetProcAddress(hinstDLL, "_SetEnabled@4");
	SetMode = (_SetMode)GetProcAddress(hinstDLL, "_SetMode@4");
	GetVkCode = (_GetVkCode)GetProcAddress(hinstDLL, "_GetVkCode@0");

	if (!hLLKeybProc || !SetShortcut || !GetShortcut || !SetEnabled || !SetMode || !GetVkCode)
	{
		TCHAR warn[1024];
		LoadString(hInstance, IDS_BADHOOK, warn, sizeof(warn) / sizeof(warn[0]));
		TCHAR caption[1024];
		LoadString(hInstance, IDS_ERRORCAPTION, caption, sizeof(caption) / sizeof(caption[0]));
		MessageBox(NULL, warn, caption, MB_OK | MB_ICONERROR);
		return 0;
	}

	hhook = SetWindowsHookEx(WH_KEYBOARD_LL,hLLKeybProc,hinstDLL,NULL); 
	HookEvent = CreateEvent(NULL, TRUE, FALSE, _T("Hooked!"));

	IconBlack = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_MIC_MUTE));
	IconRed = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_MIC_MUTE_RED));

	GetModuleFileName(NULL, szMediaPath, MAX_PATH);
	PathRemoveFileSpec(szMediaPath);
	StringCchCat(szMediaPath, MAX_PATH, _T("\\"));
	StringCchCopy(szMicOnDefault, 1024, szMediaPath);
	StringCchCat(szMicOnDefault, MAX_PATH, _T("beep750.wav"));
	StringCchCopy(szMicOffDefault, 1024, szMediaPath);
	StringCchCat(szMicOffDefault, MAX_PATH, _T("beep300.wav"));

	ReadIni();

	// Initialize global strings
	LoadString(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
	LoadString(hInstance, IDC_MIC_MUTE, szWindowClass, MAX_LOADSTRING);
	MyRegisterClass(hInstance);

	// Perform application initialization:
	if (!InitInstance (hInstance, SW_HIDE))
	{
		return 0;
	}

	Thread = CreateThread( 
		NULL,              // default security attributes
		0,                 // use default stack size  
		ThreadProc,        // thread function 
		AppHWnd,             // argument to thread function 
		0,                 // use default creation flags 
		0);   // returns the thread identifier 
	StopEvent = CreateEvent(NULL, TRUE, FALSE, _T("Stop!"));

	TrayMsg = RegisterWindowMessage(_T("Tray!"));

	hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_MIC_MUTE));

	DevicesMenu = CreateMenu();
	CoInitialize(NULL);
	DirectSoundCaptureEnumerate(& EnumCallback, DevicesMenu);

	mixer_mic_in.SelectDevice(SelectedDevice);

	NOTIFYICONDATA nid;
	ZeroMemory(&nid, sizeof(nid));
	nid.cbSize = NOTIFYICONDATA_V2_SIZE;
	nid.hWnd = AppHWnd;
	nid.uID = 1;
	nid.hIcon = (StartMuted || mixer_mic_in.GetVolume() == 0 || mixer_mic_in.GetMute()) ? IconBlack : IconRed;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid.uCallbackMessage = TrayMsg;
	nid.uTimeout = 5000;
	nid.dwInfoFlags = NIIF_NOSOUND;
	if (ShowNotifications)
	{
		nid.uFlags |= NIF_INFO;
		nid.dwInfoFlags |= NIIF_USER;
	}
	TCHAR _tip[] = _T("MicMute");
	StringCchCopy(nid.szTip, 128, _tip);
	StringCchCopy(nid.szInfoTitle, 64, _tip);
	TCHAR _tooltip_text[1024];
	LoadString(hInstance, IDS_STARTED, _tooltip_text, sizeof(_tooltip_text) / sizeof(_tooltip_text[0]));
	StringCchCopy(nid.szInfo, 256, _tooltip_text);
	Shell_NotifyIcon(NIM_ADD, &nid);

	HMENU hmenu = LoadMenu(GetModuleHandle(NULL), MAKEINTRESOURCE(IDC_TRAY_MIC_MUTE));
	TrayMenu = GetSubMenu(hmenu, 0);
	SetMenuDefaultItem(TrayMenu, IDM_MUTE, 0);

	MENUITEMINFO mii;
	ZeroMemory(& mii, sizeof(mii));
	mii.cbSize = sizeof(mii);
	mii.fMask = MIIM_SUBMENU | MIIM_STRING;
	mii.fType = MFT_STRING;
	mii.hSubMenu = DevicesMenu;
	TCHAR _devices_item[1024];
	LoadString(hInstance, IDS_DEVICES, _devices_item, sizeof(_devices_item) / sizeof(_devices_item[0]));
	mii.dwTypeData = (LPTSTR)_devices_item;
	mii.cch = sizeof(_devices_item) / sizeof(TCHAR);
	InsertMenuItem(GetMenu(AppHWnd), 2, TRUE, &mii);
	InsertMenuItem(TrayMenu, 3, TRUE, &mii);
	CheckMenuRadioItem(DevicesMenu, DEVICE_FIRST_ID, DEVICE_LAST_ID, SelectedDevice + DEVICE_FIRST_ID, MF_BYCOMMAND);

	if (!IsUserAnAdmin())
	{
		SetIcon(GetMenu(AppHWnd), IDM_AUTORUN, 106, _T("user32"));
		SetIcon(TrayMenu, IDM_AUTORUN, 106, _T("user32"));
	}
	SetIcon(TrayMenu, IDM_SHOW_MICMUTE, IDI_MIC_MUTE);

	if (SavedVolume == 0)
	{
		SavedVolume = mixer_mic_in.GetVolume();
	}

	if (StartMuted || mixer_mic_in.GetVolume() == 0 || mixer_mic_in.GetMute())
	{
		if (mixer_mic_in.GetVolume() > 0 && SavedVolume != mixer_mic_in.GetVolume())
		{
			SavedVolume = mixer_mic_in.GetVolume();
		}
		if (StartMuted)
		{
			StartMutedToggle(AppHWnd);
		}
		if (mixer_mic_in.GetVolume() > 0 || !mixer_mic_in.GetMute())
		{
			MuteToggle(AppHWnd);
		}
		else
		{
			mixer_mic_in.SetVolume(0);
			mixer_mic_in.SetMute(TRUE);
			IsMuted = true;
			CheckMenuItem(GetMenu(AppHWnd), IDM_MUTE, MF_CHECKED);
			CheckMenuItem(TrayMenu, IDM_MUTE, MF_CHECKED);
		}
	}

	if (ShowNotifications)
	{
		ShowNotificationsToggle(AppHWnd);
	}

	if (SoundSignal)
	{
		SoundSignalToggle(AppHWnd);
	}

	if (Autorun && IsUserAnAdmin())
	{
		AutorunToggle(AppHWnd);
	}

	if (MicMode == MIC_MODE_TRANSMITTER)
	{
		TransmitterToggle(AppHWnd);
	}

	// Main message loop:
	while (GetMessage(&msg, NULL, 0, 0))
	{
		if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	if (!IsMuted && mixer_mic_in.GetVolume() > 0 && SavedVolume != mixer_mic_in.GetVolume())
	{
		SavedVolume = mixer_mic_in.GetVolume();
	}
	WriteIni();

	Shell_NotifyIcon(NIM_DELETE, &nid);

	SetEvent(StopEvent);
	WaitForSingleObject(Thread, INFINITE);
	CloseHandle(Thread);
	CloseHandle(StopEvent);
	CloseHandle(HookEvent);
	UnhookWindowsHookEx(hhook);
	FreeLibrary(hinstDLL);

	FreeLibrary(dsound);

	CloseHandle(SingleControl);

	if (restart_with_admin_rights)
	{
		TCHAR cmd[1024];
		GetModuleFileName(NULL, cmd, sizeof(cmd) / sizeof(cmd[0]));
		SHELLEXECUTEINFO shExInfo = {0};
		shExInfo.cbSize = sizeof(shExInfo);
		shExInfo.fMask = SEE_MASK_DEFAULT;
		shExInfo.hwnd = 0;
		shExInfo.lpVerb = _T("runas");                
		shExInfo.lpFile = cmd;							
		shExInfo.lpParameters = NULL;                  
		shExInfo.lpDirectory = NULL;
		shExInfo.nShow = SW_SHOW;
		shExInfo.hInstApp = 0;  
		ShellExecuteEx(&shExInfo);
	}

	return (int) msg.wParam;
}



//
//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
	WNDCLASSEX wcex;

	wcex.cbSize = sizeof(WNDCLASSEX);

	wcex.style			= CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc	= WndProc;
	wcex.cbClsExtra		= 0;
	wcex.cbWndExtra		= 0;
	wcex.hInstance		= hInstance;
	wcex.hIcon			= IconBlack;
	wcex.hCursor		= LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground	= (HBRUSH)(COLOR_WINDOW+1);
	wcex.lpszMenuName	= MAKEINTRESOURCE(IDC_MIC_MUTE);
	wcex.lpszClassName	= szWindowClass;
	wcex.hIconSm		= IconBlack;

	return RegisterClassEx(&wcex);
}

//
//   FUNCTION: InitInstance(HINSTANCE, int)
//
//   PURPOSE: Saves instance handle and creates main window
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   hInst = hInstance; // Store instance handle in our global variable

   AppHWnd = CreateWindowEx(WS_EX_TOPMOST, szWindowClass, szTitle, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
      CW_USEDEFAULT, CW_USEDEFAULT, 320, 120, NULL, NULL, hInstance, NULL);

   if (!AppHWnd)
   {
      return FALSE;
   }

   ShowWindow(AppHWnd, nCmdShow);
   UpdateWindow(AppHWnd);

   return TRUE;
}

VOID ReadIni(VOID)
{
	TCHAR szPath[MAX_PATH];
	SHGetFolderPath(NULL, 
		CSIDL_LOCAL_APPDATA|CSIDL_FLAG_CREATE, 
		NULL, 
		0, 
		szPath); 
	StringCchCat(szPath, MAX_PATH, _T("\\MicMute"));
	CreateDirectory(szPath, NULL);
	StringCchCat(szPath, MAX_PATH, _T("\\mic_mute.ini"));

	/*
	if (GetFileAttributes(szPath) == INVALID_FILE_ATTRIBUTES)
	{
		DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), AppHWnd, About);
	}
	*/

	TCHAR _str[1024];
	int _count, _key1, _key2, _start_muted, _arun;

	GetPrivateProfileString(_T("Mic_Mute"), _T("ShortCut_Count"), _T("2"), _str, 1024, szPath);
	_stscanf(_str, _T("%i"), &_count);
	GetPrivateProfileString(_T("Mic_Mute"), _T("ShortCut_Key1"), _T("162"), _str, 1024, szPath);
	_stscanf(_str, _T("%i"), &_key1);
	GetPrivateProfileString(_T("Mic_Mute"), _T("ShortCut_Key2"), _T("164"), _str, 1024, szPath);
	_stscanf(_str, _T("%i"), &_key2);
	GetPrivateProfileString(_T("Mic_Mute"), _T("StartMuted"), _T("0"), _str, 1024, szPath);
	_stscanf(_str, _T("%i"), &_start_muted);
	GetPrivateProfileString(_T("Mic_Mute"), _T("Device"), _T("-1"), _str, 1024, szPath);
	_stscanf(_str, _T("%i"), &SelectedDevice);
	GetPrivateProfileString(_T("Mic_Mute"), _T("MicMode"), _T("0"), _str, 1024, szPath);
	_stscanf(_str, _T("%i"), &MicMode);
	GetPrivateProfileString(_T("Mic_Mute"), _T("ShowNotifications"), _T("1"), _str, 1024, szPath);
	_stscanf(_str, _T("%i"), &ShowNotifications);
	GetPrivateProfileString(_T("Mic_Mute"), _T("SoundSignal"), _T("1"), _str, 1024, szPath);
	_stscanf(_str, _T("%i"), &SoundSignal);
	GetPrivateProfileString(_T("Mic_Mute"), _T("Autorun"), _T("0"), _str, 1024, szPath);
	_stscanf(_str, _T("%i"), &_arun);
	GetPrivateProfileString(_T("Mic_Mute"), _T("SavedVolume"), _T("0"), _str, 1024, szPath);
	_stscanf(_str, _T("%i"), &SavedVolume);

	GetPrivateProfileString(_T("Mic_Mute"), _T("MicOnSound"), szMicOnDefault, szMicOnSound, MAX_PATH, szPath);
	GetPrivateProfileString(_T("Mic_Mute"), _T("MicOffSound"), szMicOffDefault, szMicOffSound, MAX_PATH, szPath);

	size_t _len;
	StringCchLength(szMicOnSound, MAX_PATH, & _len);
	if (_len == 0)
	{
		StringCchCopy(szMicOnSound, MAX_PATH, szMicOnDefault);
	}
	StringCchLength(szMicOffSound, MAX_PATH, & _len);
	if (_len == 0)
	{
		StringCchCopy(szMicOffSound, MAX_PATH, szMicOffDefault);
	}

	StartMuted = (_start_muted != 0);
	Autorun = (_arun != 0);
	SetShortcut(_count, _key1, _key2);
}

VOID WriteIni(VOID)
{
	TCHAR szPath[MAX_PATH];
	SHGetFolderPath(NULL, 
		CSIDL_LOCAL_APPDATA|CSIDL_FLAG_CREATE, 
		NULL, 
		0, 
		szPath); 
	StringCchCat(szPath, MAX_PATH, _T("\\MicMute"));
	CreateDirectory(szPath, NULL);
	StringCchCat(szPath, MAX_PATH, _T("\\mic_mute.ini"));

	TCHAR _str[1024];
	int _count, _key1, _key2, _start_muted, _arun;

	_start_muted = (StartMuted == TRUE) ? 1 : 0;
	_arun = (Autorun == TRUE) ? 1 : 0;
	GetShortcut(&_count, &_key1, &_key2);
	_stprintf(_str, _T("%i"), _count);
	WritePrivateProfileString(_T("Mic_Mute"), _T("ShortCut_Count"), _str, szPath);
	_stprintf(_str, _T("%i"), _key1);
	WritePrivateProfileString(_T("Mic_Mute"), _T("ShortCut_Key1"), _str, szPath);
	_stprintf(_str, _T("%i"), _key2);
	WritePrivateProfileString(_T("Mic_Mute"), _T("ShortCut_Key2"), _str, szPath);
	_stprintf(_str, _T("%i"), _start_muted);
	WritePrivateProfileString(_T("Mic_Mute"), _T("StartMuted"), _str, szPath);
	_stprintf(_str, _T("%i"), SelectedDevice);
	WritePrivateProfileString(_T("Mic_Mute"), _T("Device"), _str, szPath);
	_stprintf(_str, _T("%i"), MicMode);
	WritePrivateProfileString(_T("Mic_Mute"), _T("MicMode"), _str, szPath);
	_stprintf(_str, _T("%i"), ShowNotifications);
	WritePrivateProfileString(_T("Mic_Mute"), _T("ShowNotifications"), _str, szPath);
	_stprintf(_str, _T("%i"), SoundSignal);
	WritePrivateProfileString(_T("Mic_Mute"), _T("SoundSignal"), _str, szPath);
	_stprintf(_str, _T("%i"), _arun);
	WritePrivateProfileString(_T("Mic_Mute"), _T("Autorun"), _str, szPath);
	_stprintf(_str, _T("%i"), SavedVolume);
	WritePrivateProfileString(_T("Mic_Mute"), _T("SavedVolume"), _str, szPath);

	WritePrivateProfileString(_T("Mic_Mute"), _T("MicOnSound"), szMicOnSound, szPath);
	WritePrivateProfileString(_T("Mic_Mute"), _T("MicOffSound"), szMicOffSound, szPath);

}

VOID MuteToggle(HWND hWnd)
{
	HMENU menu = GetMenu(hWnd);
	DWORD mute_state;
	DWORD PrevVolume;

	mciSendString(_T("close MicOnSound"), NULL, 0, 0);			
	mciSendString(_T("close MicOffSound"), NULL, 0, 0);			

	NOTIFYICONDATA nid;
	nid.cbSize = sizeof(nid);
	nid.hWnd = AppHWnd;
	nid.uID = 1;
	nid.uFlags = NIF_TIP | NIF_ICON;
	nid.uTimeout = 5000;
	nid.dwInfoFlags = NIIF_NOSOUND;
	if (ShowNotifications)
	{
		nid.uFlags |= NIF_INFO;
		nid.dwInfoFlags |= NIIF_USER;
	}
	TCHAR _tooltip_title[] = _T("MicMute");
	StringCchCopy(nid.szInfoTitle, 64, _tooltip_title);

	FLASHWINFO flash;
	flash.cbSize = sizeof(flash);
	flash.hwnd = hWnd;
	flash.dwTimeout = 0;
	flash.uCount = 0xffffffff;

	mute_state = CheckMenuItem(menu, IDM_MUTE, MF_UNCHECKED);
	if (mute_state == MF_UNCHECKED)
	{
		TCHAR _tooltip_text[1024];
		LoadString(hInst, IDS_MICOFF, _tooltip_text, sizeof(_tooltip_text) / sizeof(_tooltip_text[0]));
		StringCchCopy(nid.szInfo, 265, _tooltip_text);
		TCHAR _tip[1024];
		LoadString(hInst, IDS_MICOFF2, _tip, sizeof(_tip) / sizeof(_tip[0]));
		StringCchCopy(nid.szTip, 128, _tip);
		SetWindowText(hWnd, (LPTSTR)_tip);
		CheckMenuItem(menu, IDM_MUTE, mute_state = MF_CHECKED);
		flash.dwFlags = FLASHW_TRAY;
		IsMuted = true;
		if (SoundSignal)
		{
			size_t _len = 0;
			StringCchLength(szMicOnSound, MAX_PATH, & _len);
			TCHAR _open_str[1024];
			_open_str[0] = _T('');
			StringCchCat(_open_str, 1024, _T("open \""));
			StringCchCat(_open_str, 1024, szMicOffSound);
			StringCchCat(_open_str, 1024, _T("\" type mpegvideo alias MicOffSound"));
			mciSendString(_open_str, NULL, 0, 0); 
			mciSendString(_T("play MicOffSound"), NULL, 0, 0);
		}
		SetIcon(hWnd, IconBlack);
		nid.hIcon = IconBlack;
	}
	else 
	{
		TCHAR _tooltip_text[1024];
		LoadString(hInst, IDS_MICON, _tooltip_text, sizeof(_tooltip_text) / sizeof(_tooltip_text[0]));
		StringCchCopy(nid.szInfo, 256, _tooltip_text);
		TCHAR _tip[1024];
		LoadString(hInst, IDS_MICON2, _tip, sizeof(_tip) / sizeof(_tip[0]));
		StringCchCopy(nid.szTip, 128, _tip);
		SetWindowText(hWnd, (LPTSTR)_tip);
		mute_state = MF_UNCHECKED;
		flash.dwFlags = FLASHW_STOP;
		IsMuted = false;
		if (SoundSignal)
		{
			size_t _len = 0;
			StringCchLength(szMicOnSound, MAX_PATH, & _len);
			TCHAR _open_str[1024];
			_open_str[0] = _T('');
			StringCchCat(_open_str, 1024, _T("open \""));
			StringCchCat(_open_str, 1024, szMicOnSound);
			StringCchCat(_open_str, 1024, _T("\" type mpegvideo alias MicOnSound"));
			mciSendString(_open_str, NULL, 0, 0); 
			mciSendString(_T("play MicOnSound"), NULL, 0, 0);
		}
		SetIcon(hWnd, IconRed);
		nid.hIcon = IconRed;
	}
	CheckMenuItem(TrayMenu, IDM_MUTE, mute_state);

	Shell_NotifyIcon(NIM_MODIFY, &nid);
	FlashWindowEx(&flash);

	PrevVolume = mixer_mic_in.GetVolume();
	mixer_mic_in.SetVolume(mute_state == MF_CHECKED ? 0 : SavedVolume);
	mixer_mic_in.SetMute(mute_state == MF_CHECKED);
	if (PrevVolume > 0)
	{
		SavedVolume = PrevVolume;
	}
	if (SavedVolume == 0)
	{
		SavedVolume = 32768;
	}

	InvalidateRect(hWnd, NULL, TRUE);
}

VOID StartMutedToggle(HWND hWnd)
{
	DWORD _mute_state = 0;
	HMENU menu = GetMenu(hWnd);
	_mute_state = CheckMenuItem(menu, IDM_START_MUTED, MF_UNCHECKED);
	if (_mute_state == MF_UNCHECKED)
	{
		CheckMenuItem(menu, IDM_START_MUTED, _mute_state = MF_CHECKED);
		StartMuted = TRUE;
	}
	else
	{
		_mute_state = MF_UNCHECKED;
		StartMuted = FALSE;
	}
	CheckMenuItem(TrayMenu, IDM_START_MUTED, _mute_state);	
}

VOID ShowNotificationsToggle(HWND hWnd)
{
	DWORD _show_state = 0;
	HMENU menu = GetMenu(hWnd);
	_show_state = CheckMenuItem(menu, IDM_SHOW_NOTIFICATIONS, MF_UNCHECKED);
	if (_show_state == MF_UNCHECKED)
	{
		CheckMenuItem(menu, IDM_SHOW_NOTIFICATIONS, _show_state = MF_CHECKED);
		ShowNotifications = TRUE;
	}
	else
	{
		_show_state = MF_UNCHECKED;
		ShowNotifications = FALSE;
	}
	CheckMenuItem(TrayMenu, IDM_SHOW_NOTIFICATIONS, _show_state);	
}

VOID SoundSignalToggle(HWND hWnd)
{
	DWORD _sound_state = 0;
	HMENU menu = GetMenu(hWnd);
	_sound_state = CheckMenuItem(menu, IDM_SOUND_SIGNAL, MF_UNCHECKED);
	if (_sound_state == MF_UNCHECKED)
	{
		CheckMenuItem(menu, IDM_SOUND_SIGNAL, _sound_state = MF_CHECKED);
		SoundSignal = TRUE;
	}
	else
	{
		_sound_state = MF_UNCHECKED;
		SoundSignal = FALSE;
	}
	CheckMenuItem(TrayMenu, IDM_SOUND_SIGNAL, _sound_state);	
}


VOID AutorunToggle(HWND hWnd)
{
	if (!IsUserAnAdmin())
	{
		restart_with_admin_rights = true;
		PostQuitMessage(0);
		return;
	}

	TCHAR str[1024];

	HKEY hKey;
	RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\Microsoft\\Windows\\CurrentVersion\\Run"), 0, KEY_ALL_ACCESS, & hKey);

	DWORD _arun_state = 0;
	HMENU menu = GetMenu(hWnd);
	_arun_state = CheckMenuItem(menu, IDM_AUTORUN, MF_UNCHECKED);
	if (_arun_state == MF_UNCHECKED)
	{
		CheckMenuItem(menu, IDM_AUTORUN, _arun_state = MF_CHECKED);

		StringCchCopy(str, sizeof(str), _T("/delete /tn MicMute /f"));
		ShellExecute(NULL, _T("open"), _T("schtasks"), str, NULL, SW_HIDE);

		RegDeleteValue(hKey, _T("MicMute")); // for compatibility with 0.1.8.1 and older

/*
		LPCTSTR _cmd = GetCommandLine();
		size_t _len;
		StringCchLength(_cmd, STRSAFE_MAX_CCH, & _len);

		RegSetValueEx(hKey, _T("MicMute"), 0, REG_SZ, (BYTE *)_cmd, (DWORD)(_len * sizeof(_cmd[0]) + sizeof(L'\0')));
*/
		StringCchCopy(str, sizeof(str), _T("/create /sc onlogon /tn MicMute /rl highest /delay 0000:10 /tr "));
		StringCchCat(str, sizeof(str), GetCommandLine());
		ShellExecute(NULL, _T("open"), _T("schtasks"), str, NULL, SW_HIDE);

		Autorun = TRUE;
	}
	else
	{
		_arun_state = MF_UNCHECKED;
		SoundSignal = FALSE;

		RegDeleteValue(hKey, _T("MicMute")); // for compatibility with 0.1.8.1 and older

		StringCchCopy(str, sizeof(str), _T("/delete /tn MicMute /f"));
		ShellExecute(NULL, _T("open"), _T("schtasks"), str, NULL, SW_HIDE);

		Autorun = FALSE;
	}
	CheckMenuItem(TrayMenu, IDM_AUTORUN, _arun_state);	

	RegCloseKey(hKey);
}

VOID ShowTransmitterModeWarning(HWND hWnd)
{
	TCHAR warn[1024];
	LoadString(hInst, IDS_TRANSMWAR, warn, sizeof(warn) / sizeof(warn[0]));
	TCHAR caption[1024];
	LoadString(hInst, IDS_INFOCAPTION, caption, sizeof(caption) / sizeof(caption[0]));
	MessageBox(hWnd, warn, caption, MB_OK | MB_ICONINFORMATION);
}

VOID TransmitterToggle(HWND hWnd)
{
	DWORD trns_state;
	HMENU menu = GetMenu(hWnd);
	trns_state = CheckMenuItem(menu, IDM_TRANSMITTER_MODE, MF_UNCHECKED);
	if (trns_state == MF_UNCHECKED)
	{
		int _count, _key1, _key2;
		GetShortcut(&_count, &_key1, &_key2);
		if (_count != 1)
		{
			ShowTransmitterModeWarning(hWnd);
			return;
		}
		CheckMenuItem(menu, IDM_TRANSMITTER_MODE, trns_state = MF_CHECKED);
		MicMode = MIC_MODE_TRANSMITTER;
		EnableMenuItem(menu, IDM_MUTE, MF_BYCOMMAND | MF_GRAYED);
		EnableMenuItem(TrayMenu, IDM_MUTE, MF_BYCOMMAND | MF_GRAYED);
//		EnableMenuItem(menu, IDM_SETUP_SHORTCUT, MF_BYCOMMAND | MF_GRAYED);
//		EnableMenuItem(TrayMenu, IDM_SETUP_SHORTCUT, MF_BYCOMMAND | MF_GRAYED);
		if (IsMuted == false)
		{
			MuteToggle(hWnd);
		}
	}
	else
	{
		trns_state = MF_UNCHECKED;
		MicMode = MIC_MODE_STANDART;
		EnableMenuItem(menu, IDM_MUTE, MF_BYCOMMAND | MF_ENABLED);
		EnableMenuItem(TrayMenu, IDM_MUTE, MF_BYCOMMAND | MF_ENABLED);
//		EnableMenuItem(menu, IDM_SETUP_SHORTCUT, MF_BYCOMMAND | MF_ENABLED);
//		EnableMenuItem(TrayMenu, IDM_SETUP_SHORTCUT, MF_BYCOMMAND | MF_ENABLED);
	}
	CheckMenuItem(TrayMenu, IDM_TRANSMITTER_MODE, trns_state);	
	SetMode(MicMode);
}

//
//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE:  Processes messages for the main window.
//
//  WM_COMMAND	- process the application menu
//  WM_PAINT	- Paint the main window
//  WM_DESTROY	- post a quit message and return
//
//

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	int wmId, wmEvent;
	PAINTSTRUCT ps;
	POINT pt;

	if (message == TrayMsg)
	{
		switch ((UINT)lParam)
		{
		case WM_LBUTTONDBLCLK:
			ShowWindow(hWnd, SW_SHOWNORMAL);
			break;
		case WM_RBUTTONDOWN:
			GetCursorPos(&pt);
			SetForegroundWindow(hWnd);
			TrackPopupMenu(TrayMenu, 
				TPM_LEFTALIGN, 
				pt.x, pt.y, 0, hWnd, NULL); 
			PostMessage(hWnd, WM_NULL, 0, 0);
			break;
		}
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	switch (message)
	{
	case WM_COMMAND:
		wmId    = LOWORD(wParam);
		wmEvent = HIWORD(wParam);
		// Parse the menu selections:
		if ((wmId >= DEVICE_FIRST_ID) && (wmId <= DEVICE_LAST_ID))
		{
			CheckMenuRadioItem(DevicesMenu, DEVICE_FIRST_ID, DEVICE_LAST_ID, wmId, MF_BYCOMMAND);
			SelectedDevice = wmId - DEVICE_FIRST_ID;
			mixer_mic_in.SelectDevice(SelectedDevice);
		}
		switch (wmId)
		{
		case IDM_ABOUT:
			DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
			break;
		case IDM_EXIT:
			DestroyWindow(hWnd);
			break;
		case IDM_MUTE:
			MuteToggle(hWnd);
			break;
		case IDM_START_MUTED:
			StartMutedToggle(hWnd);
			break;
		case IDM_SHOW_NOTIFICATIONS:
			ShowNotificationsToggle(hWnd);
			break;
		case IDM_SOUND_SIGNAL:
			SoundSignalToggle(hWnd);
			break;
		case IDM_TRANSMITTER_MODE:
			TransmitterToggle(hWnd);
			break;
		case IDM_SHOW_MICMUTE:
			ShowWindow(hWnd, SW_SHOWNORMAL);
			break;
		case IDM_HIDE_MICMUTE:
			ShowWindow(hWnd, SW_HIDE);
			break;
		case IDM_AUTORUN:
			AutorunToggle(hWnd);
			break;
		case IDM_SETUP_SHORTCUT:
			DialogBox(hInst, MAKEINTRESOURCE(IDD_SETUP_SHORTCUT), hWnd, SetupShortcut);
			break;
		case IDM_SELECT_AUDIO_FILES:
			DialogBox(hInst, MAKEINTRESOURCE(IDD_SELECT_AUDIO_FILES), hWnd, SelectAudioFiles);
			break;
		default:
			return DefWindowProc(hWnd, message, wParam, lParam);
		}
		WriteIni();
		break;
	case WM_CLOSE:
		ShowWindow(hWnd, SW_HIDE);
		break;
	case WM_PAINT:
		{
			size_t _len;
			TCHAR _tip[1024];
			if (IsMuted)
			{
				LoadString(hInst, IDS_MICOFF, _tip, sizeof(_tip) / sizeof(_tip[0]));
				StringCchLength(_tip, STRSAFE_MAX_CCH, & _len);
			}
			else
			{
				LoadString(hInst, IDS_MICON, _tip, sizeof(_tip) / sizeof(_tip[0]));
				StringCchLength(_tip, STRSAFE_MAX_CCH, & _len);
			}
			BeginPaint(hWnd, &ps);
			TextOut(GetDC(hWnd), 0, 0, _tip, (int)_len);
			EndPaint(hWnd, &ps);
		}
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	case WM_KEYDOWN:
		break;
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

// Message handler for about box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	UNREFERENCED_PARAMETER(lParam);
	switch (message)
	{
	case WM_INITDIALOG:
		{
			SetWindowText(::GetDlgItem(hDlg, IDC_DONATE), _T("1FNrZr7Y4hx4fpaWRwgHsUL8T2yRKe1Rm6"));
			SendMessage(::GetDlgItem(hDlg, IDC_DONATE), WM_SETFONT, (WPARAM) GetStockObject(SYSTEM_FIXED_FONT), (LPARAM)FALSE);
			return (INT_PTR)TRUE;
		}
		break;
	case WM_COMMAND:
		if (LOWORD(wParam) == IDC_MAIL)
		{
			ShellExecute(hDlg, _T("open"), _T("mailto:mist.poryvaev@gmail.com?subject=MicMute"), NULL, NULL, SW_SHOWNORMAL);
		}
		else
		if (LOWORD(wParam) == IDC_UPDATES)
		{
			ShellExecute(hDlg, _T("open"), _T("https://sourceforge.net/projects/micmute"), NULL, NULL, SW_SHOWNORMAL);
		}
		else
		if (LOWORD(wParam) == IDC_PAYPAL)
		{
			ShellExecute(hDlg, _T("open"), _T("https://www.paypal.com/cgi-bin/webscr?cmd=_s-xclick&hosted_button_id=XTQVLZEHNQ4E8"), NULL, NULL, SW_SHOWNORMAL);
		}
		else
		if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
		{
			EndDialog(hDlg, LOWORD(wParam));
			return (INT_PTR)TRUE;
		}
		break;
	}
	return (INT_PTR)FALSE;
}

INT_PTR CALLBACK SetupShortcut(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{	
	switch (message)
	{
	case WM_INITDIALOG:
		SetEnabled(false);
		prev_code = 0;
		shortcut_edit_proc = (WNDPROC)(size_t)GetWindowLongPtr(GetDlgItem(hDlg, IDC_SHORTCUT), GWLP_WNDPROC);
		SetWindowLongPtr(GetDlgItem(hDlg, IDC_SHORTCUT), GWLP_WNDPROC, (LONG)(LONG_PTR)ShortcutEditProc);
		return (INT_PTR)TRUE;
	case WM_SHOWWINDOW:
		SetFocus(GetDlgItem(hDlg, IDC_SHORTCUT));
		break;
	case WM_COMMAND:
		if ((LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL))
		{
			EndDialog(hDlg, LOWORD(wParam));
			SetEnabled(true);
			return (INT_PTR)TRUE;
		}
		break;
	}

	return (INT_PTR)FALSE;
}

INT_PTR CALLBACK SelectAudioFiles(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{	
	switch (message)
	{
	case WM_INITDIALOG:
		SetEnabled(false);
		return (INT_PTR)TRUE;
	case WM_SHOWWINDOW:
		SetWindowText(GetDlgItem(hDlg, IDC_MIC_ON), szMicOnSound);		
		SetWindowText(GetDlgItem(hDlg, IDC_MIC_OFF), szMicOffSound);		
		break;
	case WM_COMMAND:
		if ((LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL))
		{
			EndDialog(hDlg, LOWORD(wParam));
			SetEnabled(true);
			GetWindowText(GetDlgItem(hDlg, IDC_MIC_ON), szMicOnSound, MAX_PATH);
			GetWindowText(GetDlgItem(hDlg, IDC_MIC_OFF), szMicOffSound, MAX_PATH);

			size_t _len;
			StringCchLength(szMicOnSound, MAX_PATH, & _len);
			if (_len == 0)
			{
				StringCchCopy(szMicOnSound, MAX_PATH, szMicOnDefault);
			}
			StringCchLength(szMicOffSound, MAX_PATH, & _len);
			if (_len == 0)
			{
				StringCchCopy(szMicOffSound, MAX_PATH, szMicOffDefault);
			}

			return (INT_PTR)TRUE;
		}
		else
		if (LOWORD(wParam) == ID_MIC_ON_BROWSE || LOWORD(wParam) == ID_MIC_OFF_BROWSE)
		{
			TCHAR _filename[MAX_PATH], _filefilter[1024], _title[1024];
			LoadString(hInst, IDS_AUDIO_FILE_FILTER, _filefilter, sizeof(_filefilter) / sizeof(_filefilter[0]));
			for (int i = 0; i < sizeof(_filefilter) / sizeof(_filefilter[0]); i++)
			{
				if (_filefilter[i] == _T('\n'))
				{
					_filefilter[i] = _T('\0');
				}
			}
			LoadString(hInst, IDS_SELECT_AUDIO_FILE, _title, sizeof(_title) / sizeof(_title[0]));
			GetWindowText(GetDlgItem(hDlg, LOWORD(wParam) == ID_MIC_ON_BROWSE ? IDC_MIC_ON : IDC_MIC_OFF), _filename, MAX_PATH);
			OPENFILENAME ofn;
			ZeroMemory(& ofn, sizeof(OPENFILENAME));
			ofn.lStructSize = sizeof(OPENFILENAME);
			ofn.hwndOwner = hDlg;
			ofn.lpstrFilter = _filefilter;
			ofn.lpstrFile = _filename;
			ofn.nMaxFile = MAX_PATH;
			ofn.lpstrTitle = _title;
			ofn.Flags = OFN_FILEMUSTEXIST;
			if (GetOpenFileName(& ofn))
			{
				SetWindowText(GetDlgItem(hDlg, LOWORD(wParam) == ID_MIC_ON_BROWSE ? IDC_MIC_ON : IDC_MIC_OFF), _filename);	
			}
		}
		break;
	}

	return (INT_PTR)FALSE;
}

TCHAR * KeyToName(UINT _code)
{
	static TCHAR _str[32];
	_str[0] = TCHAR(0);
	_str[1] = TCHAR(0);
	switch (_code)
	{
	case VK_BACK: return _T("Back"); 
	case VK_TAB: return _T("Tab");
	case VK_CLEAR: return _T("Clear");
	case VK_RETURN: return _T("Return");

	case VK_SHIFT: return _T("Shift");
	case VK_CONTROL: return _T("Ctrl");
	case VK_MENU: return _T("Alt");
	case VK_PAUSE: return _T("Pause");
	case VK_CAPITAL: return _T("Caps");

	case VK_ESCAPE: return _T("Esc");

	case VK_SPACE: return _T("Space");
	case VK_PRIOR: return _T("Page Up");
	case VK_NEXT : return _T("Page Down");
	case VK_END: return _T("End");
	case VK_HOME: return _T("Home");
	case VK_LEFT: return _T("Left");
	case VK_UP : return _T("Up");
	case VK_RIGHT: return _T("Right");
	case VK_DOWN: return _T("Down");
	case VK_SELECT: return _T("Select");
	case VK_PRINT: return _T("Print");
	case VK_EXECUTE: return _T("Exec");
	case VK_SNAPSHOT: return _T("Snap");
	case VK_INSERT: return _T("Ins");
	case VK_DELETE : return _T("Del");
	case VK_HELP: return _T("Help");

	case VK_LWIN: return _T("Left Win");
	case VK_RWIN: return _T("Right Win");
	case VK_APPS : return _T("Apps");

	case VK_SLEEP: return _T("Sleep");

	case VK_NUMPAD0: return _T("Num 0");
	case VK_NUMPAD1: return _T("Num 1");
	case VK_NUMPAD2: return _T("Num 2");
	case VK_NUMPAD3: return _T("Num 3");
	case VK_NUMPAD4: return _T("Num 4");
	case VK_NUMPAD5: return _T("Num 5");
	case VK_NUMPAD6: return _T("Num 6");
	case VK_NUMPAD7: return _T("Num 7");
	case VK_NUMPAD8 : return _T("Num 8");
	case VK_NUMPAD9 : return _T("Num 9");
	case VK_MULTIPLY : return _T("Num *");
	case VK_ADD     : return _T("Num +");
	case VK_SEPARATOR : return _T("Num .");
	case VK_SUBTRACT: return _T("Num -");
	case VK_DECIMAL : return _T("Num Dec");
	case VK_DIVIDE : return _T("Num /");
	case VK_F1    : return _T("F1");
	case VK_F2    : return _T("F2");
	case VK_F3    : return _T("F3");
	case VK_F4    : return _T("F4");
	case VK_F5    : return _T("F5");
	case VK_F6    : return _T("F6");
	case VK_F7    : return _T("F7");
	case VK_F8     : return _T("F8");
	case VK_F9     : return _T("F9");
	case VK_F10   : return _T("F10");
	case VK_F11   : return _T("F11");
	case VK_F12   : return _T("F12");
	case VK_F13   : return _T("F13");
	case VK_F14   : return _T("F14");
	case VK_F15   : return _T("F15");
	case VK_F16   : return _T("F16");
	case VK_F17   : return _T("F17");
	case VK_F18    : return _T("F18");
	case VK_F19    : return _T("F19");
	case VK_F20    : return _T("F20");
	case VK_F21    : return _T("F21");
	case VK_F22    : return _T("F22");
	case VK_F23    : return _T("F23");
	case VK_F24   : return _T("F24");

	case VK_NUMLOCK  : return _T("NumLock");
	case VK_SCROLL   : return _T("ScrollLock");

	case VK_LSHIFT: return _T("Left Shift");
	case VK_RSHIFT: return _T("Right Shift");
	case VK_LCONTROL: return _T("Left Ctrl");
	case VK_RCONTROL: return _T("Right Ctrl");
	case VK_LMENU: return _T("Left Alt");
	case VK_RMENU: return _T("Right Alt");

	default: 
		{
			BYTE ks[256];
			ZeroMemory(ks, sizeof(ks));
			ks[0] = 1;
			UINT _scan_code = MapVirtualKey(_code, MAPVK_VK_TO_VSC);
#ifdef _UNICODE
			ToUnicode(_code, _scan_code, ks, _str, 32, 0);
#else
			ToAscii(_code, _scan_code,  ks, _str, 0);
#endif
			if (_str[0] == _T('\0'))
			{
				_stprintf(_str, _T("Code %i"), _code);
			}
		}
	}
	return _str;
}

LRESULT CALLBACK ShortcutEditProc(HWND hEdit, UINT message, WPARAM wParam, LPARAM lParam)
{
	UINT _code = 0, _scan_code = 0;
	static TCHAR _str[1024] = _T("");
	TCHAR _key1[32] = _T(""), _key2[32] = _T("");
	UINT _count = 0;
	switch (message)
	{
	case WM_PAINT:
		if (prev_code == 0)
		{
			GetShortcut((int *)&_count, (int *)&prev_code, (int *)&_code);
			if (_count == 1)
			{
				StringCchCopy(_key1, 32, KeyToName(prev_code));
				StringCbPrintf(_str, sizeof(_str), _T("%s"), _key1);
			}
			else
			{
				StringCchCopy(_key1, 32, KeyToName(prev_code));
				StringCchCopy(_key2, 32, KeyToName(_code));
				StringCbPrintf(_str, sizeof(_str), _T("%s + %s"), _key1, _key2);
			}
			SetWindowText(hEdit, _str);
			prev_code = 0;
		}
		break;
	case WM_SYSKEYDOWN:
	case WM_KEYDOWN:
		_code = GetVkCode();
		if ((prev_code == 0) || (prev_code == _code) || (MicMode == MIC_MODE_TRANSMITTER))
		{
			StringCchCopy(_key1, 32, KeyToName(_code));
			StringCbPrintf(_str, sizeof(_str), _T("%s"), _key1);
			SetShortcut(1, _code, 0);
		}
		else 
		{
			StringCchCopy(_key1, 32, KeyToName(prev_code));
			StringCchCopy(_key2, 32, KeyToName(_code));
			StringCbPrintf(_str, sizeof(_str), _T("%s + %s"), _key1, _key2);
			SetShortcut(2, prev_code, _code);
		}
		SetWindowText(hEdit, _str);
		prev_code = _code;
		switch (_code)
		{
			case VK_MENU:
				return 0;
		}
		break;
	}
	return CallWindowProc(shortcut_edit_proc, hEdit, message, wParam, lParam);
}

VOID SetIcon(HWND hwnd, HICON hIcon)
{
	if (hIcon) 
	{
		SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
		SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
		SendMessage(GetWindow(hwnd, GW_OWNER), WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
		SendMessage(GetWindow(hwnd, GW_OWNER), WM_SETICON, ICON_BIG, (LPARAM)hIcon);
	}
}

HBITMAP	Icon2Bitmap(HICON icon, UINT sz)
{
	ICONINFO ii;
	GetIconInfo(icon, &ii);

	HDC hdcMem1 = CreateCompatibleDC(NULL);
	HDC hdcMem2 = CreateCompatibleDC(NULL);
	SelectObject(hdcMem1, ii.hbmColor);
	SelectObject(hdcMem2, ii.hbmMask);
	SetBkColor(hdcMem1, 0xffffff);
	BitBlt(hdcMem1, 0, 0, sz, sz, hdcMem2, 0, 0, SRCINVERT);
	DeleteDC(hdcMem1);
	DeleteDC(hdcMem2);

	return ii.hbmColor;
}

VOID SetIcon(HMENU menu, UINT menu_id, UINT icon_id, LPCTSTR module)
{
	HICON icon = (HICON)LoadImage(GetModuleHandle(module), MAKEINTRESOURCE(icon_id), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CXSMICON), 0);
	SetMenuItemBitmaps(menu, menu_id, MF_BITMAP | MF_BYCOMMAND, Icon2Bitmap(icon, GetSystemMetrics(SM_CXSMICON)), Icon2Bitmap(icon, GetSystemMetrics(SM_CXSMICON)));
}